/* Safe FLAC / MP3 tag writer -- see tag_writer.h for the contract, the
 * safety model and the backup file format. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "tag_writer.h"
#include "branding.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TW_IO_BUF          (64u * 1024u)
#define TW_VALUE_MAX       (64u * 1024u)
#define TW_VC_MAX          (8u * 1024u * 1024u)  /* largest VORBIS_COMMENT block we load */
#define TW_FRAME_READ_MAX  (1u * 1024u * 1024u)  /* largest ID3 frame we decode */
#define TW_FLAC_MAX_BLOCKS 1024
#define TW_FLAC_NEW_PAD    4096u
#define TW_ID3_MAX_FRAMES  4096
#define TW_ID3_NEW_PAD     1024u
#define TW_UFID_MAX        64u
#define TW_BACKUP_HDR      48u

static const uint8_t TW_BACKUP_MAGIC[8] = { 'C', 'T', 'W', 'B', 'A', 'K', '\r', '\n' };

enum { TW_FMT_NONE = 0, TW_FMT_FLAC = 1, TW_FMT_MP3 = 2 };

#ifdef TAG_WRITER_TEST_HOOKS
static int tw_fail_stage;
void tag_writer_test_fail_at(int stage) { tw_fail_stage = stage; }
#define TW_INJECT(stage) (tw_fail_stage == (stage))
#else
#define TW_INJECT(stage) 0
#endif

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */

enum {
    K_ALBUM, K_ALBUMARTIST, K_ARTIST, K_TITLE, K_TRACKNUMBER, K_TRACKTOTAL,
    K_DISCNUMBER, K_DISCTOTAL, K_DATE, K_GENRE, K_MB_ALBUMID, K_MB_ALBUMARTISTID,
    K_MB_ARTISTID, K_MB_TRACKID, K_MB_RELEASETRACKID,
    K_CUESHEET, /* FLAC only: an embedded CUE sheet (multi-line text) */
    K_COUNT
};

typedef struct {
    const char * name;
    const char * alias[2]; /* Vorbis-comment aliases removed when the key is written */
} tw_key_t;

static const tw_key_t tw_keys[K_COUNT] = {
    [K_ALBUM] = { "ALBUM", { NULL, NULL } },
    [K_ALBUMARTIST] = { "ALBUMARTIST", { "ALBUM ARTIST", "ALBUM_ARTIST" } },
    [K_ARTIST] = { "ARTIST", { NULL, NULL } },
    [K_TITLE] = { "TITLE", { NULL, NULL } },
    [K_TRACKNUMBER] = { "TRACKNUMBER", { NULL, NULL } },
    [K_TRACKTOTAL] = { "TRACKTOTAL", { "TOTALTRACKS", NULL } },
    [K_DISCNUMBER] = { "DISCNUMBER", { "DISKNUMBER", NULL } },
    [K_DISCTOTAL] = { "DISCTOTAL", { "TOTALDISCS", NULL } },
    [K_DATE] = { "DATE", { NULL, NULL } },
    [K_GENRE] = { "GENRE", { NULL, NULL } },
    [K_MB_ALBUMID] = { "MUSICBRAINZ_ALBUMID", { NULL, NULL } },
    [K_MB_ALBUMARTISTID] = { "MUSICBRAINZ_ALBUMARTISTID", { NULL, NULL } },
    [K_MB_ARTISTID] = { "MUSICBRAINZ_ARTISTID", { NULL, NULL } },
    [K_MB_TRACKID] = { "MUSICBRAINZ_TRACKID", { NULL, NULL } },
    [K_MB_RELEASETRACKID] = { "MUSICBRAINZ_RELEASETRACKID", { NULL, NULL } },
    [K_CUESHEET] = { "CUESHEET", { NULL, NULL } },
};

typedef struct {
    bool present;
    const char * value; /* NULL = remove */
} tw_req_t;

/* ------------------------------------------------------------------------ */
/* Small utilities                                                           */

#define FNV_INIT 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv64(uint64_t h, const uint8_t * p, size_t n) {
    while (n--) {
        h ^= *p++;
        h *= FNV_PRIME;
    }
    return h;
}

static uint32_t rd_be24(const uint8_t * b) { return ((uint32_t) b[0] << 16) | ((uint32_t) b[1] << 8) | b[2]; }
static uint32_t rd_be32(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3];
}
static uint32_t rd_le32(const uint8_t * b) {
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
}
static uint64_t rd_le64(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}
static bool syncsafe_ok(const uint8_t * b) { return ((b[0] | b[1] | b[2] | b[3]) & 0x80) == 0; }
static uint32_t rd_syncsafe(const uint8_t * b) {
    return ((uint32_t) b[0] << 21) | ((uint32_t) b[1] << 14) | ((uint32_t) b[2] << 7) | b[3];
}
static void wr_le32(uint8_t * b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t) (v >> (8 * i));
}
static void wr_le64(uint8_t * b, uint64_t v) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t) (v >> (8 * i));
}

static bool pread_all(int fd, void * buf, size_t n, uint64_t off) {
    uint8_t * p = buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, (off_t) off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= (size_t) r;
        off += (uint64_t) r;
    }
    return true;
}

static bool write_all(int fd, const void * buf, size_t n) {
    const uint8_t * p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= (size_t) r;
    }
    return true;
}

/* Growable byte buffer with a sticky failure flag. */
typedef struct {
    uint8_t * p;
    size_t len, cap;
    bool fail;
} dbuf_t;

static void db_free(dbuf_t * d) {
    free(d->p);
    memset(d, 0, sizeof(*d));
}

static bool db_reserve(dbuf_t * d, size_t add) {
    if (d->fail) return false;
    if (add > SIZE_MAX / 2 - d->len) { d->fail = true; return false; }
    if (d->len + add <= d->cap) return true;
    size_t cap = d->cap ? d->cap : 256;
    while (cap < d->len + add) cap *= 2;
    uint8_t * np = realloc(d->p, cap);
    if (!np) { d->fail = true; return false; }
    d->p = np;
    d->cap = cap;
    return true;
}

static void db_put(dbuf_t * d, const void * s, size_t n) {
    if (n == 0 || !db_reserve(d, n)) return;
    memcpy(d->p + d->len, s, n);
    d->len += n;
}
static void db_u8(dbuf_t * d, uint8_t v) { db_put(d, &v, 1); }
static void db_le32(dbuf_t * d, uint32_t v) {
    uint8_t b[4];
    wr_le32(b, v);
    db_put(d, b, 4);
}
static void db_str(dbuf_t * d, const char * s) { db_put(d, s, strlen(s)); }
/* NUL-terminates without counting the NUL in len. */
static bool db_cstr(dbuf_t * d) {
    if (!db_reserve(d, 1)) return false;
    d->p[d->len] = 0;
    return true;
}

/* Strict UTF-8 validation (no overlongs, surrogates or > U+10FFFF). */
static bool utf8_valid(const uint8_t * s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return false;
        if (n - i - 1 < need) return false;
        for (size_t k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += need + 1;
    }
    return true;
}

static void utf8_put_cp(dbuf_t * d, uint32_t cp) {
    uint8_t b[4];
    if (cp < 0x80) { b[0] = (uint8_t) cp; db_put(d, b, 1); }
    else if (cp < 0x800) {
        b[0] = (uint8_t) (0xC0 | (cp >> 6)); b[1] = (uint8_t) (0x80 | (cp & 0x3F));
        db_put(d, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (uint8_t) (0xE0 | (cp >> 12)); b[1] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
        b[2] = (uint8_t) (0x80 | (cp & 0x3F));
        db_put(d, b, 3);
    } else {
        b[0] = (uint8_t) (0xF0 | (cp >> 18)); b[1] = (uint8_t) (0x80 | ((cp >> 12) & 0x3F));
        b[2] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t) (0x80 | (cp & 0x3F));
        db_put(d, b, 4);
    }
}

/* Valid UTF-8 -> UTF-16LE (no BOM). */
static void utf8_to_utf16le(dbuf_t * d, const char * s) {
    const uint8_t * p = (const uint8_t *) s;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if (p[0] < 0xE0) { cp = ((uint32_t) (p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (p[0] < 0xF0) {
            cp = ((uint32_t) (p[0] & 0x0F) << 12) | ((uint32_t) (p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            cp = ((uint32_t) (p[0] & 0x07) << 18) | ((uint32_t) (p[1] & 0x3F) << 12) |
                 ((uint32_t) (p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        }
        uint8_t b[4];
        if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t) (0xD800 | (v >> 10)), lo = (uint16_t) (0xDC00 | (v & 0x3FF));
            b[0] = (uint8_t) hi; b[1] = (uint8_t) (hi >> 8); b[2] = (uint8_t) lo; b[3] = (uint8_t) (lo >> 8);
            db_put(d, b, 4);
        } else {
            b[0] = (uint8_t) cp; b[1] = (uint8_t) (cp >> 8);
            db_put(d, b, 2);
        }
    }
}

static int format_of(const char * path) {
    if (!path) return TW_FMT_NONE;
    const char * slash = strrchr(path, '/');
    const char * base = slash ? slash + 1 : path;
    const char * dot = strrchr(base, '.');
    if (!dot || dot == base) return TW_FMT_NONE;
    if (strcasecmp(dot, ".flac") == 0) return TW_FMT_FLAC;
    if (strcasecmp(dot, ".mp3") == 0) return TW_FMT_MP3;
    return TW_FMT_NONE;
}

bool tag_writer_supports(const char * path) { return format_of(path) != TW_FMT_NONE; }

const char * tag_write_result_name(tag_write_result_t r) {
    switch (r) {
        case TAG_WRITE_OK: return "TAG_WRITE_OK";
        case TAG_WRITE_UNSUPPORTED: return "TAG_WRITE_UNSUPPORTED";
        case TAG_WRITE_PARSE_ERROR: return "TAG_WRITE_PARSE_ERROR";
        case TAG_WRITE_IO_ERROR: return "TAG_WRITE_IO_ERROR";
        case TAG_WRITE_VERIFY_FAILED: return "TAG_WRITE_VERIFY_FAILED";
        case TAG_WRITE_BAD_ARGS: return "TAG_WRITE_BAD_ARGS";
        case TAG_WRITE_BACKUP_FAILED: return "TAG_WRITE_BACKUP_FAILED";
    }
    return "TAG_WRITE_UNKNOWN";
}

static tag_write_result_t normalize_fields(const tag_field_t * fields, int count, tw_req_t req[K_COUNT]) {
    memset(req, 0, sizeof(tw_req_t) * K_COUNT);
    if (count <= 0 || !fields) return TAG_WRITE_BAD_ARGS;
    for (int i = 0; i < count; i++) {
        if (!fields[i].key) return TAG_WRITE_BAD_ARGS;
        int k = -1;
        for (int j = 0; j < K_COUNT; j++)
            if (strcasecmp(fields[i].key, tw_keys[j].name) == 0) { k = j; break; }
        if (k < 0 || req[k].present) return TAG_WRITE_BAD_ARGS;
        const char * v = fields[i].value;
        if (v && !v[0]) v = NULL;
        if (v) {
            size_t n = strlen(v);
            if (n > TW_VALUE_MAX || !utf8_valid((const uint8_t *) v, n)) return TAG_WRITE_BAD_ARGS;
        }
        req[k].present = true;
        req[k].value = v;
    }
    return TAG_WRITE_OK;
}

/* ------------------------------------------------------------------------ */
/* Output: temp file writer with a running hash                              */

typedef struct {
    int fd;
    uint64_t written;
    uint64_t hash;
    uint8_t * buf; /* TW_IO_BUF bytes, shared copy buffer */
} tw_out_t;

static bool out_put(tw_out_t * o, const void * p, size_t n) {
    if (!write_all(o->fd, p, n)) return false;
    o->hash = fnv64(o->hash, p, n);
    o->written += n;
    return true;
}

/* Copies [off, off+len) of src into the output; *src_hash (if given) is
 * updated with the bytes read. */
static tag_write_result_t out_copy(tw_out_t * o, int src, uint64_t off, uint64_t len, uint64_t * src_hash) {
    while (len > 0) {
        size_t n = len < TW_IO_BUF ? (size_t) len : TW_IO_BUF;
        if (!pread_all(src, o->buf, n, off)) return TAG_WRITE_IO_ERROR;
        if (src_hash) *src_hash = fnv64(*src_hash, o->buf, n);
        if (TW_INJECT(1) && o->written > 0) return TAG_WRITE_IO_ERROR;
        if (!out_put(o, o->buf, n)) return TAG_WRITE_IO_ERROR;
        off += n;
        len -= n;
    }
    return TAG_WRITE_OK;
}

/* ------------------------------------------------------------------------ */
/* Region sinks: the same region emitter either writes the new region to the
 * temp file (OUT), compares it with the original file's bytes at the same
 * offsets to find the changed span (CMP), or captures one span (CAP).     */

enum { SINK_OUT, SINK_CMP, SINK_CAP };

/* Changed byte ranges of the tag region (in-place mode). Differences closer
 * than TW_SPAN_GAP bytes are joined; at most TW_MAX_SPANS are written. */
#define TW_MAX_SPANS 16
#define TW_SPAN_GAP 512u
typedef struct {
    uint64_t off, len;
} tw_span_t;
typedef struct {
    int n;
    tw_span_t s[TW_MAX_SPANS * 4];
} span_list_t;

static uint64_t spans_total(const span_list_t * L) {
    uint64_t t = 0;
    for (int i = 0; i < L->n; i++) t += L->s[i].len;
    return t;
}

/* Joins the closest neighbours until at most TW_MAX_SPANS remain. */
static void spans_limit(span_list_t * L) {
    while (L->n > TW_MAX_SPANS) {
        int best = 0;
        uint64_t gap = UINT64_MAX;
        for (int i = 0; i + 1 < L->n; i++) {
            uint64_t g = L->s[i + 1].off - (L->s[i].off + L->s[i].len);
            if (g < gap) { gap = g; best = i; }
        }
        L->s[best].len = L->s[best + 1].off + L->s[best + 1].len - L->s[best].off;
        memmove(&L->s[best + 1], &L->s[best + 2], sizeof(tw_span_t) * (size_t) (L->n - best - 2));
        L->n--;
    }
}

typedef struct {
    int kind;
    tw_out_t * o;     /* OUT */
    int src;          /* CMP/CAP: the original file */
    uint64_t pos;     /* offset in the region being produced */
    uint8_t * a, * b; /* CMP scratch, TW_IO_BUF each */
    span_list_t * spans; /* CMP result (positions only ever increase) */
    uint64_t cap_off; /* CAP: span to capture into cap */
    uint64_t cap_len;
    uint8_t * cap;
} sink_t;

static void cmp_mark(sink_t * s, uint64_t at) {
    span_list_t * L = s->spans;
    tw_span_t * t = L->n > 0 ? &L->s[L->n - 1] : NULL;
    if (t && (at < t->off + t->len + TW_SPAN_GAP || L->n == (int) (sizeof(L->s) / sizeof(L->s[0])))) {
        if (at + 1 > t->off + t->len) t->len = at + 1 - t->off;
        return;
    }
    L->s[L->n].off = at;
    L->s[L->n].len = 1;
    L->n++;
}

static tag_write_result_t sink_put(sink_t * s, const void * p, size_t n) {
    const uint8_t * q = p;
    if (s->kind == SINK_OUT) {
        if (!out_put(s->o, p, n)) return TAG_WRITE_IO_ERROR;
    } else if (s->kind == SINK_CMP) {
        size_t done = 0;
        while (done < n) {
            size_t k = n - done < TW_IO_BUF ? n - done : TW_IO_BUF;
            if (!pread_all(s->src, s->a, k, s->pos + done)) return TAG_WRITE_IO_ERROR;
            for (size_t i = 0; i < k; i++)
                if (s->a[i] != q[done + i]) cmp_mark(s, s->pos + done + i);
            done += k;
        }
    } else {
        uint64_t lo = s->pos > s->cap_off ? s->pos : s->cap_off;
        uint64_t hi = s->pos + n < s->cap_off + s->cap_len ? s->pos + n : s->cap_off + s->cap_len;
        if (lo < hi) memcpy(s->cap + (lo - s->cap_off), q + (lo - s->pos), (size_t) (hi - lo));
    }
    s->pos += n;
    return TAG_WRITE_OK;
}

static tag_write_result_t sink_copy(sink_t * s, uint64_t src_off, uint64_t len) {
    if (s->kind == SINK_OUT) {
        tag_write_result_t r = out_copy(s->o, s->src, src_off, len, NULL);
        if (r != TAG_WRITE_OK) return r;
    } else if (s->kind == SINK_CMP) {
        if (src_off != s->pos) { /* same offset: identical by construction */
            uint64_t done = 0;
            while (done < len) {
                size_t k = len - done < TW_IO_BUF ? (size_t) (len - done) : TW_IO_BUF;
                if (!pread_all(s->src, s->a, k, src_off + done) || !pread_all(s->src, s->b, k, s->pos + done))
                    return TAG_WRITE_IO_ERROR;
                for (size_t i = 0; i < k; i++)
                    if (s->a[i] != s->b[i]) cmp_mark(s, s->pos + done + i);
                done += k;
            }
        }
    } else {
        uint64_t lo = s->pos > s->cap_off ? s->pos : s->cap_off;
        uint64_t hi = s->pos + len < s->cap_off + s->cap_len ? s->pos + len : s->cap_off + s->cap_len;
        if (lo < hi && !pread_all(s->src, s->cap + (lo - s->cap_off), (size_t) (hi - lo), src_off + (lo - s->pos)))
            return TAG_WRITE_IO_ERROR;
    }
    s->pos += len;
    return TAG_WRITE_OK;
}

/* ------------------------------------------------------------------------ */
/* FLAC                                                                      */

typedef struct {
    uint8_t type;
    uint32_t len;
    uint64_t body_off;
} flac_blk_t;

typedef struct {
    uint64_t magic_off; /* offset of "fLaC" (== length of a leading ID3v2 prefix) */
    uint64_t audio_off;
    int nblk;
    int vc; /* index of the VORBIS_COMMENT block or -1 */
    flac_blk_t blk[TW_FLAC_MAX_BLOCKS];
} flac_info_t;

static tag_write_result_t flac_parse(int fd, uint64_t size, flac_info_t * fi) {
    uint64_t pos = 0;
    uint8_t h[10];
    for (int n = 0;; n++) {
        if (size < 10 || pos > size - 10) return TAG_WRITE_PARSE_ERROR;
        if (!pread_all(fd, h, 10, pos)) return TAG_WRITE_IO_ERROR;
        if (memcmp(h, "ID3", 3) != 0) break;
        if (n >= 8 || !syncsafe_ok(h + 6)) return TAG_WRITE_PARSE_ERROR;
        pos += 10u + rd_syncsafe(h + 6) + ((h[5] & 0x10) ? 10u : 0u);
    }
    if (memcmp(h, "fLaC", 4) != 0) return TAG_WRITE_PARSE_ERROR;
    fi->magic_off = pos;
    pos += 4;
    fi->nblk = 0;
    fi->vc = -1;
    bool last = false;
    while (!last) {
        if (fi->nblk >= TW_FLAC_MAX_BLOCKS) return TAG_WRITE_UNSUPPORTED;
        if (pos > size || size - pos < 4) return TAG_WRITE_PARSE_ERROR;
        if (!pread_all(fd, h, 4, pos)) return TAG_WRITE_IO_ERROR;
        last = (h[0] & 0x80) != 0;
        uint8_t type = h[0] & 0x7F;
        uint32_t len = rd_be24(h + 1);
        if (type == 127) return TAG_WRITE_PARSE_ERROR;
        if (fi->nblk == 0 && (type != 0 || len != 34)) return TAG_WRITE_PARSE_ERROR;
        if (fi->nblk > 0 && type == 0) return TAG_WRITE_PARSE_ERROR;
        if (len > size - pos - 4) return TAG_WRITE_PARSE_ERROR;
        if (type == 4) {
            if (fi->vc >= 0) return TAG_WRITE_UNSUPPORTED; /* two comment blocks: ambiguous */
            fi->vc = fi->nblk;
        }
        fi->blk[fi->nblk].type = type;
        fi->blk[fi->nblk].len = len;
        fi->blk[fi->nblk].body_off = pos + 4;
        fi->nblk++;
        pos += 4u + len;
    }
    fi->audio_off = pos;
    if (pos < size) {
        /* The first audio frame must follow: frame sync 0xFFF8/0xFFF9. */
        if (size - pos < 2 || !pread_all(fd, h, 2, pos)) return TAG_WRITE_PARSE_ERROR;
        if (h[0] != 0xFF || (h[1] & 0xFE) != 0xF8) return TAG_WRITE_PARSE_ERROR;
    }
    return TAG_WRITE_OK;
}

typedef struct {
    const uint8_t * p;
    uint32_t len;
} vc_item_t;

typedef struct {
    const uint8_t * vendor;
    uint32_t vendor_len;
    uint32_t count;
    vc_item_t * items;
} vc_t;

static bool vc_parse(const uint8_t * b, uint32_t n, vc_t * vc) {
    memset(vc, 0, sizeof(*vc));
    if (n < 8) return false;
    uint32_t vl = rd_le32(b);
    if (vl > n - 8) return false;
    uint32_t pos = 4 + vl;
    uint32_t count = rd_le32(b + pos);
    pos += 4;
    if (count > (n - pos) / 4) return false;
    vc->vendor = b + 4;
    vc->vendor_len = vl;
    vc->items = calloc(count ? count : 1, sizeof(vc_item_t));
    if (!vc->items) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (n - pos < 4) { free(vc->items); vc->items = NULL; return false; }
        uint32_t l = rd_le32(b + pos);
        pos += 4;
        if (l > n - pos) { free(vc->items); vc->items = NULL; return false; }
        vc->items[i].p = b + pos;
        vc->items[i].len = l;
        pos += l;
    }
    if (pos != n) { free(vc->items); vc->items = NULL; return false; } /* trailing bytes: refuse rather than drop */
    vc->count = count;
    return true;
}

static bool key_eq(const uint8_t * k, size_t klen, const char * name) {
    size_t n = strlen(name);
    return n == klen && strncasecmp((const char *) k, name, n) == 0;
}

/* Which requested key (canonical or alias) a "KEY=value" comment belongs to. */
static int vc_item_key(const vc_item_t * it) {
    const uint8_t * eq = memchr(it->p, '=', it->len);
    if (!eq) return -1;
    size_t klen = (size_t) (eq - it->p);
    for (int k = 0; k < K_COUNT; k++) {
        if (key_eq(it->p, klen, tw_keys[k].name)) return k;
        for (int a = 0; a < 2; a++)
            if (tw_keys[k].alias[a] && key_eq(it->p, klen, tw_keys[k].alias[a])) return k;
    }
    return -1;
}

static void vc_put_item(dbuf_t * d, int k, const char * value) {
    size_t kl = strlen(tw_keys[k].name), vl = strlen(value);
    db_le32(d, (uint32_t) (kl + 1 + vl));
    db_str(d, tw_keys[k].name);
    db_u8(d, '=');
    db_str(d, value);
}

/* Builds the new VORBIS_COMMENT body. vc may be NULL (no block existed). */
static bool vc_build(const vc_t * vc, const tw_req_t req[K_COUNT], dbuf_t * d) {
    static const char new_vendor[] = MT_PRODUCT_NAME " tag_writer";
    bool emitted[K_COUNT] = { false };
    uint32_t count = 0;
    if (vc) {
        db_le32(d, vc->vendor_len);
        db_put(d, vc->vendor, vc->vendor_len);
    } else {
        db_le32(d, (uint32_t) strlen(new_vendor));
        db_str(d, new_vendor);
    }
    size_t count_pos = d->len;
    db_le32(d, 0);
    for (uint32_t i = 0; vc && i < vc->count; i++) {
        int k = vc_item_key(&vc->items[i]);
        if (k < 0 || !req[k].present) {
            db_le32(d, vc->items[i].len);
            db_put(d, vc->items[i].p, vc->items[i].len);
            count++;
        } else if (req[k].value && !emitted[k]) {
            vc_put_item(d, k, req[k].value);
            emitted[k] = true;
            count++;
        }
    }
    for (int k = 0; k < K_COUNT; k++) {
        if (req[k].present && req[k].value && !emitted[k]) {
            vc_put_item(d, k, req[k].value);
            count++;
        }
    }
    if (d->fail) return false;
    wr_le32(d->p + count_pos, count);
    return true;
}

/* Every requested key reads back as exactly one "KEY=value" (or not at all). */
static bool vc_verify_fields(const uint8_t * body, uint32_t n, const tw_req_t req[K_COUNT]) {
    vc_t vc;
    if (!vc_parse(body, n, &vc)) return false;
    bool ok = true;
    for (int k = 0; k < K_COUNT && ok; k++) {
        if (!req[k].present) continue;
        int hits = 0;
        for (uint32_t i = 0; i < vc.count; i++) {
            if (vc_item_key(&vc.items[i]) != k) continue;
            hits++;
            if (!req[k].value) continue;
            size_t kl = strlen(tw_keys[k].name), vl = strlen(req[k].value);
            const vc_item_t * it = &vc.items[i];
            if (it->len != kl + 1 + vl || memcmp(it->p, tw_keys[k].name, kl) != 0 || it->p[kl] != '=' ||
                memcmp(it->p + kl + 1, req[k].value, vl) != 0)
                ok = false;
        }
        if (hits != (req[k].value ? 1 : 0)) ok = false;
    }
    free(vc.items);
    return ok;
}

enum { FO_SRC = 0, FO_VC = 1, FO_PAD = 2 };
typedef struct {
    uint8_t kind;
    uint8_t type;
    int src;
    uint32_t len;
} flac_out_t;

#define FLAC_MAX_OUT (TW_FLAC_MAX_BLOCKS + 2)

typedef struct {
    flac_info_t * fi;
    dbuf_t vc_body;
    bool emit_vc;
    int nout;
    flac_out_t out[FLAC_MAX_OUT];
    uint64_t new_region;
} flac_plan_t;

/* Parses the file and builds the new comment block body; no layout yet. */
static tag_write_result_t flac_prepare(int fd, uint64_t size, const tw_req_t req[K_COUNT], flac_plan_t * pl) {
    flac_info_t * fi = pl->fi;
    tag_write_result_t r = flac_parse(fd, size, fi);
    if (r != TAG_WRITE_OK) return r;

    bool any_value = false;
    for (int k = 0; k < K_COUNT; k++) any_value |= req[k].present && req[k].value;

    vc_t vc;
    bool have_vc = false;
    uint8_t * old = NULL;
    if (fi->vc >= 0) {
        uint32_t n = fi->blk[fi->vc].len;
        if (n > TW_VC_MAX) return TAG_WRITE_UNSUPPORTED;
        old = malloc(n ? n : 1);
        if (!old) return TAG_WRITE_IO_ERROR;
        if (!pread_all(fd, old, n, fi->blk[fi->vc].body_off)) { free(old); return TAG_WRITE_IO_ERROR; }
        if (!vc_parse(old, n, &vc)) { free(old); return TAG_WRITE_PARSE_ERROR; }
        have_vc = true;
    }
    pl->emit_vc = have_vc || any_value;
    if (pl->emit_vc && !vc_build(have_vc ? &vc : NULL, req, &pl->vc_body)) r = TAG_WRITE_IO_ERROR;
    if (have_vc) free(vc.items);
    free(old);
    if (r != TAG_WRITE_OK) return r;
    if (pl->vc_body.len > 0xFFFFFFu) return TAG_WRITE_UNSUPPORTED;
    return TAG_WRITE_OK;
}

static void flac_add(flac_plan_t * pl, uint8_t kind, uint8_t type, int src, uint32_t len) {
    flac_out_t * o = &pl->out[pl->nout++];
    o->kind = kind; o->type = type; o->src = src; o->len = len;
}

static void flac_finish(flac_plan_t * pl) {
    uint64_t t = 0;
    for (int i = 0; i < pl->nout; i++) t += 4u + pl->out[i].len;
    pl->new_region = pl->fi->magic_off + 4 + t;
}

/* Layout "merged" (full-copy default): original blocks minus PADDING, the
 * comment block in place (or right after STREAMINFO), one PADDING block
 * last, sized to keep the audio offset when possible (else 4 KiB). */
static void flac_layout_merged(flac_plan_t * pl) {
    const flac_info_t * fi = pl->fi;
    uint32_t vlen = (uint32_t) pl->vc_body.len;
    uint64_t nonpad = 0;
    pl->nout = 0;
    for (int i = 0; i < fi->nblk; i++) {
        if (fi->blk[i].type == 1) continue;
        if (i == fi->vc) {
            flac_add(pl, FO_VC, 4, -1, vlen);
            nonpad += 4u + vlen;
        } else {
            flac_add(pl, FO_SRC, fi->blk[i].type, i, fi->blk[i].len);
            nonpad += 4u + fi->blk[i].len;
        }
        if (i == 0 && fi->vc < 0 && pl->emit_vc) {
            flac_add(pl, FO_VC, 4, -1, vlen);
            nonpad += 4u + vlen;
        }
    }
    uint64_t old_meta = fi->audio_off - fi->magic_off - 4;
    if (nonpad != old_meta) {
        uint64_t pad = TW_FLAC_NEW_PAD;
        if (nonpad + 4 <= old_meta && old_meta - nonpad - 4 <= 0xFFFFFFu) pad = old_meta - nonpad - 4;
        flac_add(pl, FO_PAD, 1, -1, (uint32_t) pad);
    }
    flac_finish(pl);
}

/* Layout "keep" (in-place): every block where it was; the comment block is
 * replaced and a PADDING block directly after (or before) it absorbs the
 * size difference. Returns false if that is impossible. */
static bool flac_layout_keep(flac_plan_t * pl) {
    const flac_info_t * fi = pl->fi;
    if (fi->vc < 0) return false;
    int64_t delta = (int64_t) pl->vc_body.len - (int64_t) fi->blk[fi->vc].len;
    int pad = -1;
    if (delta != 0) {
        if (fi->vc + 1 < fi->nblk && fi->blk[fi->vc + 1].type == 1) pad = fi->vc + 1;
        else if (fi->vc - 1 > 0 && fi->blk[fi->vc - 1].type == 1) pad = fi->vc - 1;
        if (pad < 0 || (int64_t) fi->blk[pad].len - delta < 0 || (int64_t) fi->blk[pad].len - delta > 0xFFFFFF)
            return false;
    }
    pl->nout = 0;
    for (int i = 0; i < fi->nblk; i++) {
        if (i == fi->vc) flac_add(pl, FO_VC, 4, -1, (uint32_t) pl->vc_body.len);
        else if (i == pad) flac_add(pl, FO_PAD, 1, -1, (uint32_t) ((int64_t) fi->blk[pad].len - delta));
        else flac_add(pl, FO_SRC, fi->blk[i].type, i, fi->blk[i].len);
    }
    flac_finish(pl);
    return true;
}

/* Layout "relocate" (in-place): the old comment block's slot becomes PADDING
 * of the same size, and the new comment block is written into PADDING block
 * pad_idx (followed by the rest of that padding), so blocks such as a large
 * PICTURE between them do not move. */
static bool flac_layout_relocate(flac_plan_t * pl, int pad_idx) {
    const flac_info_t * fi = pl->fi;
    uint32_t vlen = (uint32_t) pl->vc_body.len;
    uint32_t plen = fi->blk[pad_idx].len;
    if (fi->blk[pad_idx].type != 1 || !pl->emit_vc) return false;
    if (plen != vlen && (plen < vlen || plen - vlen < 4)) return false;
    pl->nout = 0;
    for (int i = 0; i < fi->nblk; i++) {
        if (i == fi->vc) flac_add(pl, FO_PAD, 1, -1, fi->blk[i].len);
        else if (i == pad_idx) {
            flac_add(pl, FO_VC, 4, -1, vlen);
            if (plen != vlen) flac_add(pl, FO_PAD, 1, -1, plen - vlen - 4);
        } else flac_add(pl, FO_SRC, fi->blk[i].type, i, fi->blk[i].len);
    }
    flac_finish(pl);
    return true;
}

static tag_write_result_t flac_emit(sink_t * s, const flac_plan_t * pl) {
    const flac_info_t * fi = pl->fi;
    tag_write_result_t r = sink_copy(s, 0, fi->magic_off + 4); /* ID3 prefix + "fLaC" */
    if (r != TAG_WRITE_OK) return r;
    uint8_t zero[256];
    memset(zero, 0, sizeof(zero));
    for (int i = 0; i < pl->nout; i++) {
        const flac_out_t * b = &pl->out[i];
        uint8_t h[4] = { (uint8_t) (b->type | (i == pl->nout - 1 ? 0x80 : 0)), (uint8_t) (b->len >> 16),
                         (uint8_t) (b->len >> 8), (uint8_t) b->len };
        r = sink_put(s, h, 4);
        if (r == TAG_WRITE_OK) {
            if (b->kind == FO_SRC) r = sink_copy(s, fi->blk[b->src].body_off, b->len);
            else if (b->kind == FO_VC) r = sink_put(s, pl->vc_body.p, pl->vc_body.len);
            else {
                uint32_t left = b->len;
                while (left > 0 && r == TAG_WRITE_OK) {
                    uint32_t n = left < sizeof(zero) ? left : (uint32_t) sizeof(zero);
                    r = sink_put(s, zero, n);
                    left -= n;
                }
            }
        }
        if (r != TAG_WRITE_OK) return r;
    }
    return TAG_WRITE_OK;
}

static tag_write_result_t flac_verify(int fd, uint64_t size, const flac_plan_t * pl, const tw_req_t req[K_COUNT]) {
    flac_info_t * nfi = malloc(sizeof(*nfi));
    if (!nfi) return TAG_WRITE_IO_ERROR;
    tag_write_result_t r = flac_parse(fd, size, nfi);
    if (r != TAG_WRITE_OK) { free(nfi); return TAG_WRITE_VERIFY_FAILED; }
    bool ok = nfi->magic_off == pl->fi->magic_off && nfi->audio_off == pl->new_region && nfi->nblk == pl->nout;
    for (int i = 0; ok && i < pl->nout; i++)
        ok = nfi->blk[i].type == pl->out[i].type && nfi->blk[i].len == pl->out[i].len;
    if (ok && nfi->vc >= 0) {
        uint32_t n = nfi->blk[nfi->vc].len;
        uint8_t * b = malloc(n ? n : 1);
        ok = b && n == pl->vc_body.len && pread_all(fd, b, n, nfi->blk[nfi->vc].body_off) &&
             memcmp(b, pl->vc_body.p, n) == 0 && vc_verify_fields(b, n, req);
        free(b);
    } else if (ok) {
        /* No comment block: only valid if every request was a removal. */
        for (int k = 0; k < K_COUNT; k++)
            if (req[k].present && req[k].value) ok = false;
    }
    free(nfi);
    return ok ? TAG_WRITE_OK : TAG_WRITE_VERIFY_FAILED;
}

/* ------------------------------------------------------------------------ */
/* MP3 / ID3v2                                                               */

typedef struct {
    char id[5];
    uint8_t fl[2];
    uint32_t size;
    uint64_t off; /* offset of the 10-byte frame header */
} id3_frm_t;

typedef struct {
    bool has_tag;
    uint8_t major, rev, flags;
    uint32_t body;   /* tag size field (excl. header/footer) */
    uint64_t region; /* bytes before the audio: 10 + body (+10 footer), 0 if no tag */
    int nfrm;
    id3_frm_t * frm;
} id3_info_t;

/* Header only: region length. Used by restore() and as first parse step. */
static tag_write_result_t id3_parse_header(int fd, uint64_t size, id3_info_t * ti) {
    memset(ti, 0, sizeof(*ti));
    uint8_t h[10];
    if (size < 10) return TAG_WRITE_OK;
    if (!pread_all(fd, h, 10, 0)) return TAG_WRITE_IO_ERROR;
    if (memcmp(h, "ID3", 3) != 0) return TAG_WRITE_OK;
    if (h[3] == 0xFF || h[4] == 0xFF || !syncsafe_ok(h + 6)) return TAG_WRITE_PARSE_ERROR;
    ti->has_tag = true;
    ti->major = h[3];
    ti->rev = h[4];
    ti->flags = h[5];
    ti->body = rd_syncsafe(h + 6);
    ti->region = 10u + ti->body + ((ti->major >= 4 && (ti->flags & 0x10)) ? 10u : 0u);
    if (ti->region > size) return TAG_WRITE_PARSE_ERROR;
    return TAG_WRITE_OK;
}

static bool id3_id_ok(const uint8_t * id) {
    for (int i = 0; i < 4; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return false;
    return true;
}

static tag_write_result_t id3_parse(int fd, uint64_t size, id3_info_t * ti) {
    tag_write_result_t r = id3_parse_header(fd, size, ti);
    if (r != TAG_WRITE_OK || !ti->has_tag) return r;
    if (ti->major != 3 && ti->major != 4) return TAG_WRITE_UNSUPPORTED;
    if (ti->flags & 0x80) return TAG_WRITE_UNSUPPORTED; /* global unsynchronisation */
    if (ti->flags & 0x40) return TAG_WRITE_UNSUPPORTED; /* extended header */
    if (ti->flags & (ti->major == 3 ? 0x1F : 0x0F)) return TAG_WRITE_UNSUPPORTED;
    ti->frm = calloc(64, sizeof(id3_frm_t));
    int cap = 64;
    if (!ti->frm) return TAG_WRITE_IO_ERROR;
    uint64_t pos = 10, end = 10u + (uint64_t) ti->body;
    while (end - pos >= 10) {
        uint8_t h[10];
        if (!pread_all(fd, h, 10, pos)) return TAG_WRITE_IO_ERROR;
        if (h[0] == 0) break; /* padding */
        if (!id3_id_ok(h)) return TAG_WRITE_PARSE_ERROR;
        uint32_t fs;
        if (ti->major == 4) {
            if (!syncsafe_ok(h + 4)) return TAG_WRITE_PARSE_ERROR;
            fs = rd_syncsafe(h + 4);
        } else {
            fs = rd_be32(h + 4);
        }
        if (fs > end - pos - 10) return TAG_WRITE_PARSE_ERROR;
        if (ti->nfrm >= TW_ID3_MAX_FRAMES) return TAG_WRITE_UNSUPPORTED;
        if (ti->nfrm == cap) {
            id3_frm_t * nf = realloc(ti->frm, sizeof(id3_frm_t) * (size_t) cap * 2);
            if (!nf) return TAG_WRITE_IO_ERROR;
            ti->frm = nf;
            cap *= 2;
        }
        id3_frm_t * f = &ti->frm[ti->nfrm++];
        memcpy(f->id, h, 4);
        f->id[4] = 0;
        f->fl[0] = h[8];
        f->fl[1] = h[9];
        f->size = fs;
        f->off = pos;
        pos += 10u + fs;
    }
    return TAG_WRITE_OK;
}

/* Reads a frame's payload, undoing grouping/data-length/unsync framing.
 * Compressed or encrypted frames -> TAG_WRITE_UNSUPPORTED. */
static tag_write_result_t id3_read_payload(int fd, const id3_info_t * ti, const id3_frm_t * f, uint8_t ** out,
                                           uint32_t * out_len) {
    *out = NULL;
    *out_len = 0;
    bool comp, enc, grp, unsync = false, dlen = false;
    if (ti->major == 4) {
        grp = f->fl[1] & 0x40; comp = f->fl[1] & 0x08; enc = f->fl[1] & 0x04;
        unsync = f->fl[1] & 0x02; dlen = f->fl[1] & 0x01;
    } else {
        comp = f->fl[1] & 0x80; enc = f->fl[1] & 0x40; grp = f->fl[1] & 0x20;
    }
    if (comp || enc || f->size > TW_FRAME_READ_MAX) return TAG_WRITE_UNSUPPORTED;
    uint8_t * b = malloc(f->size ? f->size : 1);
    if (!b) return TAG_WRITE_IO_ERROR;
    if (!pread_all(fd, b, f->size, f->off + 10)) { free(b); return TAG_WRITE_IO_ERROR; }
    uint32_t skip = (grp ? 1u : 0u) + (dlen ? 4u : 0u);
    if (skip > f->size) { free(b); return TAG_WRITE_PARSE_ERROR; }
    uint32_t n = f->size - skip;
    memmove(b, b + skip, n);
    if (unsync) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < n; i++) {
            b[w++] = b[i];
            if (b[i] == 0xFF && i + 1 < n && b[i + 1] == 0x00) i++;
        }
        n = w;
    }
    *out = b;
    *out_len = n;
    return TAG_WRITE_OK;
}

/* Length of the string at p in encoding enc up to (not incl.) its terminator;
 * *next = offset after the terminator (or n if unterminated). */
static uint32_t id3_str_len(const uint8_t * p, uint32_t n, uint8_t enc, uint32_t * next) {
    if (enc == 1 || enc == 2) {
        uint32_t i = 0;
        for (; i + 1 < n; i += 2)
            if (p[i] == 0 && p[i + 1] == 0) { *next = i + 2; return i; }
        *next = n;
        return n & ~1u;
    }
    for (uint32_t i = 0; i < n; i++)
        if (p[i] == 0) { *next = i + 1; return i; }
    *next = n;
    return n;
}

/* Decodes one string to NUL-terminated UTF-8. */
static bool id3_decode(const uint8_t * p, uint32_t n, uint8_t enc, dbuf_t * out) {
    out->len = 0;
    if (enc == 0) {
        for (uint32_t i = 0; i < n; i++) utf8_put_cp(out, p[i]);
    } else if (enc == 3) {
        db_put(out, p, n);
    } else if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        uint32_t i = 0;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        }
        while (i + 1 < n) {
            uint32_t u = be ? ((uint32_t) p[i] << 8 | p[i + 1]) : ((uint32_t) p[i + 1] << 8 | p[i]);
            i += 2;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
                uint32_t u2 = be ? ((uint32_t) p[i] << 8 | p[i + 1]) : ((uint32_t) p[i + 1] << 8 | p[i]);
                if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
                    i += 2;
                } else u = 0xFFFD;
            } else if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
            utf8_put_cp(out, u);
        }
    } else {
        return false;
    }
    return db_cstr(out);
}

/* First value of a text frame payload. */
static bool id3_text_value(const uint8_t * pl, uint32_t n, dbuf_t * out) {
    if (n < 1) return false;
    uint32_t next;
    uint32_t l = id3_str_len(pl + 1, n - 1, pl[0], &next);
    return id3_decode(pl + 1, l, pl[0], out);
}

enum {
    S_TALB, S_TPE2, S_TPE1, S_TIT2, S_TRCK, S_TPOS, S_DATE, S_TCON,
    S_TXXX_ALBUMID, S_TXXX_ALBUMARTISTID, S_TXXX_ARTISTID, S_TXXX_RELTRACKID, S_UFID, S_COUNT
};

static const char * const s_txxx_desc[S_COUNT] = {
    [S_TXXX_ALBUMID] = "MusicBrainz Album Id",
    [S_TXXX_ALBUMARTISTID] = "MusicBrainz Album Artist Id",
    [S_TXXX_ARTISTID] = "MusicBrainz Artist Id",
    [S_TXXX_RELTRACKID] = "MusicBrainz Release Track Id",
};
static const char s_mb_ufid_owner[] = "http://musicbrainz.org";

typedef struct {
    bool touched;
    bool emit;
    char id[5];
    dbuf_t frame;  /* complete frame (header + payload) to write */
    dbuf_t expect; /* decoded value it must read back as */
} id3_slot_t;

typedef struct {
    id3_info_t ti;
    uint8_t major; /* version written */
    id3_slot_t slot[S_COUNT];
    int nout;
    int * out;    /* >= 0: source frame index; < 0: -(slot + 1) */
    int * fslot;  /* per source frame: slot it belongs to, or -1 */
    uint64_t frames_len;
    uint32_t pad;
    uint64_t new_region;
} mp3_plan_t;

static void mp3_plan_free(mp3_plan_t * pl) {
    free(pl->ti.frm);
    free(pl->out);
    free(pl->fslot);
    for (int s = 0; s < S_COUNT; s++) {
        db_free(&pl->slot[s].frame);
        db_free(&pl->slot[s].expect);
    }
}

/* Slot a frame belongs to (-1 if none / not touched). For TXXX/UFID the
 * payload is inspected; a frame that cannot be read while such a slot is
 * touched -> *err = UNSUPPORTED. */
static int id3_frame_slot(int fd, const id3_info_t * ti, const id3_frm_t * f, const id3_slot_t * slots,
                          uint8_t major, tag_write_result_t * err) {
    static const struct { const char * id; int slot; } simple[] = {
        { "TALB", S_TALB }, { "TPE2", S_TPE2 }, { "TPE1", S_TPE1 }, { "TIT2", S_TIT2 },
        { "TRCK", S_TRCK }, { "TPOS", S_TPOS }, { "TCON", S_TCON },
        { "TDRC", S_DATE }, { "TYER", S_DATE }, { "TDAT", S_DATE }, { "TIME", S_DATE }, { "TRDA", S_DATE },
    };
    (void) major;
    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++)
        if (strcmp(f->id, simple[i].id) == 0) return slots[simple[i].slot].touched ? simple[i].slot : -1;

    bool is_txxx = strcmp(f->id, "TXXX") == 0, is_ufid = strcmp(f->id, "UFID") == 0;
    if (!is_txxx && !is_ufid) return -1;
    bool want = false;
    if (is_ufid) want = slots[S_UFID].touched;
    else
        for (int s = S_TXXX_ALBUMID; s <= S_TXXX_RELTRACKID; s++) want |= slots[s].touched;
    if (!want) return -1;

    uint8_t * pl;
    uint32_t n;
    tag_write_result_t r = id3_read_payload(fd, ti, f, &pl, &n);
    if (r != TAG_WRITE_OK) { *err = r; return -1; }
    int result = -1;
    if (is_ufid) {
        uint32_t next;
        uint32_t l = id3_str_len(pl, n, 0, &next);
        if (l == strlen(s_mb_ufid_owner) && memcmp(pl, s_mb_ufid_owner, l) == 0) result = S_UFID;
    } else if (n >= 1) {
        uint32_t next;
        uint32_t l = id3_str_len(pl + 1, n - 1, pl[0], &next);
        dbuf_t desc = { 0 };
        if (id3_decode(pl + 1, l, pl[0], &desc)) {
            for (int s = S_TXXX_ALBUMID; s <= S_TXXX_RELTRACKID; s++)
                if (slots[s].touched && strcasecmp((const char *) desc.p, s_txxx_desc[s]) == 0) result = s;
        } else if (desc.fail) {
            *err = TAG_WRITE_IO_ERROR;
        }
        db_free(&desc);
    }
    free(pl);
    return result;
}

static void id3_wrap(dbuf_t * f, uint8_t major, const char * id, const dbuf_t * pl) {
    uint32_t n = (uint32_t) pl->len;
    uint8_t h[10];
    memcpy(h, id, 4);
    if (major == 4) {
        h[4] = (uint8_t) ((n >> 21) & 0x7F); h[5] = (uint8_t) ((n >> 14) & 0x7F);
        h[6] = (uint8_t) ((n >> 7) & 0x7F); h[7] = (uint8_t) (n & 0x7F);
    } else {
        h[4] = (uint8_t) (n >> 24); h[5] = (uint8_t) (n >> 16); h[6] = (uint8_t) (n >> 8); h[7] = (uint8_t) n;
    }
    h[8] = h[9] = 0;
    db_put(f, h, 10);
    db_put(f, pl->p, pl->len);
}

static void id3_put_encoded(dbuf_t * pl, uint8_t major, const char * s, bool terminate) {
    if (major == 4) {
        db_str(pl, s);
        if (terminate) db_u8(pl, 0);
    } else {
        db_u8(pl, 0xFF);
        db_u8(pl, 0xFE);
        utf8_to_utf16le(pl, s);
        if (terminate) { db_u8(pl, 0); db_u8(pl, 0); }
    }
}

static bool id3_build_text(id3_slot_t * sl, uint8_t major, const char * id, const char * text, bool latin1) {
    dbuf_t pl = { 0 };
    if (latin1) {
        db_u8(&pl, 0x00);
        db_str(&pl, text); /* caller guarantees ASCII */
    } else {
        db_u8(&pl, major == 4 ? 0x03 : 0x01);
        id3_put_encoded(&pl, major, text, false);
    }
    memcpy(sl->id, id, 5);
    id3_wrap(&sl->frame, major, id, &pl);
    db_str(&sl->expect, text);
    db_cstr(&sl->expect);
    bool ok = !pl.fail && !sl->frame.fail && !sl->expect.fail && pl.len < (1u << 28);
    db_free(&pl);
    return ok;
}

static bool id3_build_txxx(id3_slot_t * sl, uint8_t major, const char * desc, const char * value) {
    dbuf_t pl = { 0 };
    db_u8(&pl, major == 4 ? 0x03 : 0x01);
    id3_put_encoded(&pl, major, desc, true);
    id3_put_encoded(&pl, major, value, false);
    memcpy(sl->id, "TXXX", 5);
    id3_wrap(&sl->frame, major, "TXXX", &pl);
    db_str(&sl->expect, value);
    db_cstr(&sl->expect);
    bool ok = !pl.fail && !sl->frame.fail && !sl->expect.fail && pl.len < (1u << 28);
    db_free(&pl);
    return ok;
}

static bool id3_build_ufid(id3_slot_t * sl, uint8_t major, const char * value) {
    dbuf_t pl = { 0 };
    db_str(&pl, s_mb_ufid_owner);
    db_u8(&pl, 0);
    db_str(&pl, value);
    memcpy(sl->id, "UFID", 5);
    id3_wrap(&sl->frame, major, "UFID", &pl);
    db_str(&sl->expect, value);
    db_cstr(&sl->expect);
    bool ok = !pl.fail && !sl->frame.fail && !sl->expect.fail;
    db_free(&pl);
    return ok;
}

/* Splits "n/N" (whitespace-trimmed halves) from an existing TRCK/TPOS. */
static void split_pair(const char * s, char * a, size_t asz, char * b, size_t bsz) {
    a[0] = b[0] = 0;
    if (!s) return;
    const char * sl = strchr(s, '/');
    size_t al = sl ? (size_t) (sl - s) : strlen(s);
    const char * bs = sl ? sl + 1 : "";
    while (al > 0 && (s[0] == ' ')) { s++; al--; }
    while (al > 0 && s[al - 1] == ' ') al--;
    if (al >= asz) al = asz - 1;
    memcpy(a, s, al);
    a[al] = 0;
    while (*bs == ' ') bs++;
    size_t bl = strlen(bs);
    while (bl > 0 && bs[bl - 1] == ' ') bl--;
    if (bl >= bsz) bl = bsz - 1;
    memcpy(b, bs, bl);
    b[bl] = 0;
}

static tag_write_result_t mp3_pair_slot(int fd, mp3_plan_t * pl, int slot, const char * fid, const tw_req_t * num,
                                        const tw_req_t * tot) {
    id3_slot_t * sl = &pl->slot[slot];
    if (!num->present && !tot->present) return TAG_WRITE_OK;
    if (num->value && strchr(num->value, '/')) return TAG_WRITE_BAD_ARGS;
    if (tot->value && strchr(tot->value, '/')) return TAG_WRITE_BAD_ARGS;
    char old_n[64] = "", old_t[64] = "";
    if (!num->present || !tot->present) {
        /* Merge with the first existing frame. */
        for (int i = 0; i < pl->ti.nfrm; i++) {
            if (strcmp(pl->ti.frm[i].id, fid) != 0) continue;
            uint8_t * p;
            uint32_t n;
            tag_write_result_t r = id3_read_payload(fd, &pl->ti, &pl->ti.frm[i], &p, &n);
            if (r != TAG_WRITE_OK) return r;
            dbuf_t v = { 0 };
            bool ok = id3_text_value(p, n, &v);
            free(p);
            if (!ok) { bool f = v.fail; db_free(&v); return f ? TAG_WRITE_IO_ERROR : TAG_WRITE_PARSE_ERROR; }
            split_pair((const char *) v.p, old_n, sizeof(old_n), old_t, sizeof(old_t));
            db_free(&v);
            break;
        }
    }
    const char * n = num->present ? (num->value ? num->value : "") : old_n;
    const char * t = tot->present ? (tot->value ? tot->value : "") : old_t;
    sl->touched = true;
    if (!n[0]) {
        /* "/N" is not representable: an explicitly requested total without a
         * number is refused; a total merely kept from the old frame goes
         * along with the removed number. */
        if (t[0] && tot->present) return TAG_WRITE_BAD_ARGS;
        sl->emit = false;
        return TAG_WRITE_OK;
    }
    size_t len = strlen(n) + 1 + strlen(t) + 1;
    char * text = malloc(len);
    if (!text) return TAG_WRITE_IO_ERROR;
    if (t[0]) snprintf(text, len, "%s/%s", n, t);
    else snprintf(text, len, "%s", n);
    sl->emit = true;
    bool ok = id3_build_text(sl, pl->major, fid, text, false);
    free(text);
    return ok ? TAG_WRITE_OK : TAG_WRITE_IO_ERROR;
}

static tag_write_result_t mp3_layout(mp3_plan_t * pl, bool append);

static tag_write_result_t mp3_plan(int fd, uint64_t size, const tw_req_t req[K_COUNT], mp3_plan_t * pl) {
    if (req[K_CUESHEET].present) return TAG_WRITE_UNSUPPORTED; /* no ID3 mapping for an embedded sheet */
    tag_write_result_t r = id3_parse(fd, size, &pl->ti);
    if (r != TAG_WRITE_OK) return r;
    pl->major = pl->ti.has_tag ? pl->ti.major : 4;
    uint8_t major = pl->major;

    static const struct { int key; int slot; const char * id; } simple[] = {
        { K_ALBUM, S_TALB, "TALB" }, { K_ALBUMARTIST, S_TPE2, "TPE2" }, { K_ARTIST, S_TPE1, "TPE1" },
        { K_TITLE, S_TIT2, "TIT2" }, { K_GENRE, S_TCON, "TCON" },
    };
    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++) {
        const tw_req_t * q = &req[simple[i].key];
        if (!q->present) continue;
        id3_slot_t * sl = &pl->slot[simple[i].slot];
        sl->touched = true;
        sl->emit = q->value != NULL;
        if (sl->emit && !id3_build_text(sl, major, simple[i].id, q->value, false)) return TAG_WRITE_IO_ERROR;
    }
    if (req[K_DATE].present) {
        id3_slot_t * sl = &pl->slot[S_DATE];
        sl->touched = true;
        sl->emit = req[K_DATE].value != NULL;
        if (sl->emit) {
            const char * v = req[K_DATE].value;
            if (major == 4) {
                if (!id3_build_text(sl, major, "TDRC", v, false)) return TAG_WRITE_IO_ERROR;
            } else {
                char year[5];
                for (int i = 0; i < 4; i++) {
                    if (v[i] < '0' || v[i] > '9') return TAG_WRITE_BAD_ARGS;
                    year[i] = v[i];
                }
                year[4] = 0;
                if (!id3_build_text(sl, major, "TYER", year, true)) return TAG_WRITE_IO_ERROR;
            }
        }
    }
    static const struct { int key; int slot; } txxx[] = {
        { K_MB_ALBUMID, S_TXXX_ALBUMID }, { K_MB_ALBUMARTISTID, S_TXXX_ALBUMARTISTID },
        { K_MB_ARTISTID, S_TXXX_ARTISTID }, { K_MB_RELEASETRACKID, S_TXXX_RELTRACKID },
    };
    for (size_t i = 0; i < sizeof(txxx) / sizeof(txxx[0]); i++) {
        const tw_req_t * q = &req[txxx[i].key];
        if (!q->present) continue;
        id3_slot_t * sl = &pl->slot[txxx[i].slot];
        sl->touched = true;
        sl->emit = q->value != NULL;
        if (sl->emit && !id3_build_txxx(sl, major, s_txxx_desc[txxx[i].slot], q->value)) return TAG_WRITE_IO_ERROR;
    }
    if (req[K_MB_TRACKID].present) {
        id3_slot_t * sl = &pl->slot[S_UFID];
        sl->touched = true;
        sl->emit = req[K_MB_TRACKID].value != NULL;
        if (sl->emit) {
            if (strlen(req[K_MB_TRACKID].value) > TW_UFID_MAX) return TAG_WRITE_BAD_ARGS;
            if (!id3_build_ufid(sl, major, req[K_MB_TRACKID].value)) return TAG_WRITE_IO_ERROR;
        }
    }
    r = mp3_pair_slot(fd, pl, S_TRCK, "TRCK", &req[K_TRACKNUMBER], &req[K_TRACKTOTAL]);
    if (r != TAG_WRITE_OK) return r;
    r = mp3_pair_slot(fd, pl, S_TPOS, "TPOS", &req[K_DISCNUMBER], &req[K_DISCTOTAL]);
    if (r != TAG_WRITE_OK) return r;

    /* Classify the existing frames once; layouts are built from this. */
    pl->fslot = malloc(sizeof(int) * (size_t) (pl->ti.nfrm + 1));
    pl->out = malloc(sizeof(int) * (size_t) (pl->ti.nfrm + S_COUNT + 1));
    if (!pl->out || !pl->fslot) return TAG_WRITE_IO_ERROR;
    for (int i = 0; i < pl->ti.nfrm; i++) {
        tag_write_result_t err = TAG_WRITE_OK;
        pl->fslot[i] = id3_frame_slot(fd, &pl->ti, &pl->ti.frm[i], pl->slot, major, &err);
        if (err != TAG_WRITE_OK) return err;
    }
    return mp3_layout(pl, false);
}

/* Frame order. append == false: a replaced frame takes the position of the
 * first frame it replaces (new ones go last). append == true: all kept
 * frames first in their order, then every new frame (in-place candidate
 * when that changes fewer bytes). */
static tag_write_result_t mp3_layout(mp3_plan_t * pl, bool append) {
    bool emitted[S_COUNT] = { false };
    pl->nout = 0;
    pl->frames_len = 0;
    for (int i = 0; i < pl->ti.nfrm; i++) {
        int s = pl->fslot[i];
        if (s < 0) {
            pl->out[pl->nout++] = i;
            pl->frames_len += 10u + pl->ti.frm[i].size;
        } else if (!append && pl->slot[s].emit && !emitted[s]) {
            pl->out[pl->nout++] = -(s + 1);
            emitted[s] = true;
            pl->frames_len += pl->slot[s].frame.len;
        }
    }
    for (int s = 0; s < S_COUNT; s++) {
        if (pl->slot[s].emit && !emitted[s]) {
            pl->out[pl->nout++] = -(s + 1);
            emitted[s] = true;
            pl->frames_len += pl->slot[s].frame.len;
        }
    }
    if (!pl->ti.has_tag && pl->nout == 0) {
        pl->new_region = 0; /* nothing to write and no tag before: stay tagless */
        return TAG_WRITE_OK;
    }
    if (pl->ti.has_tag && 10u + pl->frames_len <= pl->ti.region) pl->pad = (uint32_t) (pl->ti.region - 10u - pl->frames_len);
    else pl->pad = TW_ID3_NEW_PAD;
    if (pl->frames_len + pl->pad >= (1u << 28)) return TAG_WRITE_UNSUPPORTED;
    pl->new_region = 10u + pl->frames_len + pl->pad;
    return TAG_WRITE_OK;
}

static tag_write_result_t mp3_emit(sink_t * s, const mp3_plan_t * pl) {
    if (pl->new_region == 0) return TAG_WRITE_OK;
    uint32_t body = (uint32_t) (pl->frames_len + pl->pad);
    uint8_t h[10] = { 'I', 'D', '3', pl->major, pl->ti.has_tag ? pl->ti.rev : 0,
                      (uint8_t) (pl->ti.has_tag ? (pl->ti.flags & ~0x10) : 0),
                      (uint8_t) ((body >> 21) & 0x7F), (uint8_t) ((body >> 14) & 0x7F),
                      (uint8_t) ((body >> 7) & 0x7F), (uint8_t) (body & 0x7F) };
    tag_write_result_t r = sink_put(s, h, 10);
    for (int i = 0; r == TAG_WRITE_OK && i < pl->nout; i++) {
        int v = pl->out[i];
        if (v >= 0) {
            const id3_frm_t * f = &pl->ti.frm[v];
            r = sink_copy(s, f->off, 10u + (uint64_t) f->size);
        } else {
            const dbuf_t * fr = &pl->slot[-v - 1].frame;
            r = sink_put(s, fr->p, fr->len);
        }
    }
    uint8_t zero[256];
    memset(zero, 0, sizeof(zero));
    uint32_t left = pl->pad;
    while (r == TAG_WRITE_OK && left > 0) {
        uint32_t n = left < sizeof(zero) ? left : (uint32_t) sizeof(zero);
        r = sink_put(s, zero, n);
        left -= n;
    }
    return r;
}

static tag_write_result_t mp3_verify(int fd, uint64_t size, const mp3_plan_t * pl) {
    id3_info_t nt;
    memset(&nt, 0, sizeof(nt));
    tag_write_result_t r = id3_parse(fd, size, &nt);
    bool ok = r == TAG_WRITE_OK && nt.region == pl->new_region;
    if (ok && pl->new_region == 0) { free(nt.frm); return nt.has_tag ? TAG_WRITE_VERIFY_FAILED : TAG_WRITE_OK; }
    ok = ok && nt.has_tag && nt.major == pl->major && nt.nfrm == pl->nout;
    /* Same frame sequence as planned (ids and sizes). */
    for (int i = 0; ok && i < pl->nout; i++) {
        int v = pl->out[i];
        const char * id = v >= 0 ? pl->ti.frm[v].id : pl->slot[-v - 1].id;
        uint32_t sz = v >= 0 ? pl->ti.frm[v].size : (uint32_t) (pl->slot[-v - 1].frame.len - 10);
        ok = strcmp(nt.frm[i].id, id) == 0 && nt.frm[i].size == sz;
    }
    /* Every touched slot: exactly one frame with our bytes decoding to the
     * expected value, or none. */
    for (int s = 0; ok && s < S_COUNT; s++) {
        const id3_slot_t * sl = &pl->slot[s];
        if (!sl->touched) continue;
        int hits = 0;
        for (int i = 0; ok && i < nt.nfrm; i++) {
            tag_write_result_t err = TAG_WRITE_OK;
            int fs = id3_frame_slot(fd, &nt, &nt.frm[i], pl->slot, pl->major, &err);
            if (err != TAG_WRITE_OK) { ok = false; break; }
            if (fs != s) continue;
            hits++;
            if (!sl->emit) continue;
            uint8_t * raw = malloc(sl->frame.len);
            ok = raw && nt.frm[i].size + 10u == sl->frame.len && pread_all(fd, raw, sl->frame.len, nt.frm[i].off) &&
                 memcmp(raw, sl->frame.p, sl->frame.len) == 0;
            free(raw);
            if (!ok) break;
            uint8_t * p;
            uint32_t n;
            if (id3_read_payload(fd, &nt, &nt.frm[i], &p, &n) != TAG_WRITE_OK) { ok = false; break; }
            dbuf_t v = { 0 };
            if (s == S_UFID) {
                uint32_t next;
                id3_str_len(p, n, 0, &next);
                ok = n - next == strlen((const char *) sl->expect.p) && memcmp(p + next, sl->expect.p, n - next) == 0;
            } else if (s >= S_TXXX_ALBUMID && s <= S_TXXX_RELTRACKID) {
                uint32_t next;
                ok = n >= 1;
                if (ok) {
                    id3_str_len(p + 1, n - 1, p[0], &next);
                    uint32_t vn;
                    uint32_t l = id3_str_len(p + 1 + next, n - 1 - next, p[0], &vn);
                    ok = id3_decode(p + 1 + next, l, p[0], &v) && strcmp((const char *) v.p, (const char *) sl->expect.p) == 0;
                }
            } else {
                ok = id3_text_value(p, n, &v) && strcmp((const char *) v.p, (const char *) sl->expect.p) == 0;
            }
            db_free(&v);
            free(p);
        }
        if (ok && hits != (sl->emit ? 1 : 0)) ok = false;
    }
    free(nt.frm);
    return ok ? TAG_WRITE_OK : TAG_WRITE_VERIFY_FAILED;
}

/* ------------------------------------------------------------------------ */
/* Shared temp + verify + rename driver                                      */

typedef struct {
    char dir[PATH_MAX];
    char tmp[PATH_MAX];
    char btmp[PATH_MAX];
    char bdir[PATH_MAX];
} tw_paths_t;

static bool split_dir(const char * path, char * dir, size_t dsz, const char ** base) {
    const char * slash = strrchr(path, '/');
    if (!slash) {
        if (dsz < 2) return false;
        strcpy(dir, ".");
        *base = path;
        return true;
    }
    size_t dl = (size_t) (slash - path);
    if (dl == 0) dl = 1; /* "/name" */
    if (dl >= dsz) return false;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    *base = slash + 1;
    return true;
}

static void fsync_dir(const char * dir) {
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    (void) fsync(dfd); /* best effort: some filesystems refuse fsync on a directory */
    close(dfd);
}

typedef enum { JOB_FLAC, JOB_MP3, JOB_RESTORE } tw_job_kind_t;

typedef struct {
    tw_job_kind_t kind;
    int fmt;
    const tw_req_t * req;
    flac_plan_t * flac;
    mp3_plan_t * mp3;
    /* restore (v1) */
    int bfd;
    uint64_t b_region;
    uint64_t b_audio_hash;
    /* common */
    int src;
    struct stat st;
    uint64_t size;
    uint64_t old_region; /* audio offset in the source */
    uint64_t new_region; /* audio offset in the new file */
    uint64_t audio_hash;
} tw_job_t;

static tag_write_result_t job_emit(tw_job_t * j, sink_t * s) {
    switch (j->kind) {
        case JOB_FLAC: return flac_emit(s, j->flac);
        case JOB_MP3: return mp3_emit(s, j->mp3);
        case JOB_RESTORE: return out_copy(s->o, j->bfd, TW_BACKUP_HDR, j->b_region, NULL);
    }
    return TAG_WRITE_IO_ERROR;
}

/* Audio offset of a file in the given format, structure-checked. */
static tag_write_result_t audio_offset(int fd, uint64_t size, int fmt, uint64_t * off) {
    if (fmt == TW_FMT_FLAC) {
        flac_info_t * fi = malloc(sizeof(*fi));
        if (!fi) return TAG_WRITE_IO_ERROR;
        tag_write_result_t r = flac_parse(fd, size, fi);
        if (r == TAG_WRITE_OK) *off = fi->audio_off;
        free(fi);
        return r;
    }
    id3_info_t ti;
    tag_write_result_t r = id3_parse_header(fd, size, &ti);
    if (r == TAG_WRITE_OK) *off = ti.region;
    return r;
}

static tag_write_result_t job_verify_tags(tw_job_t * j, int fd, uint64_t size) {
    switch (j->kind) {
        case JOB_FLAC: return flac_verify(fd, size, j->flac, j->req);
        case JOB_MP3: return mp3_verify(fd, size, j->mp3);
        case JOB_RESTORE: {
            /* The restored region must parse back to the same audio offset. */
            uint64_t off = 0;
            tag_write_result_t r = audio_offset(fd, size, j->fmt, &off);
            return (r == TAG_WRITE_OK && off == j->new_region) ? TAG_WRITE_OK : TAG_WRITE_VERIFY_FAILED;
        }
    }
    return TAG_WRITE_VERIFY_FAILED;
}

/* Sampled audio hash used by the in-place mode: FNV-1a 64 over the audio
 * length (8 bytes LE), the first min(A, 1 MiB) and the last min(A, 1 MiB)
 * bytes of the audio payload [off, size). Reads at most 2 MiB. */
#define TW_SAMPLE_BYTES (1024u * 1024u)
static tag_write_result_t audio_sample_hash(int fd, uint64_t off, uint64_t size, uint8_t * buf, uint64_t * out) {
    uint64_t a = size - off;
    uint8_t lb[8];
    wr_le64(lb, a);
    uint64_t h = fnv64(FNV_INIT, lb, 8);
    uint64_t n = a < TW_SAMPLE_BYTES ? a : TW_SAMPLE_BYTES;
    for (int part = 0; part < 2; part++) {
        uint64_t pos = part == 0 ? off : size - n, left = n;
        while (left > 0) {
            size_t k = left < TW_IO_BUF ? (size_t) left : TW_IO_BUF;
            if (!pread_all(fd, buf, k, pos)) return TAG_WRITE_IO_ERROR;
            h = fnv64(h, buf, k);
            pos += k;
            left -= k;
        }
    }
    *out = h;
    return TAG_WRITE_OK;
}

static bool pwrite_all(int fd, const uint8_t * p, size_t n, uint64_t off) {
    while (n > 0) {
        ssize_t r = pwrite(fd, p, n, (off_t) off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= (size_t) r;
        off += (uint64_t) r;
    }
    return true;
}

/* Writes a backup durably: hdr, then [0, src_len) of src, then m1 and m2,
 * then the FNV-1a 64 of everything before it. Goes to "<bpath>.tmp", is
 * fsync()ed, re-read and checked, and only then renamed over bpath. */
static bool backup_commit(const char * bpath, tw_paths_t * P, const uint8_t * hdr, size_t hlen, int src,
                          uint64_t src_len, const uint8_t * m1, size_t l1, const uint8_t * m2, size_t l2,
                          uint8_t * buf) {
    const char * bbase;
    if (!split_dir(bpath, P->bdir, sizeof(P->bdir), &bbase) || !bbase[0]) return false;
    if ((size_t) snprintf(P->btmp, sizeof(P->btmp), "%s.tmp", bpath) >= sizeof(P->btmp)) return false;
    if (TW_INJECT(4)) return false;
    (void) unlink(P->btmp);
    int fd = open(P->btmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    tw_out_t o = { fd, 0, FNV_INIT, buf };
    bool ok = out_put(&o, hdr, hlen) && (src_len == 0 || out_copy(&o, src, 0, src_len, NULL) == TAG_WRITE_OK) &&
              (l1 == 0 || out_put(&o, m1, l1)) && (l2 == 0 || out_put(&o, m2, l2));
    uint8_t t[8];
    wr_le64(t, o.hash);
    ok = ok && write_all(fd, t, 8) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) {
        int rfd = open(P->btmp, O_RDONLY | O_CLOEXEC);
        ok = rfd >= 0;
        if (ok) {
            struct stat bst;
            uint64_t want = o.written;
            ok = fstat(rfd, &bst) == 0 && (uint64_t) bst.st_size == want + 8;
#ifdef POSIX_FADV_DONTNEED
            if (ok) (void) posix_fadvise(rfd, 0, 0, POSIX_FADV_DONTNEED);
#endif
            uint64_t hh = FNV_INIT, pos = 0;
            while (ok && pos < want) {
                size_t n = want - pos < TW_IO_BUF ? (size_t) (want - pos) : TW_IO_BUF;
                ok = pread_all(rfd, buf, n, pos);
                hh = fnv64(hh, buf, n);
                pos += n;
            }
            ok = ok && pread_all(rfd, t, 8, want) && rd_le64(t) == hh && hh == o.hash;
            close(rfd);
        }
    }
    ok = ok && rename(P->btmp, bpath) == 0;
    if (!ok) {
        (void) unlink(P->btmp);
        return false;
    }
    fsync_dir(P->bdir);
    return true;
}

/* Version-1 backup (full-copy mode): see tag_writer.h. */
static bool write_backup_v1(const char * bpath, tw_paths_t * P, tw_job_t * j, uint8_t * buf) {
    uint8_t h[TW_BACKUP_HDR];
    memcpy(h, TW_BACKUP_MAGIC, 8);
    wr_le32(h + 8, 1);
    wr_le32(h + 12, (uint32_t) j->fmt);
    wr_le64(h + 16, j->size);
    wr_le64(h + 24, j->old_region);
    wr_le64(h + 32, j->size - j->old_region);
    wr_le64(h + 40, j->audio_hash);
    return backup_commit(bpath, P, h, sizeof(h), j->src, j->old_region, NULL, 0, NULL, 0, buf);
}

/* The original must still be the file we parsed, unchanged. */
static bool source_unchanged(const tw_job_t * j, const char * path) {
    struct stat now, by_path;
    if (TW_INJECT(5) || fstat(j->src, &now) != 0 || lstat(path, &by_path) != 0) return false;
    return now.st_size == j->st.st_size && now.st_mtime == j->st.st_mtime &&
           now.st_mtim.tv_nsec == j->st.st_mtim.tv_nsec && now.st_ino == j->st.st_ino &&
           by_path.st_ino == j->st.st_ino && by_path.st_dev == j->st.st_dev;
}

static bool make_paths(const char * path, tw_paths_t * P) {
    const char * base;
    if (!split_dir(path, P->dir, sizeof(P->dir), &base) || !base[0]) return false;
    int n;
    if (strlen(base) + 9 <= NAME_MAX) n = snprintf(P->tmp, sizeof(P->tmp), "%s/.%s.tagtmp", P->dir, base);
    else n = snprintf(P->tmp, sizeof(P->tmp), "%s/.mellowtone-tagwriter.tagtmp", P->dir);
    return n >= 0 && (size_t) n < sizeof(P->tmp);
}

/* Full-copy mode: temp file + verify + rename. */
static tag_write_result_t run_job(tw_job_t * j, const char * path, const char * backup_path) {
    tw_paths_t * P = malloc(sizeof(*P));
    uint8_t * buf = malloc(TW_IO_BUF);
    if (!P || !buf) { free(P); free(buf); return TAG_WRITE_IO_ERROR; }
    tag_write_result_t r = TAG_WRITE_OK;
    int fd = -1;
    bool tmp_made = false;
    if (!make_paths(path, P)) { r = TAG_WRITE_BAD_ARGS; goto out; }
    if (unlink(P->tmp) != 0 && errno != ENOENT) { r = TAG_WRITE_IO_ERROR; goto out; }
    fd = open(P->tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) { r = TAG_WRITE_IO_ERROR; goto out; }
    tmp_made = true;

    uint64_t audio_len = j->size - j->old_region;
    uint64_t new_size = j->new_region + audio_len;
    {
        struct statvfs vs;
        if (fstatvfs(fd, &vs) == 0 && (uint64_t) vs.f_bavail * vs.f_frsize < new_size + 65536u) {
            r = TAG_WRITE_IO_ERROR;
            goto out;
        }
    }

    tw_out_t o = { fd, 0, FNV_INIT, buf };
    sink_t s;
    memset(&s, 0, sizeof(s));
    s.kind = SINK_OUT;
    s.o = &o;
    s.src = j->src;
    r = job_emit(j, &s);
    if (r != TAG_WRITE_OK) goto out;
    if (o.written != j->new_region) { r = TAG_WRITE_IO_ERROR; goto out; }
    j->audio_hash = FNV_INIT;
    r = out_copy(&o, j->src, j->old_region, audio_len, &j->audio_hash);
    if (r != TAG_WRITE_OK) goto out;
    if (j->kind == JOB_RESTORE && j->audio_hash != j->b_audio_hash) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    (void) fchmod(fd, j->st.st_mode & 0777); /* best effort (exFAT has no modes) */
    if (fsync(fd) != 0) { r = TAG_WRITE_IO_ERROR; goto out; }
    if (close(fd) != 0) { fd = -1; r = TAG_WRITE_IO_ERROR; goto out; }
    fd = -1;

    /* Verify from the card: drop the cached pages, then re-read everything. */
    fd = open(P->tmp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { r = TAG_WRITE_IO_ERROR; goto out; }
    {
        struct stat tst;
        if (fstat(fd, &tst) != 0 || (uint64_t) tst.st_size != new_size || TW_INJECT(2)) {
            r = TAG_WRITE_VERIFY_FAILED;
            goto out;
        }
#ifdef POSIX_FADV_DONTNEED
        (void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
        uint64_t h_all = FNV_INIT, h_audio = FNV_INIT, pos = 0;
        while (pos < new_size) {
            size_t n = new_size - pos < TW_IO_BUF ? (size_t) (new_size - pos) : TW_IO_BUF;
            if (!pread_all(fd, buf, n, pos)) { r = TAG_WRITE_IO_ERROR; goto out; }
            h_all = fnv64(h_all, buf, n);
            if (pos + n > j->new_region) {
                size_t skip = pos >= j->new_region ? 0 : (size_t) (j->new_region - pos);
                h_audio = fnv64(h_audio, buf + skip, n - skip);
            }
            pos += n;
        }
        if (h_all != o.hash || h_audio != j->audio_hash) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
        r = job_verify_tags(j, fd, new_size);
        if (r != TAG_WRITE_OK) goto out;
    }
    close(fd);
    fd = -1;

    if (!source_unchanged(j, path)) { r = TAG_WRITE_IO_ERROR; goto out; }
    if (backup_path && !write_backup_v1(backup_path, P, j, buf)) { r = TAG_WRITE_BACKUP_FAILED; goto out; }
    if (TW_INJECT(3) || rename(P->tmp, path) != 0) { r = TAG_WRITE_IO_ERROR; goto out; }
    tmp_made = false;
    fsync_dir(P->dir);
    r = TAG_WRITE_OK;

out:
    if (fd >= 0) close(fd);
    if (tmp_made) (void) unlink(P->tmp);
    free(P);
    free(buf);
    return r;
}

/* ------------------------------------------------------------------------ */
/* In-place mode                                                             */

#define TW_BACKUP2_HDR 64u
#define TW_SPAN_MAX (32u * 1024u * 1024u)

/* Changed spans of the current layout against the original (region sizes
 * must be equal). L->n == 0: nothing differs. */
static tag_write_result_t job_spans(tw_job_t * j, uint8_t * scratch, span_list_t * L) {
    sink_t s;
    memset(&s, 0, sizeof(s));
    memset(L, 0, sizeof(*L));
    s.kind = SINK_CMP;
    s.src = j->src;
    s.a = scratch;
    s.b = scratch + TW_IO_BUF;
    s.spans = L;
    tag_write_result_t r = job_emit(j, &s);
    if (r != TAG_WRITE_OK) return r;
    if (s.pos != j->new_region) return TAG_WRITE_IO_ERROR;
    spans_limit(L);
    return TAG_WRITE_OK;
}

/* Re-reads [off, off+len) of fd (cache dropped first) and compares. */
static bool span_reads_back(int fd, uint64_t off, const uint8_t * want, size_t len, uint8_t * buf) {
#ifdef POSIX_FADV_DONTNEED
    (void) posix_fadvise(fd, 0, (off_t) (off + len), POSIX_FADV_DONTNEED);
#endif
    size_t done = 0;
    while (done < len) {
        size_t k = len - done < TW_IO_BUF ? len - done : TW_IO_BUF;
        if (!pread_all(fd, buf, k, off + done) || memcmp(buf, want + done, k) != 0) return false;
        done += k;
    }
    return true;
}

static bool spans_write(int fd, const span_list_t * L, const uint8_t * data) {
    uint64_t pos = 0;
    for (int i = 0; i < L->n; i++) {
        if (!pwrite_all(fd, data + pos, (size_t) L->s[i].len, L->s[i].off)) return false;
        pos += L->s[i].len;
    }
    return fsync(fd) == 0;
}

static bool spans_read_back(int fd, const span_list_t * L, const uint8_t * data, uint8_t * buf) {
    uint64_t pos = 0;
    for (int i = 0; i < L->n; i++) {
        if (!span_reads_back(fd, L->s[i].off, data + pos, (size_t) L->s[i].len, buf)) return false;
        pos += L->s[i].len;
    }
    return true;
}

static bool spans_rollback(int fd, const span_list_t * L, const uint8_t * old, uint8_t * buf) {
    return spans_write(fd, L, old) && spans_read_back(fd, L, old, buf);
}

static tag_write_result_t run_inplace(tw_job_t * j, const char * path, const char * backup_path,
                                      const span_list_t * L) {
    uint64_t total = spans_total(L);
    if (total > TW_SPAN_MAX) return TAG_WRITE_UNSUPPORTED;
    tw_paths_t * P = malloc(sizeof(*P));
    uint8_t * buf = malloc(TW_IO_BUF);
    uint8_t * nspan = malloc((size_t) total);
    uint8_t * ospan = malloc((size_t) total);
    tag_write_result_t r = TAG_WRITE_OK;
    int rw = -1;
    if (!P || !buf || !nspan || !ospan) { r = TAG_WRITE_IO_ERROR; goto out; }

    /* New and old bytes of every span, concatenated in span order. */
    {
        uint64_t pos = 0;
        for (int i = 0; i < L->n; i++) {
            sink_t s;
            memset(&s, 0, sizeof(s));
            s.kind = SINK_CAP;
            s.src = j->src;
            s.cap_off = L->s[i].off;
            s.cap_len = L->s[i].len;
            s.cap = nspan + pos;
            r = job_emit(j, &s);
            if (r != TAG_WRITE_OK) goto out;
            if (!pread_all(j->src, ospan + pos, (size_t) L->s[i].len, L->s[i].off)) { r = TAG_WRITE_IO_ERROR; goto out; }
            pos += L->s[i].len;
        }
    }
    r = audio_sample_hash(j->src, j->old_region, j->size, buf, &j->audio_hash);
    if (r != TAG_WRITE_OK) goto out;
    if (!source_unchanged(j, path)) { r = TAG_WRITE_IO_ERROR; goto out; }

    if (backup_path) {
        size_t hlen = TW_BACKUP2_HDR + 24u * (size_t) L->n;
        uint8_t h[TW_BACKUP2_HDR + 24u * TW_MAX_SPANS];
        memcpy(h, TW_BACKUP_MAGIC, 8);
        wr_le32(h + 8, 2);
        wr_le32(h + 12, (uint32_t) j->fmt);
        wr_le32(h + 16, 1); /* mode: in-place */
        wr_le32(h + 20, 1); /* audio hash kind: sampled (see audio_sample_hash) */
        wr_le64(h + 24, j->size);
        wr_le64(h + 32, j->old_region);
        wr_le64(h + 40, j->size - j->old_region);
        wr_le64(h + 48, j->audio_hash);
        wr_le32(h + 56, (uint32_t) L->n);
        wr_le32(h + 60, 0);
        uint64_t pos = 0;
        for (int i = 0; i < L->n; i++) {
            uint8_t * e = h + TW_BACKUP2_HDR + 24u * (size_t) i;
            wr_le64(e, L->s[i].off);
            wr_le64(e + 8, L->s[i].len);
            wr_le64(e + 16, fnv64(FNV_INIT, nspan + pos, (size_t) L->s[i].len));
            pos += L->s[i].len;
        }
        if (!make_paths(path, P) ||
            !backup_commit(backup_path, P, h, hlen, -1, 0, ospan, (size_t) total, nspan, (size_t) total, buf)) {
            r = TAG_WRITE_BACKUP_FAILED;
            goto out;
        }
    }
    if (TW_INJECT(6) || !source_unchanged(j, path)) { r = TAG_WRITE_IO_ERROR; goto out; }

    rw = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat rst;
    if (rw < 0 || fstat(rw, &rst) != 0 || rst.st_ino != j->st.st_ino || rst.st_dev != j->st.st_dev ||
        rst.st_size != j->st.st_size) {
        r = TAG_WRITE_IO_ERROR;
        goto out;
    }
    if (TW_INJECT(7)) {
        /* Test: a torn write -- the first half of the new bytes reaches the
         * card, then "crash" (no roll-back). */
        uint64_t left = total / 2, pos = 0;
        for (int i = 0; i < L->n && left > 0; i++) {
            uint64_t k = L->s[i].len < left ? L->s[i].len : left;
            (void) pwrite_all(rw, nspan + pos, (size_t) k, L->s[i].off);
            pos += L->s[i].len;
            left -= k;
        }
        (void) fsync(rw);
        r = TAG_WRITE_IO_ERROR;
        goto out;
    }
    if (!spans_write(rw, L, nspan)) {
        (void) spans_rollback(rw, L, ospan, buf); /* best effort; the backup covers the rest */
        r = TAG_WRITE_IO_ERROR;
        goto out;
    }

    /* Verify from the card. */
    {
        bool ok = spans_read_back(rw, L, nspan, buf);
        struct stat vst;
        ok = ok && fstat(rw, &vst) == 0 && vst.st_size == j->st.st_size;
        ok = ok && job_verify_tags(j, rw, j->size) == TAG_WRITE_OK;
        uint64_t ah = 0;
        ok = ok && audio_sample_hash(rw, j->old_region, j->size, buf, &ah) == TAG_WRITE_OK && ah == j->audio_hash;
        if (TW_INJECT(8)) ok = false;
        if (!ok) {
            r = spans_rollback(rw, L, ospan, buf) ? TAG_WRITE_VERIFY_FAILED : TAG_WRITE_IO_ERROR;
            goto out;
        }
    }
    r = TAG_WRITE_OK;

out:
    if (rw >= 0) close(rw);
    free(P);
    free(buf);
    free(nspan);
    free(ospan);
    return r;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */

static tag_write_result_t open_source(const char * path, tw_job_t * j) {
    j->src = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (j->src < 0) return errno == ELOOP ? TAG_WRITE_UNSUPPORTED : TAG_WRITE_IO_ERROR;
    if (fstat(j->src, &j->st) != 0) { close(j->src); j->src = -1; return TAG_WRITE_IO_ERROR; }
    if (!S_ISREG(j->st.st_mode)) { close(j->src); j->src = -1; return TAG_WRITE_UNSUPPORTED; }
    j->size = (uint64_t) j->st.st_size;
    return TAG_WRITE_OK;
}

typedef struct {
    int nout;
    uint64_t new_region;
    flac_out_t out[FLAC_MAX_OUT];
} flac_saved_t;

static bool spans_better(const span_list_t * a, const span_list_t * best, bool have_best) {
    if (!have_best) return true;
    uint64_t ta = spans_total(a), tb = spans_total(best);
    return ta < tb || (ta == tb && a->n < best->n);
}

/* Picks the in-place layout changing the fewest bytes, if any layout keeps
 * the tag region's size. Leaves that layout in the plan (or the full-copy
 * layout when *in_place is false). */
static tag_write_result_t choose_layout(tw_job_t * j, uint8_t * scratch, bool * in_place, span_list_t * best) {
    *in_place = false;
    bool have = false;
    span_list_t cur;
    tag_write_result_t r;
    if (j->kind == JOB_FLAC) {
        flac_plan_t * pl = j->flac;
        flac_saved_t * saved = malloc(sizeof(*saved));
        if (!saved) return TAG_WRITE_IO_ERROR;
        for (int c = -2; c < pl->fi->nblk; c++) {
            bool ok;
            if (c == -2) ok = flac_layout_keep(pl);
            else if (c == -1) { flac_layout_merged(pl); ok = true; }
            else ok = pl->fi->blk[c].type == 1 && flac_layout_relocate(pl, c);
            if (!ok || pl->new_region != pl->fi->audio_off) continue;
            j->new_region = pl->new_region;
            r = job_spans(j, scratch, &cur);
            if (r != TAG_WRITE_OK) { free(saved); return r; }
            if (spans_better(&cur, best, have)) {
                *best = cur;
                have = true;
                saved->nout = pl->nout;
                saved->new_region = pl->new_region;
                memcpy(saved->out, pl->out, sizeof(flac_out_t) * (size_t) pl->nout);
            }
        }
        if (have) {
            pl->nout = saved->nout;
            pl->new_region = saved->new_region;
            memcpy(pl->out, saved->out, sizeof(flac_out_t) * (size_t) saved->nout);
            *in_place = true;
        } else {
            flac_layout_merged(pl);
        }
        free(saved);
        j->new_region = pl->new_region;
        return TAG_WRITE_OK;
    }
    mp3_plan_t * pl = j->mp3;
    bool best_append = false;
    for (int append = 0; append < 2; append++) {
        r = mp3_layout(pl, append != 0);
        if (r != TAG_WRITE_OK) return r;
        if (pl->new_region != pl->ti.region) continue;
        j->new_region = pl->new_region;
        r = job_spans(j, scratch, &cur);
        if (r != TAG_WRITE_OK) return r;
        if (spans_better(&cur, best, have)) {
            *best = cur;
            have = true;
            best_append = append != 0;
        }
    }
    r = mp3_layout(pl, have ? best_append : false);
    if (r != TAG_WRITE_OK) return r;
    *in_place = have;
    j->new_region = pl->new_region;
    return TAG_WRITE_OK;
}

tag_write_result_t tag_writer_apply_ex(const char * path, const tag_field_t * fields, int count,
                                       const char * backup_path, tag_write_mode_t * out_mode) {
    if (out_mode) *out_mode = TAG_WRITE_MODE_NONE;
    if (!path || !path[0] || strlen(path) >= PATH_MAX) return TAG_WRITE_BAD_ARGS;
    if (backup_path && (!backup_path[0] || strlen(backup_path) + 5 >= PATH_MAX)) return TAG_WRITE_BAD_ARGS;
    int fmt = format_of(path);
    if (fmt == TW_FMT_NONE) return TAG_WRITE_UNSUPPORTED;
    tw_req_t req[K_COUNT];
    tag_write_result_t r = normalize_fields(fields, count, req);
    if (r != TAG_WRITE_OK) return r;

    tw_job_t j;
    memset(&j, 0, sizeof(j));
    j.fmt = fmt;
    j.req = req;
    j.bfd = -1;
    r = open_source(path, &j);
    if (r != TAG_WRITE_OK) return r;

    uint8_t * scratch = malloc(2 * TW_IO_BUF);
    if (!scratch) r = TAG_WRITE_IO_ERROR;
    if (r == TAG_WRITE_OK && fmt == TW_FMT_FLAC) {
        j.kind = JOB_FLAC;
        j.flac = calloc(1, sizeof(flac_plan_t));
        if (j.flac) j.flac->fi = malloc(sizeof(flac_info_t));
        if (!j.flac || !j.flac->fi) r = TAG_WRITE_IO_ERROR;
        else r = flac_prepare(j.src, j.size, req, j.flac);
        if (r == TAG_WRITE_OK) j.old_region = j.flac->fi->audio_off;
    } else if (r == TAG_WRITE_OK) {
        j.kind = JOB_MP3;
        j.mp3 = calloc(1, sizeof(mp3_plan_t));
        if (!j.mp3) r = TAG_WRITE_IO_ERROR;
        else r = mp3_plan(j.src, j.size, req, j.mp3);
        if (r == TAG_WRITE_OK) j.old_region = j.mp3->ti.region;
    }
    if (r == TAG_WRITE_OK) {
        bool in_place = false;
        span_list_t * spans = malloc(sizeof(*spans));
        if (!spans) r = TAG_WRITE_IO_ERROR;
        else r = choose_layout(&j, scratch, &in_place, spans);
        if (r == TAG_WRITE_OK && in_place && spans->n == 0) {
            /* Already exactly as requested: nothing written, no backup. */
        } else if (r == TAG_WRITE_OK && in_place) {
            r = run_inplace(&j, path, backup_path, spans);
            if (r == TAG_WRITE_OK && out_mode) *out_mode = TAG_WRITE_MODE_IN_PLACE;
        } else if (r == TAG_WRITE_OK) {
            r = run_job(&j, path, backup_path);
            if (r == TAG_WRITE_OK && out_mode) *out_mode = TAG_WRITE_MODE_FULL_COPY;
        }
        free(spans);
    }
    if (j.flac) {
        free(j.flac->fi);
        db_free(&j.flac->vc_body);
        free(j.flac);
    }
    if (j.mp3) {
        mp3_plan_free(j.mp3);
        free(j.mp3);
    }
    free(scratch);
    close(j.src);
    return r;
}

tag_write_result_t tag_writer_apply(const char * path, const tag_field_t * fields, int count, const char * backup_path) {
    return tag_writer_apply_ex(path, fields, count, backup_path, NULL);
}

/* Restore of a version-2 (in-place) backup. */
static tag_write_result_t restore_v2(const char * path, int fmt, int bfd, const struct stat * bst) {
    uint8_t h[TW_BACKUP2_HDR];
    if ((uint64_t) bst->st_size < TW_BACKUP2_HDR + 8 || !pread_all(bfd, h, sizeof(h), 0)) return TAG_WRITE_PARSE_ERROR;
    uint32_t bfmt = rd_le32(h + 12), n = rd_le32(h + 56);
    uint64_t osize = rd_le64(h + 24), region = rd_le64(h + 32), alen = rd_le64(h + 40), ahash = rd_le64(h + 48);
    if ((bfmt != TW_FMT_FLAC && bfmt != TW_FMT_MP3) || rd_le32(h + 16) != 1 || rd_le32(h + 20) != 1 ||
        region > osize || osize - region != alen || n == 0 || n > TW_MAX_SPANS ||
        (uint64_t) bst->st_size > TW_BACKUP2_HDR + 24u * TW_MAX_SPANS + 2u * TW_SPAN_MAX + 8)
        return TAG_WRITE_PARSE_ERROR;

    tag_write_result_t r = TAG_WRITE_OK;
    int src = -1;
    size_t bsize = (size_t) bst->st_size;
    uint8_t * all = malloc(bsize);
    uint8_t * cur = NULL;
    uint8_t * buf = malloc(TW_IO_BUF);
    span_list_t * L = malloc(sizeof(*L));
    if (!all || !buf || !L) { r = TAG_WRITE_IO_ERROR; goto out; }
    if (!pread_all(bfd, all, bsize, 0)) { r = TAG_WRITE_IO_ERROR; goto out; }
    if (rd_le64(all + bsize - 8) != fnv64(FNV_INIT, all, bsize - 8)) { r = TAG_WRITE_PARSE_ERROR; goto out; }
    uint64_t total = 0, prev_end = 0;
    L->n = (int) n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t * e = all + TW_BACKUP2_HDR + 24u * i;
        L->s[i].off = rd_le64(e);
        L->s[i].len = rd_le64(e + 8);
        if (L->s[i].len == 0 || L->s[i].len > TW_SPAN_MAX || L->s[i].off < prev_end || L->s[i].off > region ||
            L->s[i].len > region - L->s[i].off) {
            r = TAG_WRITE_PARSE_ERROR;
            goto out;
        }
        prev_end = L->s[i].off + L->s[i].len;
        total += L->s[i].len;
    }
    if (total > TW_SPAN_MAX || bsize != TW_BACKUP2_HDR + 24u * n + 2 * total + 8) { r = TAG_WRITE_PARSE_ERROR; goto out; }
    const uint8_t * ospan = all + TW_BACKUP2_HDR + 24u * n;
    const uint8_t * nspan = ospan + total;
    {
        uint64_t pos = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (fnv64(FNV_INIT, nspan + pos, (size_t) L->s[i].len) != rd_le64(all + TW_BACKUP2_HDR + 24u * i + 16)) {
                r = TAG_WRITE_PARSE_ERROR;
                goto out;
            }
            pos += L->s[i].len;
        }
    }
    if ((int) bfmt != fmt) { r = TAG_WRITE_BAD_ARGS; goto out; }

    src = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (src < 0 || fstat(src, &st) != 0) { r = errno == ELOOP ? TAG_WRITE_UNSUPPORTED : TAG_WRITE_IO_ERROR; goto out; }
    if (!S_ISREG(st.st_mode)) { r = TAG_WRITE_UNSUPPORTED; goto out; }
    if ((uint64_t) st.st_size != osize) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    uint64_t ah = 0;
    r = audio_sample_hash(src, region, osize, buf, &ah);
    if (r != TAG_WRITE_OK) goto out;
    if (ah != ahash) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    cur = malloc((size_t) total);
    if (!cur) { r = TAG_WRITE_IO_ERROR; goto out; }
    {
        uint64_t pos = 0;
        for (int i = 0; i < L->n; i++) {
            if (!pread_all(src, cur + pos, (size_t) L->s[i].len, L->s[i].off)) { r = TAG_WRITE_IO_ERROR; goto out; }
            pos += L->s[i].len;
        }
    }
    if (memcmp(cur, ospan, (size_t) total) != 0) {
        /* Only our own write may be there: every byte is either the old or
         * the new one (a complete or a torn write); anything else refuses. */
        for (uint64_t i = 0; i < total; i++)
            if (cur[i] != ospan[i] && cur[i] != nspan[i]) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
        if (TW_INJECT(2)) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
        if (!spans_write(src, L, ospan)) { r = TAG_WRITE_IO_ERROR; goto out; }
    }
    if (!spans_read_back(src, L, ospan, buf)) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    uint64_t aoff = 0;
    if (audio_offset(src, osize, fmt, &aoff) != TAG_WRITE_OK || aoff != region) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    r = TAG_WRITE_OK;
out:
    if (src >= 0) close(src);
    free(all);
    free(cur);
    free(buf);
    free(L);
    return r;
}

tag_write_result_t tag_writer_restore(const char * path, const char * backup_path) {
    if (!path || !path[0] || !backup_path || !backup_path[0] || strlen(path) >= PATH_MAX) return TAG_WRITE_BAD_ARGS;
    int fmt = format_of(path);
    if (fmt == TW_FMT_NONE) return TAG_WRITE_UNSUPPORTED;

    tw_job_t j;
    memset(&j, 0, sizeof(j));
    j.kind = JOB_RESTORE;
    j.fmt = fmt;
    j.src = -1;
    j.bfd = open(backup_path, O_RDONLY | O_CLOEXEC);
    if (j.bfd < 0) return TAG_WRITE_IO_ERROR;

    tag_write_result_t r = TAG_WRITE_OK;
    uint8_t * buf = malloc(TW_IO_BUF);
    struct stat bst;
    uint8_t h[TW_BACKUP_HDR];
    if (!buf) { r = TAG_WRITE_IO_ERROR; goto out; }
    if (fstat(j.bfd, &bst) != 0) { r = TAG_WRITE_IO_ERROR; goto out; }
    if ((uint64_t) bst.st_size < 16 || !pread_all(j.bfd, h, 16, 0) || memcmp(h, TW_BACKUP_MAGIC, 8) != 0) {
        r = TAG_WRITE_PARSE_ERROR;
        goto out;
    }
    if (rd_le32(h + 8) == 2) {
        r = restore_v2(path, fmt, j.bfd, &bst);
        goto out;
    }
    if (rd_le32(h + 8) != 1 || (uint64_t) bst.st_size < TW_BACKUP_HDR + 8 || !pread_all(j.bfd, h, sizeof(h), 0)) {
        r = TAG_WRITE_PARSE_ERROR;
        goto out;
    }
    uint32_t bfmt = rd_le32(h + 12);
    uint64_t osize = rd_le64(h + 16), region = rd_le64(h + 24), alen = rd_le64(h + 32);
    j.b_audio_hash = rd_le64(h + 40);
    if ((bfmt != TW_FMT_FLAC && bfmt != TW_FMT_MP3) || region > osize || osize - region != alen ||
        (uint64_t) bst.st_size != TW_BACKUP_HDR + region + 8) {
        r = TAG_WRITE_PARSE_ERROR;
        goto out;
    }
    if ((int) bfmt != fmt) { r = TAG_WRITE_BAD_ARGS; goto out; }
    {
        uint64_t hh = FNV_INIT, pos = 0, want = TW_BACKUP_HDR + region;
        while (pos < want) {
            size_t n = want - pos < TW_IO_BUF ? (size_t) (want - pos) : TW_IO_BUF;
            if (!pread_all(j.bfd, buf, n, pos)) { r = TAG_WRITE_IO_ERROR; goto out; }
            hh = fnv64(hh, buf, n);
            pos += n;
        }
        uint8_t t[8];
        if (!pread_all(j.bfd, t, 8, want) || rd_le64(t) != hh) { r = TAG_WRITE_PARSE_ERROR; goto out; }
    }
    j.b_region = region;

    r = open_source(path, &j);
    if (r != TAG_WRITE_OK) goto out;
    r = audio_offset(j.src, j.size, fmt, &j.old_region);
    if (r != TAG_WRITE_OK) goto out;
    if (j.size - j.old_region != alen) { r = TAG_WRITE_VERIFY_FAILED; goto out; }
    j.new_region = region;
    r = run_job(&j, path, NULL);

out:
    free(buf);
    if (j.src >= 0) close(j.src);
    close(j.bfd);
    return r;
}
