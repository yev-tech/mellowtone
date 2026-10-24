/* HTTP client and stream framing against a one-shot local server:
 *
 * - a body without Content-Length or chunking runs to the end of the
 *   connection, bounded by the caller's limit;
 * - a chunk-size line without hex digits is a framing error, not the end;
 * - statuses without a body (204) read none;
 * - a Location header too long for the buffer is never followed;
 * - the radio stream reads to the end, ends on a bad chunk line, and
 *   http_stream_close() unblocks a pump thread waiting on a silent server. */
#include "http_client.h"
#include "http_stream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            fprintf(stderr, __VA_ARGS__);                                                                          \
            fputc('\n', stderr);                                                                                   \
        }                                                                                                          \
    } while (0)

/* ---- one-shot server ------------------------------------------------------------ */

typedef struct {
    int listen_fd;
    const char * response;
    size_t response_len;
    int hold_ms; /* keep the connection open (silent) this long after the response */
    pthread_t thread;
} server_t;

static void * server_thread(void * arg) {
    server_t * srv = arg;
    int fd = accept(srv->listen_fd, NULL, NULL);
    if (fd < 0) return NULL;
    /* The request head, up to its blank line. */
    char buf[4096];
    size_t got = 0;
    while (got + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t) n;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    size_t sent = 0;
    while (sent < srv->response_len) {
        ssize_t n = send(fd, srv->response + sent, srv->response_len - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t) n;
    }
    if (srv->hold_ms > 0) {
        struct timespec ts = { srv->hold_ms / 1000, (long) (srv->hold_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    close(fd);
    return NULL;
}

/* Starts a server for one connection; url gets its address. */
static void server_start(server_t * srv, const char * response, size_t len, int hold_ms, char * url, size_t url_size) {
    memset(srv, 0, sizeof(*srv));
    srv->response = response;
    srv->response_len = len;
    srv->hold_ms = hold_ms;
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = 0 };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t addr_len = sizeof(addr);
    if (bind(srv->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv->listen_fd, 1) != 0 ||
        getsockname(srv->listen_fd, (struct sockaddr *) &addr, &addr_len) != 0) {
        perror("server");
        exit(2);
    }
    snprintf(url, url_size, "http://127.0.0.1:%u/x", (unsigned) ntohs(addr.sin_port));
    pthread_create(&srv->thread, NULL, server_thread, srv);
}

static void server_stop(server_t * srv) {
    pthread_join(srv->thread, NULL);
    close(srv->listen_fd);
}

static bool get(const char * response, size_t limit, int * status, uint8_t ** body, size_t * len) {
    server_t srv;
    char url[64];
    server_start(&srv, response, strlen(response), 0, url, sizeof(url));
    bool ok = http_get_to_buffer_limited(url, false, limit, status, body, len);
    server_stop(&srv);
    return ok;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* ---- client --------------------------------------------------------------------- */

static void test_client(void) {
    int status = 0;
    uint8_t * body = NULL;
    size_t len = 0;

    bool ok = get("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the end", 1024, &status, &body, &len);
    CHECK(ok && status == 200 && len == 13 && memcmp(body, "until the end", 13) == 0,
          "body without length read to EOF (ok=%d len=%zu)", ok, len);
    free(body);

    ok = get("HTTP/1.1 200 OK\r\n\r\n0123456789abcdef", 8, &status, &body, &len);
    CHECK(!ok && body == NULL, "body without length bounded by the limit");
    free(body);

    ok = get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n", 1024, &status, &body,
             &len);
    CHECK(ok && len == 4 && memcmp(body, "abcd", 4) == 0, "chunked body");
    free(body);

    ok = get("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\nzz\r\n\r\n", 1024, &status, &body,
             &len);
    CHECK(!ok, "chunk line without hex digits is an error, not the last chunk");
    free(body);

    ok = get("HTTP/1.1 204 No Content\r\n\r\n", 1024, &status, &body, &len);
    CHECK(ok && status == 204 && len == 0, "204 has no body");
    free(body);

    /* A Location longer than the client can follow is not followed. */
    char * response = malloc(8192);
    char * long_url = malloc(4096);
    memset(long_url, 'a', 3000);
    long_url[3000] = '\0';
    snprintf(response, 8192, "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/%s\r\nContent-Length: 0\r\n\r\n",
             long_url);
    server_t srv;
    char url[64];
    server_start(&srv, response, strlen(response), 0, url, sizeof(url));
    http_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.url, sizeof(req.url), "%s", url);
    req.method = HTTP_METHOD_GET;
    req.connect_timeout_ms = 2000;
    req.read_timeout_ms = 2000;
    req.redirect_limit = 3;
    http_response_t resp;
    ok = http_request_ex(&req, NULL, &resp);
    CHECK(ok && resp.status == 302, "long Location returned as is, not followed (ok=%d status=%d err=%s)", ok,
          resp.status, resp.error ? resp.error : "");
    http_response_free(&resp);
    server_stop(&srv);
    free(long_url);
    free(response);
}

/* ---- stream --------------------------------------------------------------------- */

static size_t read_all(http_stream_t * s, char * out, size_t size) {
    size_t total = 0;
    for (;;) {
        size_t n = http_stream_read(s, out + total, size - 1 - total);
        if (n == 0) break;
        total += n;
    }
    out[total] = '\0';
    return total;
}

static void test_stream(void) {
    server_t srv;
    char url[64];
    char data[256];

    const char * plain = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nframes";
    server_start(&srv, plain, strlen(plain), 0, url, sizeof(url));
    http_stream_t * s = http_stream_open(url, false);
    CHECK(s && strcmp(http_stream_content_type(s), "audio/mpeg") == 0, "stream opened");
    if (s) {
        CHECK(read_all(s, data, sizeof(data)) == 6 && strcmp(data, "frames") == 0, "stream data: '%s'", data);
        http_stream_close(s);
    }
    server_stop(&srv);

    const char * chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\nnot-hex\r\nxyz";
    server_start(&srv, chunked, strlen(chunked), 0, url, sizeof(url));
    s = http_stream_open(url, false);
    CHECK(s != NULL, "chunked stream opened");
    if (s) {
        CHECK(read_all(s, data, sizeof(data)) == 3 && strcmp(data, "abc") == 0, "stream ends at a bad chunk line");
        http_stream_close(s);
    }
    server_stop(&srv);

    /* A server that goes silent: closing does not wait for the read timeout. */
    const char * silent = "HTTP/1.1 200 OK\r\n\r\n";
    server_start(&srv, silent, strlen(silent), 3000, url, sizeof(url));
    s = http_stream_open(url, false);
    CHECK(s != NULL, "silent stream opened");
    if (s) {
        struct timespec ts = { 0, 200000000L };
        nanosleep(&ts, NULL); /* the pump thread is in recv() by now */
        uint64_t start = now_ms();
        http_stream_close(s);
        uint64_t took = now_ms() - start;
        CHECK(took < 1000, "close unblocks the pump thread (%llu ms)", (unsigned long long) took);
    }
    server_stop(&srv);
}

int main(void) {
    test_client();
    test_stream();
    printf("http: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
