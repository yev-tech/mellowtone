/* MusicBrainz client: release search and release lookup over ws/2 JSON.
 * One process-wide gate serialises requests and spaces them at least
 * MB_MIN_INTERVAL_MS apart (measured from the end of the previous one, so
 * the starts are spaced at least as far), as the service asks of every
 * client; a 503 is retried once after 2 s inside the same gate. */
#include "musicbrainz.h"
#include "branding.h"
#include "http_client.h"
#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MB_BASE "https://musicbrainz.org/ws/2/"
#define MB_RETRY_503_MS 2000u

static pthread_mutex_t mb_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t mb_last_request_ms; /* 0 = none yet; guarded by mb_gate_mutex */

static uint64_t mb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void mb_sleep_ms(uint64_t ms) {
    struct timespec ts = { .tv_sec = (time_t) (ms / 1000u), .tv_nsec = (long) (ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* Sleeps `ms` in short slices; false when cancelled meanwhile. */
static bool mb_sleep_cancelable(uint64_t ms, http_cancel_token_t * cancel) {
    while (ms > 0) {
        if (cancel && http_cancel_token_is_cancelled(cancel)) return false;
        uint64_t step = ms < 100u ? ms : 100u;
        mb_sleep_ms(step);
        ms -= step;
    }
    return !(cancel && http_cancel_token_is_cancelled(cancel));
}

/* Waits out the gate; caller holds mb_gate_mutex. False when cancelled. */
static bool mb_gate_wait(http_cancel_token_t * cancel) {
    if (mb_last_request_ms == 0) return !(cancel && http_cancel_token_is_cancelled(cancel));
    uint64_t now = mb_now_ms();
    uint64_t ready = mb_last_request_ms + MB_MIN_INTERVAL_MS;
    return mb_sleep_cancelable(now < ready ? ready - now : 0, cancel);
}

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

/* One GET with the gate held. The request/response structs are about
 * 27 KB each, so they live on the heap rather than a worker's stack. */
static mb_result_t mb_get(const char * url, char ** out_body, size_t * out_len, http_cancel_token_t * cancel) {
    *out_body = NULL;
    *out_len = 0;
    http_request_t * req = calloc(1, sizeof(*req));
    http_response_t * resp = calloc(1, sizeof(*resp));
    if (!req || !resp) {
        free(req);
        free(resp);
        return MB_NET_ERROR;
    }
    if (snprintf(req->url, sizeof(req->url), "%s", url) >= (int) sizeof(req->url)) {
        free(req);
        free(resp);
        return MB_NET_ERROR;
    }
    req->method = HTTP_METHOD_GET;
    req->verify_tls = true;
    req->connect_timeout_ms = 8000;
    req->read_timeout_ms = 10000;
    req->total_timeout_ms = MB_TIMEOUT_MS;
    req->max_response_bytes = MB_MAX_RESPONSE;
    req->redirect_limit = 2;
    req->user_agent = mt_user_agent();
    snprintf(req->headers[0].name, sizeof(req->headers[0].name), "Accept");
    snprintf(req->headers[0].value, sizeof(req->headers[0].value), "application/json");
    req->header_count = 1;

    mb_result_t r = MB_NET_ERROR;
    pthread_mutex_lock(&mb_gate_mutex);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!mb_gate_wait(cancel)) { r = MB_CANCELLED; break; }
        bool ok = http_request_ex(req, cancel, resp);
        mb_last_request_ms = mb_now_ms();
        if (cancel && http_cancel_token_is_cancelled(cancel)) { r = MB_CANCELLED; break; }
        if (!ok) { r = MB_NET_ERROR; break; }
        if (resp->status == 503) {
            r = MB_RATE_LIMITED;
            http_response_free(resp);
            if (attempt == 0 && !mb_sleep_cancelable(MB_RETRY_503_MS, cancel)) { r = MB_CANCELLED; break; }
            continue;
        }
        if (resp->status == 200 && resp->body) {
            char * body = realloc(resp->body, resp->body_len + 1);
            if (!body) { r = MB_NET_ERROR; break; }
            body[resp->body_len] = '\0';
            *out_body = body;
            *out_len = resp->body_len;
            resp->body = NULL;
            r = MB_OK;
        } else if (resp->status == 404 || resp->status == 400) {
            r = MB_NOT_FOUND;
        } else {
            r = MB_NET_ERROR;
        }
        break;
    }
    pthread_mutex_unlock(&mb_gate_mutex);
    http_response_free(resp);
    free(req);
    free(resp);
    return r;
}

mb_result_t mb_fetch_raw(const char * url, char ** out_body, size_t * out_len) {
    return mb_get(url, out_body, out_len, NULL);
}

/* ---- JSON helpers ---- */

static const char * json_str(const cJSON * obj, const char * key) {
    const cJSON * v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int json_int(const cJSON * obj, const char * key, int fallback) {
    const cJSON * v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (int) v->valuedouble : fallback;
}

static void copy_str(char * dst, size_t size, const char * src) {
    snprintf(dst, size, "%s", src ? src : "");
}

/* "A feat. B" from [{name, joinphrase}, ...]; the first artist's MBID. */
static void artist_credit(const cJSON * credits, char * out, size_t out_size, char * id, size_t id_size) {
    out[0] = '\0';
    if (id && id_size) id[0] = '\0';
    size_t pos = 0;
    const cJSON * c;
    cJSON_ArrayForEach(c, credits) {
        const char * name = json_str(c, "name");
        if (!name[0]) name = json_str(cJSON_GetObjectItemCaseSensitive(c, "artist"), "name");
        int n = snprintf(out + pos, out_size - pos, "%s%s", name, json_str(c, "joinphrase"));
        if (n < 0 || (size_t) n >= out_size - pos) { pos = out_size - 1; break; }
        pos += (size_t) n;
        if (id && id_size && !id[0]) copy_str(id, id_size, json_str(cJSON_GetObjectItemCaseSensitive(c, "artist"), "id"));
    }
}

/* "CD", "2×CD", "CD + DVD-Video" from the media list. */
static void media_format(const cJSON * media, char * out, size_t out_size) {
    const char * names[8];
    int counts[8], kinds = 0;
    const cJSON * m;
    cJSON_ArrayForEach(m, media) {
        const char * f = json_str(m, "format");
        if (!f[0]) f = "(unknown)";
        int i = 0;
        while (i < kinds && strcmp(names[i], f) != 0) i++;
        if (i == kinds) {
            if (kinds == 8) continue;
            names[kinds] = f;
            counts[kinds++] = 0;
        }
        counts[i]++;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < kinds && pos < out_size; i++) {
        int n = counts[i] > 1 ? snprintf(out + pos, out_size - pos, "%s%d\xC3\x97%s", i ? " + " : "", counts[i], names[i])
                              : snprintf(out + pos, out_size - pos, "%s%s", i ? " + " : "", names[i]);
        if (n < 0 || (size_t) n >= out_size - pos) break;
        pos += (size_t) n;
    }
}

static void parse_release_common(const cJSON * rel, mb_release_t * r) {
    memset(r, 0, sizeof(*r));
    copy_str(r->id, sizeof(r->id), json_str(rel, "id"));
    copy_str(r->title, sizeof(r->title), json_str(rel, "title"));
    artist_credit(cJSON_GetObjectItemCaseSensitive(rel, "artist-credit"), r->artist, sizeof(r->artist), r->artist_id,
                  sizeof(r->artist_id));
    copy_str(r->date, sizeof(r->date), json_str(rel, "date"));
    copy_str(r->country, sizeof(r->country), json_str(rel, "country"));
    const cJSON * media = cJSON_GetObjectItemCaseSensitive(rel, "media");
    media_format(media, r->format, sizeof(r->format));
    r->disc_count = cJSON_GetArraySize(media);
    int sum = 0;
    const cJSON * m;
    cJSON_ArrayForEach(m, media) sum += json_int(m, "track-count", 0);
    r->track_count = json_int(rel, "track-count", sum);
    r->mb_score = json_int(rel, "score", 100);
}

mb_result_t mb_parse_search_json(const char * json, size_t len, mb_release_t * out, int max, int * out_count) {
    *out_count = 0;
    cJSON * root = cJSON_ParseWithLength(json, len);
    if (!root) return MB_PARSE_ERROR;
    const cJSON * list = cJSON_GetObjectItemCaseSensitive(root, "releases");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return MB_PARSE_ERROR;
    }
    const cJSON * rel;
    cJSON_ArrayForEach(rel, list) {
        if (*out_count >= max) break;
        if (!json_str(rel, "id")[0]) continue;
        parse_release_common(rel, &out[(*out_count)++]);
    }
    cJSON_Delete(root);
    return *out_count > 0 ? MB_OK : MB_NOT_FOUND;
}

/* Most-voted genre (first one on a tie), Title Case. */
static bool pick_genre(const cJSON * genres, char * out, size_t out_size) {
    const char * best = NULL;
    int best_count = -1;
    const cJSON * g;
    cJSON_ArrayForEach(g, genres) {
        const char * name = json_str(g, "name");
        int count = json_int(g, "count", 0);
        if (name[0] && count > best_count) { best = name; best_count = count; }
    }
    if (!best) return false;
    copy_str(out, out_size, best);
    bool word_start = true;
    for (char * p = out; *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (word_start && c >= 'a' && c <= 'z') *p = (char) (c - 'a' + 'A');
        word_start = c == ' ' || c == '-' || c == '/' || c == '&' || c == '(';
    }
    return true;
}

static int track_cmp(const void * a, const void * b) {
    const mb_track_t * x = a, * y = b;
    if (x->disc != y->disc) return x->disc < y->disc ? -1 : 1;
    return x->number < y->number ? -1 : x->number > y->number;
}

mb_result_t mb_parse_release_json(const char * json, size_t len, mb_release_detail_t * out) {
    memset(out, 0, sizeof(*out));
    cJSON * root = cJSON_ParseWithLength(json, len);
    if (!root) return MB_PARSE_ERROR;
    if (!json_str(root, "id")[0]) {
        cJSON_Delete(root);
        return MB_PARSE_ERROR;
    }
    parse_release_common(root, &out->release);
    snprintf(out->release_group_id, sizeof(out->release_group_id), "%s",
             json_str(cJSON_GetObjectItemCaseSensitive(root, "release-group"), "id"));
    if (!pick_genre(cJSON_GetObjectItemCaseSensitive(root, "genres"), out->genre, sizeof(out->genre))) {
        const cJSON * rg = cJSON_GetObjectItemCaseSensitive(root, "release-group");
        pick_genre(cJSON_GetObjectItemCaseSensitive(rg, "genres"), out->genre, sizeof(out->genre));
    }

    /* Video tracks (a bonus DVD's clips) never match an audio file, so
     * they are left out, and so is a medium that has nothing else. */
    const cJSON * media = cJSON_GetObjectItemCaseSensitive(root, "media");
    int total = 0, discs = 0, max_disc = 0;
    const cJSON * m, * t;
    cJSON_ArrayForEach(m, media) total += cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(m, "tracks"));
    if (total > 0) {
        out->tracks = calloc((size_t) total, sizeof(mb_track_t));
        if (!out->tracks) {
            cJSON_Delete(root);
            return MB_PARSE_ERROR;
        }
    }
    int disc_index = 0;
    cJSON_ArrayForEach(m, media) {
        disc_index++;
        int disc = json_int(m, "position", disc_index);
        int track_index = 0, before = out->track_count;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(m, "tracks")) {
            if (out->track_count >= total) break;
            track_index++;
            const cJSON * rec = cJSON_GetObjectItemCaseSensitive(t, "recording");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "video"))) continue;
            mb_track_t * tr = &out->tracks[out->track_count++];
            tr->disc = disc;
            tr->number = json_int(t, "position", track_index);
            const char * title = json_str(t, "title");
            copy_str(tr->title, sizeof(tr->title), title[0] ? title : json_str(rec, "title"));
            const cJSON * ac = cJSON_GetObjectItemCaseSensitive(t, "artist-credit");
            if (cJSON_GetArraySize(ac) == 0) ac = cJSON_GetObjectItemCaseSensitive(rec, "artist-credit");
            artist_credit(ac, tr->artist, sizeof(tr->artist), tr->artist_id, sizeof(tr->artist_id));
            if (!tr->artist[0]) {
                copy_str(tr->artist, sizeof(tr->artist), out->release.artist);
                copy_str(tr->artist_id, sizeof(tr->artist_id), out->release.artist_id);
            }
            copy_str(tr->recording_id, sizeof(tr->recording_id), json_str(rec, "id"));
            copy_str(tr->track_id, sizeof(tr->track_id), json_str(t, "id"));
            tr->length_ms = json_int(t, "length", 0);
            if (tr->length_ms <= 0) tr->length_ms = json_int(rec, "length", 0);
            if (tr->length_ms < 0) tr->length_ms = 0;
        }
        if (out->track_count > before) {
            discs++;
            if (disc > max_disc) max_disc = disc;
        }
    }
    cJSON_Delete(root);
    if (out->track_count > 1) qsort(out->tracks, (size_t) out->track_count, sizeof(mb_track_t), track_cmp);
    out->disc_count = discs > max_disc ? discs : max_disc;
    out->release.track_count = out->track_count;
    out->release.mb_score = 100;
    return MB_OK;
}

void mb_release_detail_free(mb_release_detail_t * d) {
    if (!d) return;
    free(d->tracks);
    d->tracks = NULL;
    d->track_count = 0;
}

/* ---- Query building ---- */

/* Lucene-escapes `in`, appending to out at *pos. The words AND/OR/NOT are
 * lower-cased so they stay words rather than operators (search is
 * case-insensitive anyway). */
static void lucene_append(const char * in, char * out, size_t out_size, size_t * pos) {
    static const char special[] = "+-&|!(){}[]^\"~*?:\\/";
    const char * p = in;
    while (*p && *pos + 3 < out_size) {
        bool word_start = p == in || p[-1] == ' ';
        if (word_start) {
            size_t wl = strcspn(p, " ");
            if ((wl == 3 && (strncmp(p, "AND", 3) == 0 || strncmp(p, "NOT", 3) == 0)) ||
                (wl == 2 && strncmp(p, "OR", 2) == 0)) {
                for (size_t i = 0; i < wl && *pos + 1 < out_size; i++) out[(*pos)++] = (char) (p[i] - 'A' + 'a');
                p += wl;
                continue;
            }
        }
        unsigned char c = (unsigned char) *p++;
        if (c < 0x20) c = ' ';
        if (strchr(special, c) && c) out[(*pos)++] = '\\';
        out[(*pos)++] = (char) c;
    }
    out[*pos] = '\0';
}

static void append_raw(const char * s, char * out, size_t out_size, size_t * pos) {
    int n = snprintf(out + *pos, out_size - *pos, "%s", s);
    if (n > 0) *pos = (size_t) n < out_size - *pos ? *pos + (size_t) n : out_size - 1;
}

static bool has_text(const char * s) {
    if (!s) return false;
    for (; *s; s++)
        if (*s != ' ') return true;
    return false;
}

void mb_build_search_query(const char * album, const char * artist, int track_count, char * out, size_t out_size) {
    size_t pos = 0;
    out[0] = '\0';
    append_raw("+release:(", out, out_size, &pos);
    lucene_append(album ? album : "", out, out_size, &pos);
    append_raw(")", out, out_size, &pos);
    if (has_text(artist)) {
        append_raw(" artist:(", out, out_size, &pos);
        lucene_append(artist, out, out_size, &pos);
        append_raw(")", out, out_size, &pos);
    }
    if (track_count > 0) {
        char t[24];
        snprintf(t, sizeof(t), " tracks:%d", track_count);
        append_raw(t, out, out_size, &pos);
    }
}

mb_result_t mb_search_releases(const char * album, const char * artist, int track_count, mb_release_t * out,
                               int max, int * out_count, http_cancel_token_t * cancel) {
    *out_count = 0;
    if (!has_text(album) || !out || max <= 0) return MB_NOT_FOUND;
    if (max > MB_SEARCH_MAX) max = MB_SEARCH_MAX;
    char query[1024], encoded[3 * 1024], url[2048];
    mb_build_search_query(album, artist, track_count, query, sizeof(query));
    url_encode(query, encoded, sizeof(encoded));
    if (snprintf(url, sizeof(url), MB_BASE "release/?query=%s&fmt=json&limit=%d", encoded, max) >= (int) sizeof(url))
        return MB_NOT_FOUND;
    char * body = NULL;
    size_t len = 0;
    mb_result_t r = mb_get(url, &body, &len, cancel);
    if (r != MB_OK) return r;
    r = mb_parse_search_json(body, len, out, max, out_count);
    free(body);
    return r;
}

static bool mbid_is_valid(const char * id) {
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) return false;
    }
    return n == 36;
}

mb_result_t mb_get_release(const char * release_id, mb_release_detail_t * out, http_cancel_token_t * cancel) {
    memset(out, 0, sizeof(*out));
    if (!release_id || !mbid_is_valid(release_id)) return MB_NOT_FOUND;
    char url[256];
    snprintf(url, sizeof(url),
             MB_BASE "release/%s?inc=recordings+artist-credits+media+genres+release-groups&fmt=json", release_id);
    char * body = NULL;
    size_t len = 0;
    mb_result_t r = mb_get(url, &body, &len, cancel);
    if (r != MB_OK) return r;
    r = mb_parse_release_json(body, len, out);
    free(body);
    return r;
}
