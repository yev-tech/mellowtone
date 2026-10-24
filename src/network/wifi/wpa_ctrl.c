#include "wpa_ctrl.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct wpa_ctrl {
    int fd;
    struct sockaddr_un local;
};

static unsigned next_client_id(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static unsigned counter;
    pthread_mutex_lock(&mu);
    unsigned id = ++counter;
    pthread_mutex_unlock(&mu);
    return id;
}

wpa_ctrl_t * wpa_ctrl_open(const char * ctrl_path, const char * client_dir) {
    if (!ctrl_path || !ctrl_path[0]) return NULL;
    struct sockaddr_un dest = { .sun_family = AF_UNIX };
    if (strlen(ctrl_path) >= sizeof(dest.sun_path)) return NULL;
    memcpy(dest.sun_path, ctrl_path, strlen(ctrl_path) + 1);

    wpa_ctrl_t * ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl) return NULL;
    ctrl->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctrl->fd < 0) {
        free(ctrl);
        return NULL;
    }
    ctrl->local.sun_family = AF_UNIX;
    int n = snprintf(ctrl->local.sun_path, sizeof(ctrl->local.sun_path), "%s/mt_wpa_%d-%u",
                     client_dir && client_dir[0] ? client_dir : "/tmp", (int) getpid(), next_client_id());
    if (n < 0 || (size_t) n >= sizeof(ctrl->local.sun_path)) goto fail;
    unlink(ctrl->local.sun_path); /* left over by an earlier process with this pid */
    if (bind(ctrl->fd, (struct sockaddr *) &ctrl->local, sizeof(ctrl->local)) != 0) goto fail;
    /* ENOENT (no supplicant) and ECONNREFUSED (stale socket file) end here. */
    if (connect(ctrl->fd, (struct sockaddr *) &dest, sizeof(dest)) != 0) {
        unlink(ctrl->local.sun_path);
        goto fail;
    }
    return ctrl;

fail:
    close(ctrl->fd);
    free(ctrl);
    return NULL;
}

void wpa_ctrl_close(wpa_ctrl_t * ctrl) {
    if (!ctrl) return;
    close(ctrl->fd);
    unlink(ctrl->local.sun_path);
    free(ctrl);
}

int wpa_ctrl_fd(const wpa_ctrl_t * ctrl) {
    return ctrl ? ctrl->fd : -1;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One datagram, waiting up to timeout_ms (-1 forever). >= 0 length,
 * WPA_CTRL_TIMEOUT or WPA_CTRL_ERR. */
static int recv_one(wpa_ctrl_t * ctrl, char * buf, size_t size, int timeout_ms) {
    for (;;) {
        struct pollfd pfd = { .fd = ctrl->fd, .events = POLLIN };
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return WPA_CTRL_ERR;
        if (r == 0) return WPA_CTRL_TIMEOUT;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return WPA_CTRL_ERR;
        ssize_t n = recv(ctrl->fd, buf, size - 1, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) return WPA_CTRL_ERR;
        buf[n] = '\0';
        return (int) n;
    }
}

int wpa_ctrl_request(wpa_ctrl_t * ctrl, const char * cmd, char * reply, size_t reply_size, int timeout_ms) {
    if (!ctrl || !cmd || !reply || reply_size < 2) return WPA_CTRL_ERR;
    /* Drop anything queued: the late reply of an earlier request that timed
     * out would otherwise be read as this one's. */
    char junk[256];
    while (recv(ctrl->fd, junk, sizeof(junk), MSG_DONTWAIT) > 0) {}

    size_t len = strlen(cmd);
    for (;;) {
        ssize_t n = send(ctrl->fd, cmd, len, 0);
        if (n == (ssize_t) len) break;
        if (n < 0 && errno == EINTR) continue;
        return WPA_CTRL_ERR;
    }
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        long long left = deadline - now_ms();
        if (left < 0) left = 0;
        int n = recv_one(ctrl, reply, reply_size, (int) left);
        if (n < 0) return n;
        /* "<N>..." is an event for an attached handle, not our reply. */
        if (n > 0 && reply[0] == '<') continue;
        return n;
    }
}

bool wpa_ctrl_attach(wpa_ctrl_t * ctrl, int timeout_ms) {
    char reply[16];
    int n = wpa_ctrl_request(ctrl, "ATTACH", reply, sizeof(reply), timeout_ms);
    return n >= 2 && strncmp(reply, "OK", 2) == 0;
}

int wpa_ctrl_recv_event(wpa_ctrl_t * ctrl, char * buf, size_t size, int timeout_ms) {
    if (!ctrl || !buf || size < 2) return WPA_CTRL_ERR;
    int n = recv_one(ctrl, buf, size, timeout_ms);
    if (n == WPA_CTRL_TIMEOUT) return 0;
    return n;
}
