#include "power_events.h"
#include "loop_wake.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define POWER_EVENTS_MAX_SUBSCRIBERS 8
#define POWER_EVENTS_DEFAULT_POLL_MS 60000
#define POWER_EVENTS_DEFAULT_FAST_MS 2000
#define POWER_EVENTS_DEFAULT_COALESCE_MS 100
#define UEVENT_BUF_SIZE 4096

typedef struct {
    power_event_cb cb;
    void * ctx;
} subscriber_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static subscriber_t subscribers[POWER_EVENTS_MAX_SUBSCRIBERS];
static int subscriber_count;
static unsigned pending;
static uint64_t pending_since_ms;
static uint64_t fast_until_ms;
static unsigned generation;
static bool running;
static bool stop_requested;
static pthread_t worker_thread;
static int uevent_fd = -1;
static bool uevent_is_netlink;
static int wake_pipe[2] = { -1, -1 };
static power_events_config_t config;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* --- parsing ------------------------------------------------------------- */

static void copy_field(char * dst, size_t size, const char * src) {
    snprintf(dst, size, "%.*s", (int) (size - 1), src);
}

bool power_uevent_parse(const char * buf, size_t len, power_uevent_t * out) {
    if (!buf || !out || len == 0) return false;
    memset(out, 0, sizeof(*out));
    /* Kernel messages start with "action@devpath\0"; udev's re-broadcasts
     * (group 2, "libudev\0" + binary header) never reach group 1, but a
     * stray one is rejected here too. */
    const char * end = buf + len;
    const char * head_end = memchr(buf, '\0', len);
    if (!head_end) return false;
    const char * at = memchr(buf, '@', (size_t) (head_end - buf));
    if (!at || at == buf) return false;
    size_t action_len = (size_t) (at - buf);
    if (action_len >= sizeof(out->action)) action_len = sizeof(out->action) - 1;
    memcpy(out->action, buf, action_len);
    out->action[action_len] = '\0';

    for (const char * p = head_end + 1; p < end;) {
        const char * z = memchr(p, '\0', (size_t) (end - p));
        size_t n = z ? (size_t) (z - p) : (size_t) (end - p);
        char kv[256];
        if (n >= sizeof(kv)) n = sizeof(kv) - 1;
        memcpy(kv, p, n);
        kv[n] = '\0';
        if (strncmp(kv, "ACTION=", 7) == 0) copy_field(out->action, sizeof(out->action), kv + 7);
        else if (strncmp(kv, "SUBSYSTEM=", 10) == 0) copy_field(out->subsystem, sizeof(out->subsystem), kv + 10);
        else if (strncmp(kv, "POWER_SUPPLY_NAME=", 18) == 0) copy_field(out->name, sizeof(out->name), kv + 18);
        else if (strncmp(kv, "USB_STATE=", 10) == 0) copy_field(out->usb_state, sizeof(out->usb_state), kv + 10);
        if (!z) break;
        p = z + 1;
    }
    return out->subsystem[0] != '\0';
}

bool power_uevent_relevant(const power_uevent_t * ev) {
    return ev && (strcmp(ev->subsystem, "power_supply") == 0 || strcmp(ev->subsystem, "android_usb") == 0);
}

/* --- notification -------------------------------------------------------- */

static void wake_worker(void) {
    if (wake_pipe[1] >= 0) {
        char c = 1;
        if (write(wake_pipe[1], &c, 1) < 0) { /* pipe full: a wake-up is already queued */ }
    }
}

static void add_pending_locked(unsigned reasons) {
    if (!pending) pending_since_ms = now_ms();
    pending |= reasons;
}

void power_events_notify(unsigned reasons) {
    pthread_mutex_lock(&lock);
    bool active = running;
    if (active) add_pending_locked(reasons);
    pthread_mutex_unlock(&lock);
    if (active) wake_worker();
}

void power_events_fast_window(int window_ms) {
    pthread_mutex_lock(&lock);
    uint64_t until = now_ms() + (uint64_t) (window_ms > 0 ? window_ms : 0);
    if (until > fast_until_ms) fast_until_ms = until;
    pthread_mutex_unlock(&lock);
    wake_worker();
}

bool power_events_subscribe(power_event_cb cb, void * ctx) {
    if (!cb) return false;
    pthread_mutex_lock(&lock);
    bool ok = subscriber_count < POWER_EVENTS_MAX_SUBSCRIBERS;
    if (ok) {
        subscribers[subscriber_count++] = (subscriber_t){ cb, ctx };
        if (running) add_pending_locked(POWER_EVT_START);
    }
    pthread_mutex_unlock(&lock);
    if (ok) wake_worker();
    return ok;
}

unsigned power_events_generation(void) {
    pthread_mutex_lock(&lock);
    unsigned g = generation;
    pthread_mutex_unlock(&lock);
    return g;
}

bool power_events_running(void) {
    pthread_mutex_lock(&lock);
    bool r = running;
    pthread_mutex_unlock(&lock);
    return r;
}

bool power_events_sys_message(const char * msg) {
    if (!msg) return false;
    if (strncmp(msg, "CHARGE:", 7) != 0 && strcmp(msg, "USB:INSERT") != 0 && strcmp(msg, "USB:REMOVE") != 0)
        return false;
    power_events_notify(POWER_EVT_SYS_SERVER);
    return true;
}

/* --- worker -------------------------------------------------------------- */

/* Drains every queued datagram; true if one of them matters. */
static bool read_uevents(void) {
    bool relevant = false;
    char buf[UEVENT_BUF_SIZE];
    for (;;) {
        struct sockaddr_nl from;
        memset(&from, 0, sizeof(from));
        struct iovec iov = { buf, sizeof(buf) - 1 };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (uevent_is_netlink) {
            msg.msg_name = &from;
            msg.msg_namelen = sizeof(from);
        }
        ssize_t n = recvmsg(uevent_fd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* The socket buffer overflowed: something was lost, look again. */
            if (errno == ENOBUFS) {
                relevant = true;
                continue;
            }
            break; /* EAGAIN: drained */
        }
        if (n == 0) break;
        /* Only the kernel (port id 0) sends on group 1. */
        if (uevent_is_netlink && from.nl_pid != 0) continue;
        buf[n] = '\0';
        power_uevent_t ev;
        if (power_uevent_parse(buf, (size_t) n, &ev) && power_uevent_relevant(&ev)) relevant = true;
    }
    return relevant;
}

static void drain_wake_pipe(void) {
    char tmp[64];
    while (read(wake_pipe[0], tmp, sizeof(tmp)) > 0) {}
}

static void dispatch(unsigned reasons) {
    subscriber_t copy[POWER_EVENTS_MAX_SUBSCRIBERS];
    int n;
    pthread_mutex_lock(&lock);
    n = subscriber_count;
    memcpy(copy, subscribers, sizeof(copy));
    generation++;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) copy[i].cb(reasons, copy[i].ctx);
    /* A cable or charger change: the GUI's idle cadence (gui.c) depends on
     * external power, and USB prompts and mode switches start there. */
    if (reasons & (POWER_EVT_UEVENT | POWER_EVT_SYS_SERVER)) loop_wake_signal();
}

static void * worker(void * arg) {
    (void) arg;
    uint64_t last_dispatch = now_ms();
    for (;;) {
        pthread_mutex_lock(&lock);
        if (stop_requested) {
            pthread_mutex_unlock(&lock);
            break;
        }
        uint64_t now = now_ms();
        int interval = now < fast_until_ms ? config.fast_interval_ms : config.poll_interval_ms;
        unsigned reasons = 0;
        int64_t wait_ms;
        if (pending) {
            uint64_t due = pending_since_ms + (uint64_t) config.coalesce_ms;
            if (now >= due) {
                reasons = pending;
                pending = 0;
            }
            wait_ms = now >= due ? 0 : (int64_t) (due - now);
        } else {
            uint64_t due = last_dispatch + (uint64_t) interval;
            if (now >= due) reasons = POWER_EVT_POLL;
            wait_ms = now >= due ? 0 : (int64_t) (due - now);
        }
        pthread_mutex_unlock(&lock);

        if (reasons) {
            dispatch(reasons);
            last_dispatch = now_ms();
            continue;
        }

        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = (struct pollfd){ .fd = wake_pipe[0], .events = POLLIN };
        if (uevent_fd >= 0) fds[nfds++] = (struct pollfd){ .fd = uevent_fd, .events = POLLIN };
        int r = poll(fds, (nfds_t) nfds, wait_ms > 60000 * 10 ? 60000 * 10 : (int) wait_ms);
        if (r < 0 && errno != EINTR) usleep(100000);
        if (r <= 0) continue;
        if (fds[0].revents) drain_wake_pipe();
        if (nfds > 1 && fds[1].revents) {
            if (read_uevents()) {
                pthread_mutex_lock(&lock);
                add_pending_locked(POWER_EVT_UEVENT);
                pthread_mutex_unlock(&lock);
            }
        }
    }
    return NULL;
}

static int open_uevent_socket(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; /* kernel uevents */
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    int rcvbuf = 256 * 1024;
    (void) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

bool power_events_start(const power_events_config_t * cfg) {
    pthread_mutex_lock(&lock);
    if (running) {
        pthread_mutex_unlock(&lock);
        return true;
    }
    pthread_mutex_unlock(&lock);

    power_events_config_t c = { .uevent_fd = -1 };
    if (cfg) c = *cfg;
    if (c.poll_interval_ms <= 0) c.poll_interval_ms = POWER_EVENTS_DEFAULT_POLL_MS;
    if (c.fast_interval_ms <= 0) c.fast_interval_ms = POWER_EVENTS_DEFAULT_FAST_MS;
    if (c.coalesce_ms <= 0) c.coalesce_ms = POWER_EVENTS_DEFAULT_COALESCE_MS;

    int ufd = c.uevent_fd;
    bool is_netlink = false;
    if (ufd < 0) {
        ufd = open_uevent_socket();
        is_netlink = ufd >= 0;
        /* Without uevents the slow poll and sys_server messages still work. */
        if (ufd < 0) fprintf(stderr, "power_events: no uevent socket (%s); polling every %d s\n",
                             strerror(errno), c.poll_interval_ms / 1000);
    }
    if (ufd >= 0) fcntl(ufd, F_SETFL, fcntl(ufd, F_GETFL) | O_NONBLOCK);
    int p[2];
    if (pipe(p) != 0) {
        if (ufd >= 0) close(ufd);
        return false;
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);

    pthread_mutex_lock(&lock);
    config = c;
    uevent_fd = ufd;
    uevent_is_netlink = is_netlink;
    wake_pipe[0] = p[0];
    wake_pipe[1] = p[1];
    stop_requested = false;
    pending = 0;
    fast_until_ms = 0;
    running = true;
    add_pending_locked(POWER_EVT_START);
    bool ok = pthread_create(&worker_thread, NULL, worker, NULL) == 0;
    if (!ok) {
        running = false;
        pending = 0;
        close(p[0]);
        close(p[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
        if (ufd >= 0) close(ufd);
        uevent_fd = -1;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

void power_events_stop(void) {
    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        return;
    }
    stop_requested = true;
    pthread_mutex_unlock(&lock);
    wake_worker();
    pthread_join(worker_thread, NULL);
    pthread_mutex_lock(&lock);
    running = false;
    pending = 0;
    subscriber_count = 0;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
    if (uevent_fd >= 0) close(uevent_fd);
    uevent_fd = -1;
    pthread_mutex_unlock(&lock);
}
