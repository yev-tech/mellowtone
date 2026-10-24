#define _GNU_SOURCE
#include "lx.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char log_path[PATH_MAX];
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void lx_log_set_file(const char * path) {
    pthread_mutex_lock(&log_mutex);
    snprintf(log_path, sizeof(log_path), "%s", path ? path : "");
    pthread_mutex_unlock(&log_mutex);
}

void lx_vlog(const char * fmt, va_list ap) {
    char line[600];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(line, sizeof(line), "%ld.%01lds ", (long) ts.tv_sec, (long) (ts.tv_nsec / 100000000L));
    if (n < 0) n = 0;
    vsnprintf(line + n, sizeof(line) - (size_t) n, fmt, ap);
    pthread_mutex_lock(&log_mutex);
    fprintf(stderr, "mellowtone-launcher: %s\n", line + n);
    if (log_path[0]) {
        FILE * f = fopen(log_path, "ae");
        if (f) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&log_mutex);
}

void lx_log(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lx_vlog(fmt, ap);
    va_end(ap);
}

bool lx_join(char * out, size_t size, const char * a, const char * b) {
    int n = snprintf(out, size, "%s/%s", a, b);
    return n > 0 && (size_t) n < size;
}

bool lx_exists(const char * path) {
    struct stat st;
    return path && lstat(path, &st) == 0;
}

bool lx_is_dir(const char * path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool lx_is_executable(const char * path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

bool lx_mkdir_p(const char * dir) {
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", dir) >= (int) sizeof(buf)) return false;
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return (mkdir(buf, 0755) == 0 || errno == EEXIST) && lx_is_dir(buf);
}

bool lx_rm_rf(const char * path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR * d = opendir(path);
    if (!d) return false;
    bool ok = true;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        if (!lx_join(child, sizeof(child), path, e->d_name) || !lx_rm_rf(child)) ok = false;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

void lx_fsync_dir(const char * dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void) fsync(fd);
    close(fd);
}

static void fsync_parent(const char * path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    lx_fsync_dir(dir);
}

bool lx_write_file_sync(const char * path, const char * text) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t) n;
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    fsync_parent(path);
    return true;
}

bool lx_read_file(const char * path, char * buf, size_t size) {
    if (size == 0) return false;
    buf[0] = '\0';
    FILE * f = fopen(path, "re");
    if (!f) return false;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

void lx_update_log(const lx_config_t * cfg, const char * fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    lx_log("update: %s", msg);
    if (!cfg->log_dir) return;
    char path[PATH_MAX];
    if (!lx_join(path, sizeof(path), cfg->log_dir, "update.log")) return;
    FILE * f = fopen(path, "ae");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s %s\n", stamp, msg);
    fflush(f);
    (void) fsync(fileno(f));
    fclose(f);
}

int lx_version_cmp(const char * a, const char * b) {
    while (*a || *b) {
        if (isdigit((unsigned char) *a) && isdigit((unsigned char) *b)) {
            unsigned long x = strtoul(a, (char **) &a, 10);
            unsigned long y = strtoul(b, (char **) &b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        if (*a == *b) {
            a++;
            b++;
            continue;
        }
        /* A shorter version is older ("1.2" < "1.2.1"); otherwise byte order. */
        if (!*a) return -1;
        if (!*b) return 1;
        return (unsigned char) *a < (unsigned char) *b ? -1 : 1;
    }
    return 0;
}
