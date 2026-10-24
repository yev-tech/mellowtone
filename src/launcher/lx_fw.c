/* Firmware images on the card (lx_update.h): which one recovery would use,
 * what is in it, and whether every part's md5 is right. The image is read
 * as ISO 9660 directly (plain names, Rock Ridge NM or Joliet), never
 * mounted. */
#define _GNU_SOURCE
#include "lx_update.h"
#include "board_launcher.h"

#include "mbedtls/md5.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ISO_SECTOR 2048

/* ---- ISO 9660 ------------------------------------------------------------ */

typedef struct {
    FILE * f;
    uint32_t root_lba, root_len;
    bool joliet;
} iso_t;

typedef struct {
    char name[256];
    uint32_t lba, len;
    bool dir;
} iso_entry_t;

static uint32_t le32(const unsigned char * p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static bool iso_read(iso_t * iso, uint64_t off, void * buf, size_t n) {
    return fseeko(iso->f, (off_t) off, SEEK_SET) == 0 && fread(buf, 1, n, iso->f) == n;
}

static bool iso_open(iso_t * iso, const char * path) {
    memset(iso, 0, sizeof(*iso));
    iso->f = fopen(path, "rbe");
    if (!iso->f) return false;
    unsigned char vd[ISO_SECTOR];
    bool primary = false;
    for (uint32_t s = 16; s < 64; s++) {
        if (!iso_read(iso, (uint64_t) s * ISO_SECTOR, vd, sizeof(vd)) || memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 255) break;
        bool joliet = vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E');
        if ((vd[0] == 1 && !primary && !iso->joliet) || joliet) {
            iso->root_lba = le32(vd + 156 + 2);
            iso->root_len = le32(vd + 156 + 10);
            if (vd[0] == 1) primary = true;
            if (joliet) iso->joliet = true;
        }
    }
    if (!primary && !iso->joliet) {
        fclose(iso->f);
        iso->f = NULL;
        return false;
    }
    return true;
}

static void iso_close(iso_t * iso) {
    if (iso->f) fclose(iso->f);
    iso->f = NULL;
}

/* The record's name: Joliet UCS-2, else a Rock Ridge NM entry, else the
 * ISO name without ";1" and a trailing dot. */
static void record_name(const iso_t * iso, const unsigned char * r, char * out, size_t size) {
    unsigned len = r[0], nlen = r[32];
    const unsigned char * name = r + 33;
    size_t o = 0;
    out[0] = '\0';
    if (iso->joliet) {
        for (unsigned i = 0; i + 1 < nlen && o + 1 < size; i += 2) {
            unsigned c = (unsigned) name[i] << 8 | name[i + 1];
            out[o++] = c < 0x80 ? (char) c : '?';
        }
        out[o] = '\0';
    } else {
        /* System use area: after the name, padded to an even offset. */
        unsigned su = 33 + nlen + ((nlen & 1) ? 0 : 1);
        bool rr = false;
        while (su + 4 <= len) {
            unsigned elen = r[su + 2];
            if (elen < 4 || su + elen > len) break;
            if (r[su] == 'N' && r[su + 1] == 'M' && elen >= 5) {
                for (unsigned i = 5; i < elen && o + 1 < size; i++) out[o++] = (char) r[su + i];
                out[o] = '\0';
                rr = true;
                if (!(r[su + 4] & 1)) break; /* no CONTINUE flag */
            }
            su += elen;
        }
        if (rr) return;
        for (unsigned i = 0; i < nlen && o + 1 < size; i++) out[o++] = (char) name[i];
        out[o] = '\0';
    }
    char * semi = strchr(out, ';');
    if (semi) *semi = '\0';
    size_t n = strlen(out);
    if (n && out[n - 1] == '.') out[n - 1] = '\0';
}

typedef bool (*iso_visit_fn)(const iso_entry_t * e, void * ctx); /* false: stop */

static bool iso_list(iso_t * iso, uint32_t lba, uint32_t len, iso_visit_fn fn, void * ctx) {
    unsigned char sec[ISO_SECTOR];
    for (uint32_t off = 0; off < len; off += ISO_SECTOR) {
        if (!iso_read(iso, (uint64_t) lba * ISO_SECTOR + off, sec, sizeof(sec))) return false;
        for (unsigned p = 0; p < ISO_SECTOR;) {
            unsigned rlen = sec[p];
            if (rlen == 0) break; /* rest of the sector is padding */
            if (rlen < 34 || p + rlen > ISO_SECTOR) return false;
            const unsigned char * r = sec + p;
            unsigned nlen = r[32];
            bool self_or_parent = nlen == 1 && (r[33] == 0 || r[33] == 1);
            if (!self_or_parent) {
                iso_entry_t e;
                record_name(iso, r, e.name, sizeof(e.name));
                e.lba = le32(r + 2);
                e.len = le32(r + 10);
                e.dir = (r[25] & 2) != 0;
                if (!fn(&e, ctx)) return true;
            }
            p += rlen;
        }
    }
    return true;
}

typedef struct {
    const char * want;
    iso_entry_t found;
    bool ok;
} find_ctx_t;

static bool find_visit(const iso_entry_t * e, void * c) {
    find_ctx_t * f = c;
    if (strcasecmp(e->name, f->want) != 0) return true;
    f->found = *e;
    f->ok = true;
    return false;
}

/* "a/b/c", case-insensitive (the R3 Pro's image has upper-case 8.3 names
 * that UPDATE.TXT spells in lower case; the loop mount folds them). */
static bool iso_find(iso_t * iso, const char * path, iso_entry_t * out) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    uint32_t lba = iso->root_lba, len = iso->root_len;
    for (char * c = strtok(buf, "/"); c; c = strtok(NULL, "/")) {
        find_ctx_t f = { .want = c };
        if (!iso_list(iso, lba, len, find_visit, &f) || !f.ok) return false;
        *out = f.found;
        lba = f.found.lba;
        len = f.found.len;
    }
    return true;
}

static bool iso_read_text(iso_t * iso, const char * path, char * buf, size_t size) {
    iso_entry_t e;
    if (!iso_find(iso, path, &e) || e.dir) return false;
    size_t n = e.len < size - 1 ? e.len : size - 1;
    if (!iso_read(iso, (uint64_t) e.lba * ISO_SECTOR, buf, n)) return false;
    buf[n] = '\0';
    return true;
}

/* ---- finding ------------------------------------------------------------ */

static bool is_regular(const char * path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool lx_fw_find(const lx_config_t * cfg, char * path, size_t size) {
    char name[96];
    snprintf(name, sizeof(name), "%s.upt", cfg->board);
    if (lx_join(path, size, cfg->sd_root, name) && is_regular(path)) return true;
    for (int i = 0; cfg->fw_names && cfg->fw_names[i]; i++)
        if (lx_join(path, size, cfg->sd_root, cfg->fw_names[i]) && is_regular(path)) return true;
    if (!cfg->fw_any_upt) return false;
    DIR * d = opendir(cfg->sd_root);
    if (!d) return false;
    bool found = false;
    struct dirent * e;
    char best[256] = "";
    while ((e = readdir(d))) {
        const char * ext = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !ext || strcasecmp(ext, ".upt") != 0) continue;
        char p[PATH_MAX];
        if (!lx_join(p, sizeof(p), cfg->sd_root, e->d_name) || !is_regular(p)) continue;
        /* Several: the same one every time (name order). */
        if (!found || strcmp(e->d_name, best) < 0) snprintf(best, sizeof(best), "%s", e->d_name);
        found = true;
    }
    closedir(d);
    return found && lx_join(path, size, cfg->sd_root, best);
}

/* ---- R3 Pro: UPDATE.TXT / VERSION.TXT ------------------------------------- */

/* Sections "label={" ... "}" with key=value lines. Calls fn per section end. */
typedef void (*section_fn)(const char * label, char keys[][2][128], int n, void * ctx);

static char * trim(char * s) {
    while (isspace((unsigned char) *s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char) s[n - 1])) s[--n] = '\0';
    return s;
}

static void parse_sections(char * text, section_fn fn, void * ctx) {
    char label[64] = "";
    char keys[12][2][128];
    int n = 0;
    bool in = false;
    for (char * line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char * t = trim(line);
        size_t len = strlen(t);
        if (!in && len >= 2 && strcmp(t + len - 2, "={") == 0) {
            t[len - 2] = '\0';
            snprintf(label, sizeof(label), "%s", trim(t));
            n = 0;
            in = true;
        } else if (in && strcmp(t, "}") == 0) {
            fn(label, keys, n, ctx);
            in = false;
        } else if (in && n < 12) {
            char * eq = strchr(t, '=');
            if (!eq) continue;
            *eq = '\0';
            snprintf(keys[n][0], sizeof(keys[n][0]), "%s", trim(t));
            snprintf(keys[n][1], sizeof(keys[n][1]), "%s", trim(eq + 1));
            n++;
        }
    }
}

static const char * key(char keys[][2][128], int n, const char * k) {
    for (int i = 0; i < n; i++)
        if (strcmp(keys[i][0], k) == 0) return keys[i][1];
    return NULL;
}

static void update_section(const char * label, char keys[][2][128], int n, void * ctx) {
    lx_fw_info_t * info = ctx;
    const char * name = key(keys, n, "name");
    if (strcmp(label, "version") == 0) {
        if (name) snprintf(info->product, sizeof(info->product), "%s", name);
        const char * ver = key(keys, n, "ver");
        if (ver) snprintf(info->version, sizeof(info->version), "%s", ver);
        return;
    }
    const char * file = key(keys, n, "file_path");
    const char * md5 = key(keys, n, "md5");
    if (!file || !md5 || info->nparts >= LX_FW_MAX_PARTS) return;
    typeof(info->parts[0]) * p = &info->parts[info->nparts++];
    snprintf(p->name, sizeof(p->name), "%s", name ? name : label);
    /* file_path is relative to recoveryd's mount point ".../autoupdate". */
    const char * rel = strncmp(file, "autoupdate/", 11) == 0 ? file + 11 : file;
    snprintf(p->file, sizeof(p->file), "%s", rel);
    snprintf(p->md5, sizeof(p->md5), "%s", md5);
}

static bool inspect_update_txt(const lx_config_t * cfg, iso_t * iso, lx_fw_info_t * info) {
    char text[8192];
    if (!iso_read_text(iso, "UPDATE.TXT", text, sizeof(text))) {
        snprintf(info->error, sizeof(info->error), "no UPDATE.TXT in the image");
        return false;
    }
    parse_sections(text, update_section, info);
    if (iso_read_text(iso, "VERSION.TXT", text, sizeof(text))) parse_sections(text, update_section, info);
    if (info->nparts == 0) {
        snprintf(info->error, sizeof(info->error), "UPDATE.TXT lists no parts");
        return false;
    }
    for (int i = 0; i < info->nparts; i++) {
        iso_entry_t e;
        if (!iso_find(iso, info->parts[i].file, &e) || e.dir) {
            snprintf(info->error, sizeof(info->error), "part %s: %s missing", info->parts[i].name, info->parts[i].file);
            return false;
        }
        info->parts[i].size = e.len;
        /* Writing the boot loader can brick the device beyond the card's reach:
         * never from here. */
        if (strcasecmp(info->parts[i].name, "bootloader") == 0 || strcasecmp(info->parts[i].name, "uboot") == 0) {
            snprintf(info->error, sizeof(info->error), "the image writes the boot loader; refused");
            return false;
        }
        if (strcasecmp(info->parts[i].name, "userfs") == 0)
            snprintf(info->warning, sizeof(info->warning), "This image also erases the data partition.");
    }
    if (cfg->fw_product) {
        if (!info->product[0]) {
            snprintf(info->warning, sizeof(info->warning), "The image does not name its model.");
        } else if (strcasecmp(info->product, cfg->fw_product) != 0) {
            snprintf(info->error, sizeof(info->error), "image is for %s, this is %s", info->product, cfg->fw_product);
            return false;
        }
    }
    return true;
}

/* ---- X1600: ota_v0/ota_update.in, chunked images ----------------------------- */

/* boards/common/repack_upt.sh names the board in mellowtone.in ("board=r1"): the
 * X1600 boards share one image format, so nothing else tells them apart.
 * Older images have no marker and are let through. */
static bool check_ota_board(const lx_config_t * cfg, iso_t * iso, lx_fw_info_t * info) {
    char text[512];
    if (!iso_read_text(iso, LX_FW_BOARD_MARKER, text, sizeof(text))) {
        lx_log("firmware %s: no board marker, not checked against %s", info->file, cfg->board);
        return true;
    }
    for (char * line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char * t = trim(line);
        if (strncmp(t, "board=", 6) != 0) continue;
        const char * board = trim(t + 6);
        snprintf(info->product, sizeof(info->product), "%s", board);
        if (strcmp(board, cfg->board) != 0) {
            snprintf(info->error, sizeof(info->error), "image is for %s, this is %s", board, cfg->board);
            return false;
        }
        return true;
    }
    lx_log("firmware %s: %s names no board, not checked", info->file, LX_FW_BOARD_MARKER);
    return true;
}

static bool inspect_ota(const lx_config_t * cfg, iso_t * iso, lx_fw_info_t * info) {
    if (!check_ota_board(cfg, iso, info)) return false;
    char text[4096];
    if (!iso_read_text(iso, "ota_v0/ota_update.in", text, sizeof(text))) {
        snprintf(info->error, sizeof(info->error), "no ota_v0/ota_update.in in the image");
        return false;
    }
    typeof(info->parts[0]) * p = NULL;
    for (char * line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char * t = trim(line);
        char * eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * k = t, * v = eq + 1;
        if (strcmp(k, "ota_version") == 0) {
            snprintf(info->version, sizeof(info->version), "ota %s", v);
        } else if (strcmp(k, "img_type") == 0) {
            if (info->nparts >= LX_FW_MAX_PARTS) break;
            p = &info->parts[info->nparts++];
            snprintf(p->name, sizeof(p->name), "%s", v);
        } else if (p && strcmp(k, "img_name") == 0) {
            snprintf(p->file, sizeof(p->file), "%s", v);
        } else if (p && strcmp(k, "img_size") == 0) {
            p->size = atoll(v);
        } else if (p && strcmp(k, "img_md5") == 0) {
            snprintf(p->md5, sizeof(p->md5), "%s", v);
        }
    }
    if (info->nparts == 0) {
        snprintf(info->error, sizeof(info->error), "ota_update.in lists no images");
        return false;
    }
    for (int i = 0; i < info->nparts; i++) {
        if (!info->parts[i].file[0] || strlen(info->parts[i].md5) != 32) {
            snprintf(info->error, sizeof(info->error), "ota_update.in: image %d incomplete", i + 1);
            return false;
        }
    }
    return true;
}

bool lx_fw_inspect(const lx_config_t * cfg, const char * image, lx_fw_info_t * info) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", image); /* image may be info->path */
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", path);
    const char * name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(info->file, sizeof(info->file), "%.127s", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(info->error, sizeof(info->error), "cannot read the image");
        return false;
    }
    info->size = st.st_size;
    iso_t iso;
    if (!iso_open(&iso, path)) {
        snprintf(info->error, sizeof(info->error), "not a firmware image (no ISO 9660 volume)");
        return false;
    }
    bool ok = cfg->fw_format == BOARD_FW_INGENIC_OTA ? inspect_ota(cfg, &iso, info) : inspect_update_txt(cfg, &iso, info);
    iso_close(&iso);
    return ok;
}

/* ---- md5 ------------------------------------------------------------------- */

typedef struct {
    lx_progress_fn fn;
    void * ctx;
    long long done, total;
    int last;
    const char * what;
} fw_progress_t;

static bool md5_extent(iso_t * iso, uint32_t lba, uint64_t len, mbedtls_md5_context * md5, fw_progress_t * prog) {
    static unsigned char buf[64 * 1024];
    if (fseeko(iso->f, (off_t) lba * ISO_SECTOR, SEEK_SET) != 0) return false;
    while (len > 0) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : (size_t) len;
        if (fread(buf, 1, n, iso->f) != n) return false;
        mbedtls_md5_update(md5, buf, n);
        len -= n;
        prog->done += (long long) n;
        int pct = prog->total > 0 ? (int) (prog->done * 100 / prog->total) : 100;
        if (prog->fn && pct != prog->last) prog->fn(prog->ctx, pct > 100 ? 100 : pct, prog->what);
        prog->last = pct;
    }
    return true;
}

static void hex(const unsigned char * d, char out[33]) {
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

typedef struct {
    const char * prefix; /* "<img_name>." */
    iso_entry_t chunks[512];
    int n;
} chunk_ctx_t;

static bool chunk_visit(const iso_entry_t * e, void * c) {
    chunk_ctx_t * k = c;
    size_t pl = strlen(k->prefix);
    if (e->dir || strncmp(e->name, k->prefix, pl) != 0) return true;
    const char * idx = e->name + pl;
    if (!isdigit((unsigned char) idx[0]) || k->n >= 512) return true;
    k->chunks[k->n++] = *e;
    return true;
}

static int chunk_cmp(const void * a, const void * b) {
    return strcmp(((const iso_entry_t *) a)->name, ((const iso_entry_t *) b)->name);
}

bool lx_fw_verify(lx_fw_info_t * info, int fw_format, lx_progress_fn progress, void * ctx) {
    iso_t iso;
    if (!iso_open(&iso, info->path)) {
        snprintf(info->error, sizeof(info->error), "cannot read the image");
        return false;
    }
    fw_progress_t prog = { progress, ctx, 0, 0, -1, NULL };
    for (int i = 0; i < info->nparts; i++) prog.total += info->parts[i].size;
    bool ok = true;
    static chunk_ctx_t chunks;
    for (int i = 0; i < info->nparts && ok; i++) {
        typeof(info->parts[0]) * p = &info->parts[i];
        prog.what = p->name;
        mbedtls_md5_context md5;
        mbedtls_md5_init(&md5);
        mbedtls_md5_starts(&md5);
        bool read_ok = true;
        long long size = 0;
        if (fw_format == BOARD_FW_INGENIC_OTA) {
            char prefix[140];
            snprintf(prefix, sizeof(prefix), "%s.", p->file);
            memset(&chunks, 0, sizeof(chunks));
            chunks.prefix = prefix;
            iso_entry_t dir;
            read_ok = iso_find(&iso, "ota_v0", &dir) && dir.dir && iso_list(&iso, dir.lba, dir.len, chunk_visit, &chunks) &&
                      chunks.n > 0;
            qsort(chunks.chunks, (size_t) chunks.n, sizeof(chunks.chunks[0]), chunk_cmp);
            for (int c = 0; c < chunks.n && read_ok; c++) {
                read_ok = md5_extent(&iso, chunks.chunks[c].lba, chunks.chunks[c].len, &md5, &prog);
                size += chunks.chunks[c].len;
            }
        } else {
            iso_entry_t e = { .len = 0 };
            read_ok = iso_find(&iso, p->file, &e) && !e.dir && md5_extent(&iso, e.lba, e.len, &md5, &prog);
            size = e.len;
        }
        unsigned char digest[16];
        char got[33];
        mbedtls_md5_finish(&md5, digest);
        mbedtls_md5_free(&md5);
        hex(digest, got);
        if (!read_ok) {
            snprintf(info->error, sizeof(info->error), "part %s: cannot read it", p->name);
            ok = false;
        } else if (strcasecmp(got, p->md5) != 0 || (p->size && size != p->size)) {
            snprintf(info->error, sizeof(info->error), "part %s: md5 mismatch", p->name);
            ok = false;
        } else {
            p->md5_ok = true;
        }
    }
    iso_close(&iso);
    return ok;
}
