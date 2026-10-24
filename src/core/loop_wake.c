#include "loop_wake.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* A self-pipe: the reading end is polled by the main loop, any thread may
 * write one byte. wake_pending collapses a burst of signals into a single
 * byte, so the pipe can never fill up. */
static int wake_pipe[2] = { -1, -1 };
static pthread_once_t wake_once = PTHREAD_ONCE_INIT;
static atomic_bool wake_pending = false;

static void wake_pipe_init(void) {
    int fds[2];
    if (pipe(fds) != 0) return;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    wake_pipe[0] = fds[0];
    wake_pipe[1] = fds[1];
}

void loop_wake_signal(void) {
    pthread_once(&wake_once, wake_pipe_init);
    if (wake_pipe[1] < 0) return;
    if (atomic_exchange(&wake_pending, true)) return;
    char byte = 1;
    ssize_t n;
    do {
        n = write(wake_pipe[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
}

bool loop_wake_wait(uint32_t timeout_ms) {
    pthread_once(&wake_once, wake_pipe_init);
    if (wake_pipe[0] < 0) {
        usleep(timeout_ms * 1000);
        return false;
    }
    struct pollfd pfd = { .fd = wake_pipe[0], .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, timeout_ms > (uint32_t) INT32_MAX ? -1 : (int) timeout_ms);
    if (ret <= 0 || !(pfd.revents & POLLIN)) return false;
    /* Clear the flag before draining: a signal racing with the drain then
     * writes a fresh byte and the next wait returns at once. */
    atomic_store(&wake_pending, false);
    char buf[16];
    while (read(wake_pipe[0], buf, sizeof(buf)) > 0) {
    }
    return true;
}
