/* MusicBrainz web service client (https://musicbrainz.org/ws/2/, JSON).
 *
 * Blocking calls meant for a worker thread. Every request goes through one
 * process-wide gate that keeps at least MB_MIN_INTERVAL_MS between
 * requests (the service allows one per second per client) and sends the
 * mt_user_agent() string (branding.h); no account or e-mail is ever sent. An HTTP 503
 * (rate limited) is retried once after 2 s.
 *
 * Memory: a release lookup response is bounded to MB_MAX_RESPONSE bytes
 * and parsed with cJSON; the parsed tree is freed before returning, so only
 * the mb_track_t array (about 1.1 KB per track) outlives a call. */
#ifndef MUSICBRAINZ_H
#define MUSICBRAINZ_H

#include <stdbool.h>
#include <stddef.h>

#define MB_MIN_INTERVAL_MS 1100u
#define MB_MAX_RESPONSE (2u * 1024u * 1024u)
#define MB_TIMEOUT_MS 15000u
/* Upper bound on search results per request (the service caps at 100). */
#define MB_SEARCH_MAX 25

typedef struct {
    char id[40];            /* release MBID */
    char title[256];
    char artist[256];       /* artist-credit phrase, e.g. "Various Artists" */
    char artist_id[40];     /* first credited artist MBID (or Various Artists' MBID) */
    char date[16];          /* "YYYY", "YYYY-MM" or "YYYY-MM-DD" or "" */
    char country[8];
    char format[48];        /* e.g. "CD", "Digital Media", "2×CD", "CD + DVD-Video" */
    int track_count;        /* total over all media */
    int disc_count;
    int mb_score;           /* search score 0..100 (100 for a lookup) */
} mb_release_t;

typedef struct {
    int disc;               /* 1-based medium position */
    int number;             /* 1-based position within the medium */
    char title[256];
    char artist[256];       /* track artist-credit phrase */
    char artist_id[40];
    char recording_id[40];
    char track_id[40];      /* release track MBID */
    int length_ms;          /* 0 if unknown */
} mb_track_t;

/* Video tracks (recording.video) are left out of `tracks`, and a medium
 * with only video tracks does not count in disc_count; release.format still
 * names every medium. */
typedef struct {
    mb_release_t release;
    char genre[64];         /* most-voted release or release-group genre, Title Case, "" if none */
    char release_group_id[40]; /* the release group MBID ("" if absent): its cover covers every edition */
    mb_track_t * tracks;    /* malloc'd, ordered by disc then number */
    int track_count;
    int disc_count;
} mb_release_detail_t;

typedef enum { MB_OK, MB_NOT_FOUND, MB_NET_ERROR, MB_PARSE_ERROR, MB_RATE_LIMITED, MB_CANCELLED } mb_result_t;

/* http_client.h's http_cancel_token_t. Cancelling it aborts the request in
 * flight and any wait at the rate gate; the call then returns MB_CANCELLED.
 * NULL: not cancelable. */
struct http_cancel_token;

/* Release search by album title and (optional) artist. The query is
 *   +release:(<album words>) artist:(<artist words>) tracks:<N>
 * so only the album words are required; the artist and track count raise
 * the ranking without excluding near matches (a compilation tagged with a
 * track artist still finds the "Various Artists" release). User text is
 * Lucene-escaped. Up to `max` (<= MB_SEARCH_MAX) results, best first, in
 * out; count in *out_count. MB_NOT_FOUND when the search returned nothing
 * or album is empty. */
mb_result_t mb_search_releases(const char * album, const char * artist, int track_count, mb_release_t * out,
                               int max, int * out_count, struct http_cancel_token * cancel);

/* Full release with media/tracks, artist credits and genres
 * (/ws/2/release/<id>?inc=recordings+artist-credits+media+genres+release-groups);
 * the genre falls back to the release group's when the release has none.
 * On success *out owns a malloc'd track array: mb_release_detail_free(). */
mb_result_t mb_get_release(const char * release_id, mb_release_detail_t * out, struct http_cancel_token * cancel);
void mb_release_detail_free(mb_release_detail_t * d);

/* ---- Helpers, exposed for tests and fixtures (no network) ---- */

/* Parses a /ws/2/release search response. */
mb_result_t mb_parse_search_json(const char * json, size_t len, mb_release_t * out, int max, int * out_count);
/* Parses a /ws/2/release/<id> lookup response (with the inc= above). */
mb_result_t mb_parse_release_json(const char * json, size_t len, mb_release_detail_t * out);
/* Writes the Lucene query (not URL-encoded) used by mb_search_releases(). */
void mb_build_search_query(const char * album, const char * artist, int track_count, char * out, size_t out_size);
/* Raw GET of a ws/2 URL through the rate gate (for fixture capture).
 * *out_body is malloc'd and NUL-terminated on MB_OK; caller frees it. */
mb_result_t mb_fetch_raw(const char * url, char ** out_body, size_t * out_len);

#endif /* MUSICBRAINZ_H */
