#define _GNU_SOURCE
#include "hooks_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void blh_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

long blh_uptime_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long) ts.tv_sec;
}

int blh_run(char * const argv[], int timeout_ms) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    for (int waited = 0;; waited += 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (waited >= timeout_ms) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        blh_sleep_ms(50);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool blh_write(const char * path, const char * text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t) n;
    if (close(fd) != 0) ok = false;
    return ok;
}

const char * blh_read(const char * path, char * buf, size_t size) {
    if (size == 0) return buf;
    buf[0] = '\0';
    FILE * f = fopen(path, "re");
    if (!f) return buf;
    if (!fgets(buf, (int) size, f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}
