#include "http_stream.h"
#include "branding.h"
#include "http_client.h"
#include "http_conn.h"
#include "debug_log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* Sized for several real seconds of cushion at typical internet-radio
 * bitrates (a 128kbps MP3 stream is ~16KB/s, so this is ~16s) -- far more
 * slack than audio.c's own ~93ms ALSA hardware buffer alone could ever
 * provide against a network hiccup. Not tuned against a real device yet;
 * a first-pass sizing, not a measured constant. */
#define STREAM_RING_CAPACITY (256 * 1024)
#define STREAM_MAX_REDIRECTS 5
/* Longest header line and redirect target; a Location that does not fit
 * is never followed. */
#define STREAM_URL_MAX 4096
/* A dropped Wi-Fi link must end the stream rather than hang the audio
 * thread: connecting gives up after STREAM_CONNECT_TIMEOUT_MS, and a read
 * that sees no data for STREAM_READ_TIMEOUT_MS ends the stream, so the
 * decoder sees its end and Stop/Next keep working. */
#define STREAM_CONNECT_TIMEOUT_MS 10000
#define STREAM_READ_TIMEOUT_MS 15000

struct http_stream {
    http_conn_t conn;
    /* Holds the socket while it is open: http_stream_close() cancels it to
     * unblock a pending read, and http_conn_close() unregisters the fd under
     * the token's lock, so a closed (and possibly reused) fd is never shut
     * down. */
    http_cancel_token_t cancel;
    http_conn_reader_t reader; /* only used for chunked framing / leftover header bytes -- see stream_thread_func() */
    bool is_chunked;
    char content_type[128];

    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond; /* signaled on: data pushed, space freed, or stop_requested/ended set */

    uint8_t * ring;
    size_t ring_head; /* next write position */
    size_t ring_tail; /* next read position */
    size_t ring_fill;

    bool stop_requested; /* caller asked to close */
    bool ended;           /* pump thread hit EOF/error -- no more data will ever arrive */
};

/* Pushes len bytes into the ring buffer, blocking (waiting for the consumer
 * to free up space) as needed. Returns false if stop_requested was seen
 * while waiting -- caller (the pump thread) should abandon its read loop. */
static bool push_to_ring(http_stream_t * s, const uint8_t * data, size_t len) {
    size_t off = 0;
    while (off < len) {
        pthread_mutex_lock(&s->mutex);
        while (s->ring_fill == STREAM_RING_CAPACITY && !s->stop_requested) {
            pthread_cond_wait(&s->cond, &s->mutex);
        }
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->mutex);
            return false;
        }
        size_t space = STREAM_RING_CAPACITY - s->ring_fill;
        size_t take = (len - off) < space ? (len - off) : space;
        for (size_t i = 0; i < take; i++) {
            s->ring[s->ring_head] = data[off + i];
            s->ring_head = (s->ring_head + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill += take;
        off += take;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    return true;
}

static void * stream_thread_func(void * arg) {
    http_stream_t * s = arg;
    uint8_t chunk[HTTP_CONN_READ_CHUNK];

    for (;;) {
        pthread_mutex_lock(&s->mutex);
        bool stop = s->stop_requested;
        pthread_mutex_unlock(&s->mutex);
        if (stop) break;

        if (s->is_chunked) {
            /* Some servers front a live stream with a reverse proxy that
             * chunk-encodes it -- each chunk is just a bounded slice of the
             * ongoing stream, not a sign the stream itself is finite. Only
             * a genuine zero-size chunk (or a read failure) ends the pump. */
            char size_line[64];
            if (!http_conn_reader_line(&s->reader, size_line, sizeof(size_line))) break;
            char * semi = strchr(size_line, ';');
            if (semi) *semi = '\0';
            char * end;
            uint64_t chunk_size = strtoull(size_line, &end, 16);
            if (end == size_line) break; /* no hex digits: not a chunk size */
            if (chunk_size == 0) break;

            uint64_t remaining = chunk_size;
            bool ok = true;
            while (remaining > 0) {
                size_t take = remaining < sizeof(chunk) ? (size_t) remaining : sizeof(chunk);
                if (!http_conn_reader_read_exact(&s->reader, chunk, take)) { ok = false; break; }
                if (!push_to_ring(s, chunk, take)) { ok = false; break; }
                remaining -= take;
            }
            if (!ok) break;

            char crlf[4];
            if (!http_conn_reader_line(&s->reader, crlf, sizeof(crlf))) break;
        } else {
            /* Read raw stream bytes, draining reader's buffered bytes first. */
            int n = http_conn_reader_read_some(&s->reader, chunk, sizeof(chunk));
            if (n <= 0) break;
            if (!push_to_ring(s, chunk, (size_t) n)) break;
        }
    }

    /* Mark stream ended and close network connection under mutex. */
    pthread_mutex_lock(&s->mutex);
    s->ended = true;
    pthread_cond_broadcast(&s->cond);
    http_conn_close(&s->conn);
    pthread_mutex_unlock(&s->mutex);

    return NULL;
}

/* Frees a stream whose pump thread was never started. */
static void stream_free_unstarted(http_stream_t * s) {
    http_conn_close(&s->conn);
    http_cancel_token_destroy(&s->cancel);
    free(s);
}

/* Opens url. A redirect that is to be followed returns NULL with its
 * target in next_url (next_url[0] is '\0' otherwise); http_stream_open()
 * loops over these, so the large header buffers here are on the stack once. */
static http_stream_t * http_stream_open_internal(const char * url, bool verify_tls, int redirects_left,
                                                 char * next_url, size_t next_url_size) {
    next_url[0] = '\0';
    bool is_https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        DBG_LOG("http_stream: parse_url failed for '%s'\n", url);
        return NULL;
    }

    http_stream_t * s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    http_cancel_token_init(&s->cancel);

    if (!http_conn_open_ex(&s->conn, host, port, is_https, verify_tls, STREAM_CONNECT_TIMEOUT_MS,
                           STREAM_READ_TIMEOUT_MS, &s->cancel, NULL)) {
        DBG_LOG("http_stream: conn_open failed for host='%s' port='%s' https=%d\n", host, port, is_https);
        stream_free_unstarted(s);
        return NULL;
    }

    char request[2560];
    /* Icy-MetaData: 0 explicitly opts out of Shoutcast/Icecast's own
     * metadata-interleaving extension (periodic non-audio blocks spliced
     * into the byte stream at a server-chosen interval) -- without this,
     * a server that defaults it on would corrupt dr_mp3's view of the
     * stream, since those blocks aren't valid MP3 frame data. */
    int req_len = snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                            "Icy-MetaData: 0\r\nConnection: close\r\n\r\n",
                            path, host, mt_user_agent());
    if (req_len < 0 || (size_t) req_len >= sizeof(request)) {
        DBG_LOG("http_stream: request too long for path='%s'\n", path);
        stream_free_unstarted(s);
        return NULL;
    }

    size_t sent = 0;
    while (sent < (size_t) req_len) {
        int n = http_conn_write(&s->conn, (const uint8_t *) request + sent, (size_t) req_len - sent);
        if (n <= 0) {
            DBG_LOG("http_stream: conn_write failed (n=%d) after %zu/%d bytes\n", n, sent, req_len);
            stream_free_unstarted(s);
            return NULL;
        }
        sent += (size_t) n;
    }

    memset(&s->reader, 0, sizeof(s->reader));
    s->reader.conn = &s->conn;

    char status_line[256];
    if (!http_conn_reader_line(&s->reader, status_line, sizeof(status_line))) {
        DBG_LOG("http_stream: failed to read status line from host='%s'\n", host);
        stream_free_unstarted(s);
        return NULL;
    }
    DBG_LOG("http_stream: status line: '%s'\n", status_line);

    const char * sp = strchr(status_line, ' ');
    int status = sp ? atoi(sp + 1) : 0;

    /* Signed stream URLs in Location can be long; a header that still does
     * not fit is never acted on (see redirect_truncated). */
    char header_line[STREAM_URL_MAX];
    char redirect_url[STREAM_URL_MAX] = "";
    bool redirect_truncated = false;
    while (http_conn_reader_line(&s->reader, header_line, sizeof(header_line)) && header_line[0] != '\0') {
        bool truncated = s->reader.line_truncated;
        char * colon = strchr(header_line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char * value = colon + 1;
        while (*value == ' ') value++;

        if (strcasecmp(header_line, "Transfer-Encoding") == 0 && strcasecmp(value, "chunked") == 0) {
            s->is_chunked = true;
        } else if (strcasecmp(header_line, "Content-Type") == 0) {
            char * semi = strchr(value, ';');
            if (semi) *semi = '\0';
            snprintf(s->content_type, sizeof(s->content_type), "%s", value);
        } else if (strcasecmp(header_line, "Location") == 0) {
            redirect_truncated = truncated;
            snprintf(redirect_url, sizeof(redirect_url), "%s", value);
            if (truncated) DBG_LOG("http_stream: Location header too long, not following it\n");
        }
    }

    /* Radio aggregators commonly return a short-lived, signed stream URL
     * via 301/302/307/308 rather than audio in the first response (Zeno/
     * SurferNetwork is one real example).  Unlike ordinary finite HTTP
     * downloads there is no useful stable URL for Radio.txt to substitute:
     * the Location token expires after about a minute, so redirects must be
     * followed when playback starts. Keep this bounded to prevent loops.
     * Location is deliberately required to be absolute http(s) here;
     * http_conn_parse_url() then applies the same scheme/host validation as
     * the original request, while ambiguous relative redirects fail cleanly. */
    if (status >= 300 && status < 400 && redirect_url[0] != '\0') {
        stream_free_unstarted(s);
        if (redirect_truncated || redirects_left <= 0 ||
            (strncasecmp(redirect_url, "http://", 7) != 0 && strncasecmp(redirect_url, "https://", 8) != 0)) {
            DBG_LOG("http_stream: refused redirect status=%d location='%s' remaining=%d\n",
                    status, redirect_url, redirects_left);
            return NULL;
        }
        DBG_LOG("http_stream: following redirect status=%d to '%s'\n", status, redirect_url);
        snprintf(next_url, next_url_size, "%s", redirect_url);
        return NULL;
    }

    if (status < 200 || status >= 300) {
        DBG_LOG("http_stream: non-2xx status %d from host='%s'\n", status, host);
        stream_free_unstarted(s);
        return NULL;
    }
    DBG_LOG("http_stream: status=%d content_type='%s' chunked=%d\n", status, s->content_type, s->is_chunked);

    s->ring = malloc(STREAM_RING_CAPACITY);
    if (!s->ring) {
        stream_free_unstarted(s);
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);

    if (pthread_create(&s->thread, NULL, stream_thread_func, s) != 0) {
        DBG_LOG("http_stream: pthread_create failed\n");
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        free(s->ring);
        stream_free_unstarted(s);
        return NULL;
    }

    return s;
}

http_stream_t * http_stream_open(const char * url, bool verify_tls) {
    char * current = malloc(STREAM_URL_MAX);
    char * next = malloc(STREAM_URL_MAX);
    http_stream_t * s = NULL;
    if (current && next && snprintf(current, STREAM_URL_MAX, "%s", url) < STREAM_URL_MAX) {
        for (int redirects_left = STREAM_MAX_REDIRECTS; redirects_left >= 0; redirects_left--) {
            s = http_stream_open_internal(current, verify_tls, redirects_left, next, STREAM_URL_MAX);
            if (s || !next[0]) break;
            char * swap = current;
            current = next;
            next = swap;
        }
    }
    free(current);
    free(next);
    return s;
}

size_t http_stream_read(http_stream_t * s, void * buf, size_t n) {
    if (!s || n == 0) return 0;
    uint8_t * out = buf;
    size_t total = 0;

    pthread_mutex_lock(&s->mutex);
    while (total < n) {
        while (s->ring_fill == 0 && !s->ended && !s->stop_requested) {
            pthread_cond_wait(&s->cond, &s->mutex);
        }
        if (s->ring_fill == 0) break; /* ended or stop_requested -- no more data will ever arrive */

        size_t take = (n - total) < s->ring_fill ? (n - total) : s->ring_fill;
        for (size_t i = 0; i < take; i++) {
            out[total + i] = s->ring[s->ring_tail];
            s->ring_tail = (s->ring_tail + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill -= take;
        total += take;
        pthread_cond_broadcast(&s->cond); /* wake the pump thread if it's waiting for space */
    }
    pthread_mutex_unlock(&s->mutex);

    return total;
}

const char * http_stream_content_type(http_stream_t * s) {
    return s ? s->content_type : "";
}

void http_stream_close(http_stream_t * s) {
    if (!s) return;

    pthread_mutex_lock(&s->mutex);
    s->stop_requested = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    /* Unblocks a pending socket read; does nothing once the pump thread
     * has closed the connection (the fd is unregistered then). */
    http_cancel_token_cancel(&s->cancel);

    pthread_join(s->thread, NULL);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    http_cancel_token_destroy(&s->cancel);
    free(s->ring);
    free(s);
}
