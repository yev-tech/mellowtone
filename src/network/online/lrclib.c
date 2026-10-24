/* LRCLIB client: /api/get with the full signature first (exact match,
 * duration +-2 s on the server), then /api/search on title + artist and
 * the closest-duration hit that carries synced lyrics. The result is saved
 * as the track's same-basename .lrc, which lyrics_load_sidecar() reads;
 * an existing sidecar is never overwritten. */
#include "lrclib.h"
#include "http_client.h"
#include "lyrics.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LRCLIB_BASE "https://lrclib.net/api/"
#define LRCLIB_MAX_RESPONSE (512u * 1024u)

static void url_encode(const char * in, char * out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char * p = (const unsigned char *) in; *p && pos + 4 < out_size; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[pos++] = (char) *p;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[*p >> 4];
            out[pos++] = hex[*p & 0xF];
        }
    }
    out[pos] = '\0';
}

/* Returns the parsed JSON for a 200 response, NULL otherwise; *net_ok is
 * false only when no HTTP response came back at all, and *busy is set for
 * an error status other than 404 (rate limited, server error), which must
 * not read as "no lyrics". */
static cJSON * lrclib_get_json(const char * url, bool * net_ok, bool * busy) {
    int status = 0;
    uint8_t * body = NULL;
    size_t size = 0;
    if (!http_get_to_buffer_limited(url, true, LRCLIB_MAX_RESPONSE, &status, &body, &size)) {
        *net_ok = false;
        free(body);
        return NULL;
    }
    if (status != 200 && status != 404) *busy = true;
    cJSON * json = status == 200 && body ? cJSON_ParseWithLength((const char *) body, size) : NULL;
    free(body);
    return json;
}

static const char * json_text(const cJSON * obj, const char * key) {
    const cJSON * v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static bool sidecar_path(const char * track_path, char * out, size_t out_size) {
    snprintf(out, out_size, "%s", track_path);
    char * dot = strrchr(out, '.');
    char * slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    size_t n = strlen(out);
    if (n + 5 > out_size) return false;
    memcpy(out + n, ".lrc", 5);
    return true;
}

/* Synced lyrics are checked to parse; plain ones are written as they are
 * (the lyrics view shows a timestamp-free .lrc as static text). */
static lrclib_result_t write_sidecar(const char * track_path, const char * lrc, bool synced) {
    if (synced) {
        lyrics_doc_t doc;
        if (!lyrics_parse_buffer(lrc, strlen(lrc), &doc)) return LRCLIB_PLAIN_ONLY;
        lyrics_doc_free(&doc);
    }
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    if (!sidecar_path(track_path, path, sizeof(path))) return LRCLIB_WRITE_ERROR;
    if (access(path, F_OK) == 0) return LRCLIB_EXISTS;
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return LRCLIB_WRITE_ERROR;
    size_t len = strlen(lrc), done = 0;
    while (done < len) {
        ssize_t w = write(fd, lrc + done, len - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t) w;
    }
    bool ok = done == len && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    /* link() fails rather than replace a sidecar that appeared meanwhile;
     * exFAT has no hard links, so fall back to rename after a re-check. */
    if (ok && link(tmp, path) != 0) ok = access(path, F_OK) != 0 && rename(tmp, path) == 0;
    unlink(tmp);
    return ok ? (synced ? LRCLIB_OK : LRCLIB_OK_PLAIN) : LRCLIB_WRITE_ERROR;
}

lrclib_result_t lrclib_fetch_sidecar(const char * track_path, const char * title, const char * artist,
                                     const char * album, int duration_s) {
    if (!track_path || !title || !title[0]) return LRCLIB_NOT_FOUND;
    char t[512], a[512], al[512], url[2048];
    url_encode(title, t, sizeof(t));
    url_encode(artist ? artist : "", a, sizeof(a));
    url_encode(album ? album : "", al, sizeof(al));
    bool net_ok = true;
    bool busy = false;

    if (artist && artist[0] && duration_s > 0) {
        snprintf(url, sizeof(url), LRCLIB_BASE "get?track_name=%s&artist_name=%s&album_name=%s&duration=%d", t, a,
                 al, duration_s);
        cJSON * hit = lrclib_get_json(url, &net_ok, &busy);
        if (hit) {
            const char * synced = json_text(hit, "syncedLyrics");
            lrclib_result_t r = synced ? write_sidecar(track_path, synced, true) : LRCLIB_PLAIN_ONLY;
            if (r == LRCLIB_PLAIN_ONLY && json_text(hit, "plainLyrics")) {
                /* Exact match with only unsynced text: still the right song. */
                r = write_sidecar(track_path, json_text(hit, "plainLyrics"), false);
            }
            cJSON_Delete(hit);
            if (r != LRCLIB_PLAIN_ONLY) return r;
        }
        if (!net_ok) return LRCLIB_NET_ERROR;
        if (busy) return LRCLIB_BUSY;
    }

    if (artist && artist[0])
        snprintf(url, sizeof(url), LRCLIB_BASE "search?track_name=%s&artist_name=%s", t, a);
    else
        snprintf(url, sizeof(url), LRCLIB_BASE "search?q=%s", t);
    cJSON * list = lrclib_get_json(url, &net_ok, &busy);
    if (!list) return !net_ok ? LRCLIB_NET_ERROR : busy ? LRCLIB_BUSY : LRCLIB_NOT_FOUND;
    const char * best = NULL, * best_plain = NULL;
    double best_plain_delta = 1e9;
    double best_delta = 1e9;
    const cJSON * item;
    cJSON_ArrayForEach(item, list) {
        const char * synced = json_text(item, "syncedLyrics");
        const char * plain = json_text(item, "plainLyrics");
        const cJSON * d = cJSON_GetObjectItemCaseSensitive(item, "duration");
        double delta = duration_s > 0 && cJSON_IsNumber(d) ? d->valuedouble - duration_s : 0;
        if (delta < 0) delta = -delta;
        /* A different cut (radio edit, live) would drift out of sync. */
        if (duration_s > 0 && cJSON_IsNumber(d) && delta > 5) continue;
        if (synced && delta < best_delta) { best = synced; best_delta = delta; }
        if (!synced && plain && delta < best_plain_delta) { best_plain = plain; best_plain_delta = delta; }
    }
    lrclib_result_t r = best ? write_sidecar(track_path, best, true)
                      : best_plain ? write_sidecar(track_path, best_plain, false) : LRCLIB_NOT_FOUND;
    cJSON_Delete(list);
    return r;
}
