/* Player update packages (lx_update.h): find, check, unpack, swap. */
#define _GNU_SOURCE
#include "lx_update.h"

#include "mbedtls/sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PKG_MAX_FILES 512
#define MANIFEST_MAX (64 * 1024)
#define TAR_BLOCK 512

typedef struct {
    char path[256];
    unsigned char sha[32];
    bool seen;
} pkg_file_t;

typedef struct {
    char board[32];
    char version[64];
    int nfiles;
    pkg_file_t files[PKG_MAX_FILES];
    long long total;
} manifest_t;

static void set_err(char * err, size_t size, const char * fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char * err, size_t size, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, size, fmt, ap);
    va_end(ap);
}

/* ---- finding ---------------------------------------------------------- */

static bool update_dir(const lx_config_t * cfg, char * out, size_t size) {
    int n = snprintf(out, size, "%s/.mellowtone/update", cfg->sd_root);
    return n > 0 && (size_t) n < size;
}

static bool version_chars_ok(const char * v) {
    if (!*v || strlen(v) >= 64) return false;
    for (; *v; v++)
        if (!isalnum((unsigned char) *v) && *v != '.' && *v != '-' && *v != '_' && *v != '+') return false;
    return true;
}

bool lx_pkg_find(const lx_config_t * cfg, lx_pkg_ref_t * out) {
    char dir[PATH_MAX], prefix[64];
    if (!update_dir(cfg, dir, sizeof(dir))) return false;
    snprintf(prefix, sizeof(prefix), "mellowtone-%s-", cfg->board);
    size_t plen = strlen(prefix);
    DIR * d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent * e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= plen + 4 || strncmp(e->d_name, prefix, plen) != 0 || strcmp(e->d_name + n - 4, ".tar") != 0) continue;
        char ver[64];
        size_t vlen = n - plen - 4;
        if (vlen >= sizeof(ver)) continue;
        memcpy(ver, e->d_name + plen, vlen);
        ver[vlen] = '\0';
        if (!version_chars_ok(ver)) continue;
        char path[PATH_MAX];
        if (!lx_join(path, sizeof(path), dir, e->d_name)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!found || lx_version_cmp(ver, out->version) > 0) {
            snprintf(out->path, sizeof(out->path), "%s", path);
            snprintf(out->version, sizeof(out->version), "%s", ver);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* ---- tar ---------------------------------------------------------------- */

typedef struct {
    char name[260]; /* prefix (155) + '/' + name (100) */
    char type;
    long long size;
    unsigned mode;
} tar_entry_t;

static long long octal(const unsigned char * p, size_t n) {
    long long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static bool read_full(int fd, void * buf, size_t n) {
    unsigned char * p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t) r;
    }
    return true;
}

/* 1: entry, 0: end of archive, -1: error (err set). */
static int tar_next(int fd, tar_entry_t * e, char * err, size_t err_size) {
    unsigned char h[TAR_BLOCK];
    if (!read_full(fd, h, sizeof(h))) {
        set_err(err, err_size, "archive truncated");
        return -1;
    }
    bool zero = true;
    for (size_t i = 0; i < sizeof(h); i++)
        if (h[i]) { zero = false; break; }
    if (zero) return 0;
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof(h); i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if ((long long) sum != octal(h + 148, 8)) {
        set_err(err, err_size, "archive corrupt (header checksum)");
        return -1;
    }
    char name[101], prefix[156];
    memcpy(name, h, 100);
    name[100] = '\0';
    prefix[0] = '\0';
    if (memcmp(h + 257, "ustar", 5) == 0) {
        memcpy(prefix, h + 345, 155);
        prefix[155] = '\0';
    }
    if (prefix[0]) snprintf(e->name, sizeof(e->name), "%s/%s", prefix, name);
    else snprintf(e->name, sizeof(e->name), "%s", name);
    e->type = (char) h[156];
    e->size = octal(h + 124, 12);
    e->mode = (unsigned) octal(h + 100, 8);
    return 1;
}

/* Skips an entry's data (size bytes, padded to the block). */
static bool tar_skip(int fd, long long size) {
    long long padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    return lseek(fd, (off_t) padded, SEEK_CUR) >= 0;
}

/* After reading an entry's size bytes: skips the padding to the block. */
static bool tar_skip_padding(int fd, long long size) {
    long long pad = (TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK;
    return pad == 0 || lseek(fd, (off_t) pad, SEEK_CUR) >= 0;
}

/* "./a/b" -> "a/b"; refuses absolute paths, "..", empty components. */
static bool clean_path(char * name) {
    while (name[0] == '.' && name[1] == '/') memmove(name, name + 2, strlen(name + 2) + 1);
    size_t n = strlen(name);
    while (n && name[n - 1] == '/') name[--n] = '\0';
    if (!n || name[0] == '/') return false;
    for (char * c = name;;) {
        char * slash = strchr(c, '/');
        size_t len = slash ? (size_t) (slash - c) : strlen(c);
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.')) return false;
        if (!slash) break;
        c = slash + 1;
    }
    return true;
}

/* ---- manifest ---------------------------------------------------------- */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_manifest(char * text, manifest_t * m, char * err, size_t err_size) {
    memset(m, 0, sizeof(*m));
    bool header = false;
    for (char * line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        line[strcspn(line, "\r")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        if (strcmp(line, "mellowtone-package 1") == 0) header = true;
        else if (strncmp(line, "board ", 6) == 0) snprintf(m->board, sizeof(m->board), "%s", line + 6);
        else if (strncmp(line, "version ", 8) == 0) snprintf(m->version, sizeof(m->version), "%s", line + 8);
        else if (strncmp(line, "file ", 5) == 0) {
            char * hex = line + 5;
            char * sp = strchr(hex, ' ');
            if (!sp || sp - hex != 64 || m->nfiles >= PKG_MAX_FILES) {
                set_err(err, err_size, "manifest: bad file line");
                return false;
            }
            pkg_file_t * f = &m->files[m->nfiles];
            for (int i = 0; i < 32; i++) {
                int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
                if (hi < 0 || lo < 0) {
                    set_err(err, err_size, "manifest: bad checksum");
                    return false;
                }
                f->sha[i] = (unsigned char) (hi << 4 | lo);
            }
            snprintf(f->path, sizeof(f->path), "%s", sp + 1);
            if (!clean_path(f->path) || strcmp(f->path, LX_MANIFEST) == 0) {
                set_err(err, err_size, "manifest: bad path %s", sp + 1);
                return false;
            }
            for (int i = 0; i < m->nfiles; i++) {
                if (strcmp(m->files[i].path, f->path) == 0) {
                    set_err(err, err_size, "manifest: %s listed twice", f->path);
                    return false;
                }
            }
            m->nfiles++;
        }
    }
    if (!header) {
        set_err(err, err_size, "not a Mellowtone package (manifest header)");
        return false;
    }
    if (!m->board[0] || !m->version[0]) {
        set_err(err, err_size, "manifest: board or version missing");
        return false;
    }
    return true;
}

static pkg_file_t * manifest_file(manifest_t * m, const char * path) {
    for (int i = 0; i < m->nfiles; i++)
        if (strcmp(m->files[i].path, path) == 0) return &m->files[i];
    return NULL;
}

/* Pass 1: the manifest and the entry sizes. */
static bool read_manifest(int fd, manifest_t * m, char * err, size_t err_size) {
    tar_entry_t e;
    int r;
    while ((r = tar_next(fd, &e, err, err_size)) == 1) {
        if (!clean_path(e.name)) {
            set_err(err, err_size, "unsafe path in archive: %s", e.name);
            return false;
        }
        if ((e.type == '0' || e.type == '\0') && strcmp(e.name, LX_MANIFEST) == 0) {
            if (e.size <= 0 || e.size > MANIFEST_MAX) {
                set_err(err, err_size, "manifest too large");
                return false;
            }
            char * text = malloc((size_t) e.size + 1);
            if (!text) return false;
            bool ok = read_full(fd, text, (size_t) e.size);
            text[ok ? e.size : 0] = '\0';
            ok = ok && tar_skip_padding(fd, e.size) &&
                 parse_manifest(text, m, err, err_size);
            free(text);
            if (!ok) {
                if (!err[0]) set_err(err, err_size, "archive truncated");
                return false;
            }
            continue;
        }
        if (e.type != '0' && e.type != '\0' && e.type != '5') {
            set_err(err, err_size, "unsupported entry type '%c' for %s (ustar files and directories only)",
                    e.type ? e.type : '0', e.name);
            return false;
        }
        if (e.type != '5') m->total += e.size;
        if (!tar_skip(fd, e.size)) {
            set_err(err, err_size, "archive truncated");
            return false;
        }
    }
    if (r < 0) return false;
    if (!m->board[0]) {
        set_err(err, err_size, "no MANIFEST in the package");
        return false;
    }
    return true;
}

static bool mkdir_parents(const char * root, const char * rel) {
    char path[PATH_MAX];
    if (!lx_join(path, sizeof(path), root, rel)) return false;
    char * slash = strrchr(path, '/');
    if (!slash) return true;
    *slash = '\0';
    return lx_mkdir_p(path);
}

static void fsync_tree(const char * dir) {
    DIR * d = opendir(dir);
    if (!d) return;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char child[PATH_MAX];
        if (lx_join(child, sizeof(child), dir, e->d_name) && lx_is_dir(child)) fsync_tree(child);
    }
    closedir(d);
    lx_fsync_dir(dir);
}

typedef struct {
    lx_progress_fn fn;
    void * ctx;
    long long done, total;
    int last;
} progress_t;

static void progress_add(progress_t * p, long long n, const char * what) {
    p->done += n;
    int pct = p->total > 0 ? (int) (p->done * 100 / p->total) : 100;
    if (pct > 100) pct = 100;
    if (p->fn && pct != p->last) p->fn(p->ctx, pct, what);
    p->last = pct;
}

/* Pass 2: unpack into dest, every file checked against the manifest. With
 * dest NULL nothing is written: the whole package is checked before the
 * installed version is touched. */
static bool unpack(int fd, manifest_t * m, const char * dest, progress_t * prog, char * err, size_t err_size) {
    tar_entry_t e;
    int r;
    static unsigned char buf[64 * 1024];
    while ((r = tar_next(fd, &e, err, err_size)) == 1) {
        if (!clean_path(e.name)) {
            set_err(err, err_size, "unsafe path in archive: %s", e.name);
            return false;
        }
        char path[PATH_MAX];
        if (dest && !lx_join(path, sizeof(path), dest, e.name)) return false;
        if (e.type == '5') {
            if (dest && !lx_mkdir_p(path)) {
                set_err(err, err_size, "cannot create %s", e.name);
                return false;
            }
            continue;
        }
        bool is_manifest = strcmp(e.name, LX_MANIFEST) == 0;
        pkg_file_t * f = is_manifest ? NULL : manifest_file(m, e.name);
        if (!is_manifest && !f) {
            set_err(err, err_size, "%s is not in the manifest", e.name);
            return false;
        }
        if (f && f->seen) {
            set_err(err, err_size, "%s is in the archive twice", e.name);
            return false;
        }
        if (dest && !mkdir_parents(dest, e.name)) {
            set_err(err, err_size, "cannot create the directory for %s", e.name);
            return false;
        }
        int out = dest ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (e.mode & 0111) ? 0755 : 0644) : -2;
        if (out == -1) {
            set_err(err, err_size, "cannot write %s: %s", e.name, strerror(errno));
            return false;
        }
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        long long left = e.size;
        bool ok = true;
        while (left > 0 && ok) {
            size_t n = left > (long long) sizeof(buf) ? sizeof(buf) : (size_t) left;
            if (!read_full(fd, buf, n)) {
                set_err(err, err_size, "archive truncated in %s", e.name);
                ok = false;
                break;
            }
            mbedtls_sha256_update(&sha, buf, n);
            for (size_t off = 0; out >= 0 && off < n && ok;) {
                ssize_t w = write(out, buf + off, n - off);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    set_err(err, err_size, "write failed for %s: %s", e.name, w < 0 ? strerror(errno) : "short");
                    ok = false;
                }
                else off += (size_t) w;
            }
            left -= (long long) n;
            if (prog) progress_add(prog, (long long) n, e.name);
        }
        unsigned char digest[32];
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        if (out >= 0 && fsync(out) != 0) ok = false;
        if (out >= 0 && close(out) != 0) ok = false;
        if (!ok) {
            if (!err[0]) set_err(err, err_size, "write failed for %s", e.name);
            return false;
        }
        if (!tar_skip_padding(fd, e.size)) {
            set_err(err, err_size, "archive truncated");
            return false;
        }
        if (f) {
            if (memcmp(digest, f->sha, sizeof(digest)) != 0) {
                set_err(err, err_size, "checksum mismatch: %s", e.name);
                return false;
            }
            f->seen = true;
        }
        if (dest && lx_step_hook && !lx_step_hook("extract")) {
            set_err(err, err_size, "stopped (test)");
            return false;
        }
    }
    if (r < 0) return false;
    for (int i = 0; i < m->nfiles; i++) {
        if (!m->files[i].seen) {
            set_err(err, err_size, "%s is missing from the archive", m->files[i].path);
            return false;
        }
    }
    return true;
}

bool lx_pkg_install(const lx_config_t * cfg, const lx_pkg_ref_t * pkg, lx_progress_fn progress, void * ctx,
                    char * err, size_t err_size) {
    err[0] = '\0';
    const char * file = strrchr(pkg->path, '/');
    file = file ? file + 1 : pkg->path;
    lx_update_log(cfg, "install %s: start", file);
    static manifest_t m;
    bool ok = false;
    char partial[PATH_MAX], next[PATH_MAX];
    int fd = open(pkg->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, err_size, "cannot open the package: %s", strerror(errno));
        goto out;
    }
    if (!read_manifest(fd, &m, err, err_size)) goto out;
    if (strcmp(m.board, cfg->board) != 0) {
        set_err(err, err_size, "package is for %s, this is %s", m.board, cfg->board);
        goto out;
    }
    if (strcmp(m.version, pkg->version) != 0) {
        set_err(err, err_size, "version %s does not match the file name (%s)", m.version, pkg->version);
        goto out;
    }
    if (!manifest_file(&m, LX_PLAYER_EXE)) {
        set_err(err, err_size, "package has no %s", LX_PLAYER_EXE);
        goto out;
    }
    /* Every checksum before anything changes. */
    if (lseek(fd, 0, SEEK_SET) != 0 || !unpack(fd, &m, NULL, NULL, err, err_size)) goto out;
    for (int i = 0; i < m.nfiles; i++) m.files[i].seen = false;
    if (!lx_mkdir_p(cfg->slots_root)) {
        set_err(err, err_size, "cannot create %s", cfg->slots_root);
        goto out;
    }
    if (!lx_join(partial, sizeof(partial), cfg->slots_root, "next.partial") ||
        !lx_join(next, sizeof(next), cfg->slots_root, "next"))
        goto out;
    /* Leftovers of an earlier attempt (lx_slots_recover() normally removed them). */
    if (!lx_rm_rf(partial) || !lx_rm_rf(next)) {
        set_err(err, err_size, "cannot prepare %s", partial);
        goto out;
    }
    if (progress) progress(ctx, 0, "Saving the installed version to the card");
    if (!lx_slots_install_begin(cfg, m.version, m.total, err, err_size)) {
        /* Stopped after the journal (a test's power cut): the next start finishes it. */
        if (!err[0]) set_err(err, err_size, "stopped");
        goto out;
    }
    if (!lx_mkdir_p(partial)) {
        set_err(err, err_size, "cannot create %s", partial);
        goto failed;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) goto failed;
    progress_t prog = { progress, ctx, 0, m.total, -1 };
    if (!unpack(fd, &m, partial, &prog, err, err_size)) goto failed;
    char player[PATH_MAX];
    if (lx_join(player, sizeof(player), partial, LX_PLAYER_EXE)) chmod(player, 0755);
    fsync_tree(partial);
    if (rename(partial, next) != 0) {
        set_err(err, err_size, "cannot finish the unpack: %s", strerror(errno));
        goto failed;
    }
    lx_fsync_dir(cfg->slots_root);
    if (lx_step_hook && !lx_step_hook("next-ready")) goto out;
    if (!lx_slots_commit_next(cfg, m.version)) {
        if (!lx_step_hook) set_err(err, err_size, "making the new version current failed");
        goto out;
    }
    if (unlink(pkg->path) != 0) {
        lx_update_log(cfg, "install %s: could not delete the package: %s", file, strerror(errno));
    } else {
        char dir[PATH_MAX];
        if (update_dir(cfg, dir, sizeof(dir))) lx_fsync_dir(dir);
    }
    ok = true;
    goto out;
failed:
    /* The installed version comes back from the card (not after a test's
     * power cut: lx_slots_recover() is what the next start runs). */
    if (!lx_step_hook) {
        char why[200];
        lx_rm_rf(partial);
        if (lx_slots_install_abort(cfg, why, sizeof(why)))
            lx_update_log(cfg, "install %s: the previous version was restored from the card", file);
        else
            lx_update_log(cfg, "install %s: restoring the previous version failed: %s", file, why);
    }
out:
    if (fd >= 0) close(fd);
    if (ok) lx_update_log(cfg, "install %s: version %s installed (%d files)", file, m.version, m.nfiles);
    else lx_update_log(cfg, "install %s: refused or failed: %s", file, err[0] ? err : "error");
    return ok;
}
