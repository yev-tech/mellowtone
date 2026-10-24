#define _GNU_SOURCE
#include "state_reset.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* storage_paths.h's name, spelled out: this file builds without a board. */
#define STATE_DIR ".mellowtone"

static const char * const STATE_SUBDIRS[] = {
    "config", "plugins", "plugin-data", "library", "cache", "logs", "backup", "update",
};

static void fsync_dir(const char * dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void) fsync(fd);
    close(fd);
}

static bool mkdir_one(const char * dir) {
    struct stat st;
    return (mkdir(dir, 0755) == 0 || errno == EEXIST) && stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

bool mt_state_create_layout_at(const char * sd_root) {
    char state[PATH_MAX];
    if (snprintf(state, sizeof(state), "%s/" STATE_DIR, sd_root) >= (int) sizeof(state) || !mkdir_one(state))
        return false;
    bool ok = true;
    for (size_t i = 0; i < sizeof(STATE_SUBDIRS) / sizeof(STATE_SUBDIRS[0]); i++) {
        char dir[PATH_MAX];
        if (snprintf(dir, sizeof(dir), "%s/%s", state, STATE_SUBDIRS[i]) >= (int) sizeof(dir) || !mkdir_one(dir))
            ok = false;
    }
    fsync_dir(state);
    return ok;
}

bool mt_state_reset_config_at(const char * sd_root, char * backup_out, size_t backup_size) {
    char state[PATH_MAX], config[PATH_MAX], backup[PATH_MAX];
    if (backup_out && backup_size) backup_out[0] = '\0';
    if (snprintf(state, sizeof(state), "%s/" STATE_DIR, sd_root) >= (int) sizeof(state) ||
        snprintf(config, sizeof(config), "%s/config", state) >= (int) sizeof(config) ||
        snprintf(backup, sizeof(backup), "%s/backup", state) >= (int) sizeof(backup))
        return false;
    struct stat st;
    if (stat(config, &st) != 0) return errno == ENOENT;
    if (!mkdir_one(backup)) return false;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);
    char dest[PATH_MAX];
    for (int attempt = 0; attempt < 100; attempt++) {
        int n = attempt == 0 ? snprintf(dest, sizeof(dest), "%s/reset-%s", backup, stamp)
                             : snprintf(dest, sizeof(dest), "%s/reset-%s-%d", backup, stamp, attempt);
        if (n < 0 || n >= (int) sizeof(dest)) return false;
        if (lstat(dest, &st) == 0) continue;
        if (rename(config, dest) != 0) return false;
        fsync_dir(backup);
        fsync_dir(state);
        if (backup_out && backup_size) snprintf(backup_out, backup_size, "%s", dest);
        return true;
    }
    return false;
}
