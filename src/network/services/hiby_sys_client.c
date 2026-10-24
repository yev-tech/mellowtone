/* The player side of the stock sys_server protocol (R3 Pro).
 *
 * sys_server reports every command result ("WIFI:SCAN:OK", "BT:CONNECT:FAIL",
 * "WIFI:STATUS:...") by connecting to /var/run/sys_client, sending one
 * message and waiting for a short reply. HiBy's Bluetooth ALSA plugin does
 * the same with "BT:CODEC ..." at the end of every open. The stock player
 * serves this socket (hiby_player FUN_006fd300: accept, recv, handle, write
 * a 2-byte answer, close); Mellowtone does exactly the same, so both sys_server
 * and the plugin get their answer and command results can be waited for.
 *
 * Results are kept in a small ring with sequence numbers; a request records
 * the sequence before sending and waits for a later message with one of the
 * expected prefixes.
 *
 * early_mount also reports power changes here (CHARGE:USB_INSTD, USB_RMVD,
 * AC_INSTD, AC_RMVD): they go to power_events as well. */
#include "hiby_sys_client.h"
#include "hiby_sys_server.h"
#include "debug_log.h"
#include "power_events.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SYS_CLIENT_PATH "/var/run/sys_client"
static const char * sys_client_path = SYS_CLIENT_PATH; /* tests: hiby_sys_client_test_set_path() */

void hiby_sys_client_test_set_path(const char * path) { sys_client_path = path ? path : SYS_CLIENT_PATH; }
#define RING_SIZE 32
#define MSG_MAX 512

static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond = PTHREAD_COND_INITIALIZER;
static char ring[RING_SIZE][MSG_MAX];
static unsigned long ring_seq[RING_SIZE];
static unsigned long next_seq = 1;
static int listen_fd = -1;

static void push_message(const char * msg) {
    pthread_mutex_lock(&ring_mutex);
    int slot = (int) (next_seq % RING_SIZE);
    snprintf(ring[slot], MSG_MAX, "%s", msg);
    ring_seq[slot] = next_seq++;
    pthread_cond_broadcast(&ring_cond);
    pthread_mutex_unlock(&ring_mutex);
}

static void * listen_thread(void * arg) {
    (void) arg;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "sys_client: accept failed: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        /* A sender that connects and never writes must not stall every
         * later result (and the plugin waiting behind it). */
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char buf[MSG_MAX];
        ssize_t got = recv(fd, buf, sizeof(buf) - 1, 0);
        if (got > 0) {
            buf[got] = '\0';
            buf[strcspn(buf, "\r\n")] = '\0';
            DBG_LOG("sys_client: \"%s\"\n", buf);
            /* Answer first: sys_server and the ALSA plugin block until then. */
            if (write(fd, "OK", 2) < 0) { /* sender gave up; nothing to do */ }
            push_message(buf);
            /* CHARGE:USB_INSTD / USB_RMVD / AC_INSTD / AC_RMVD (early_mount
             * forwards its power_supply uevents here): wake the battery and
             * charger code. */
            power_events_sys_message(buf);
        }
        close(fd);
    }
    return NULL;
}

bool hiby_sys_client_start(void) {
    if (listen_fd >= 0) return true;
    unlink(sys_client_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sys_client_path);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 10) != 0) {
        fprintf(stderr, "sys_client: cannot serve %s: %s\n", sys_client_path, strerror(errno));
        close(fd);
        return false;
    }
    listen_fd = fd;
    pthread_t t;
    if (pthread_create(&t, NULL, listen_thread, NULL) != 0) {
        close(fd);
        listen_fd = -1;
        return false;
    }
    pthread_detach(t);
    return true;
}

static bool has_prefix(const char * msg, const char * const * prefixes) {
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(msg, prefixes[i], strlen(prefixes[i])) == 0) return true;
    return false;
}

bool hiby_sys_request(const char * cmd, const char * const * result_prefixes, int timeout_ms,
                      char * out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    if (listen_fd < 0) return false;

    pthread_mutex_lock(&ring_mutex);
    unsigned long start = next_seq;
    pthread_mutex_unlock(&ring_mutex);

    hiby_sys_server_command(cmd);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    bool found = false;
    pthread_mutex_lock(&ring_mutex);
    for (;;) {
        for (unsigned long s = start; s < next_seq && !found; s++) {
            int slot = (int) (s % RING_SIZE);
            if (ring_seq[slot] == s && has_prefix(ring[slot], result_prefixes)) {
                if (out && out_size) snprintf(out, out_size, "%s", ring[slot]);
                found = true;
            }
        }
        if (found) break;
        if (next_seq - start > RING_SIZE) start = next_seq - RING_SIZE;
        if (pthread_cond_timedwait(&ring_cond, &ring_mutex, &deadline) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&ring_mutex);
    if (!found) fprintf(stderr, "sys_client: no result for \"%.40s\" within %d ms\n", cmd, timeout_ms);
    return found;
}
