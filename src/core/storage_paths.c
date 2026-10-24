#define _GNU_SOURCE
#include "storage_paths.h"
#include "state_reset.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How old the cached card state may be for a write check. The UI poll
 * refreshes it every few seconds anyway; this bounds /proc/mounts reads by
 * busy writers (album art, logs) to about one a second. */
#define STATE_CHECK_MAX_AGE_MS 1000

static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned pending_mask;
static void (*flush_hooks[2])(void);

/* Whether config/ belongs to this session: settings were loaded from this
 * card (or it had none when it appeared). Cleared while the card is away;
 * set again when the same card (bound_card_id) comes back, or a card
 * without settings appears. A card with its own settings that is not the
 * bound one sets config_conflict instead: its settings are kept and config
 * writes pause until a restart. */
static atomic_bool config_bound;
static atomic_bool config_conflict;
/* The bound card's identity: MT_CARD_ID_FILE, made once per card. Names,
 * inode numbers and the device do not tell two cards in the same slot
 * apart. UI thread only (init, poll, lend). */
static char bound_card_id[33];
static bool bound_card_known;
static sd_card_state_t polled_state = SD_CARD_ABSENT;
static bool polled_once;
/* The card is lent to a computer (mt_state_set_card_lent()). */
static atomic_bool card_lent;

static bool under_state_root(const char * path) {
    size_t n = strlen(MT_STATE_ROOT);
    return path && strncmp(path, MT_STATE_ROOT, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static bool under_config(const char * path) {
    size_t n = strlen(MT_CONFIG_DIR);
    return strncmp(path, MT_CONFIG_DIR, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

sd_card_state_t mt_state_card(void) {
    if (atomic_load(&card_lent)) return SD_CARD_ABSENT;
    return sd_card_state_fresh(STATE_CHECK_MAX_AGE_MS);
}

bool mt_state_available(void) {
    return sd_card_state_writable(mt_state_card());
}

bool mt_config_writable(void) {
    return mt_state_available() && atomic_load(&config_bound) && !atomic_load(&config_conflict);
}

static bool mkdir_p(const char * dir) {
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", dir) >= (int) sizeof(buf)) return false;
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';
    /* Only the components below the card root are created: the mount point
     * itself must already exist (it is the card). */
    size_t start = strlen(MT_SD_ROOT);
    for (size_t i = start + 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

bool mt_state_mkdirs(const char * dir) {
    if (!under_state_root(dir)) return false;
    if (under_config(dir) ? !mt_config_writable() : !mt_state_available()) return false;
    return mkdir_p(dir);
}

bool mt_state_prepare_write(const char * path) {
    if (!under_state_root(path)) return false;
    if (under_config(path) ? !mt_config_writable() : !mt_state_available()) return false;
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int) sizeof(dir)) return false;
    char * slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';
    return mkdir_p(dir);
}

bool mt_state_write_file(const char * path, const void * data, size_t len) {
    if (!mt_state_prepare_write(path)) return false;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const char * p = data;
    size_t left = len;
    bool ok = true;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        p += n;
        left -= (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void) fsync(dfd); close(dfd); }
    }
    return true;
}

int mt_log_open(const char * name, int flags) {
    if (!name || strchr(name, '/') || !mt_state_mkdirs(MT_LOGS_DIR)) return -1;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", MT_LOGS_DIR, name) >= (int) sizeof(path)) return -1;
    return open(path, flags | O_CLOEXEC, 0644);
}

int mt_state_open_library_at(int sd_rootfd, bool create) {
    if (sd_rootfd < 0) return -1;
    if (create && mt_state_available()) {
        (void) mkdirat(sd_rootfd, MT_STATE_DIR, 0755);
        (void) mkdirat(sd_rootfd, MT_LIBRARY_REL, 0755);
    }
    return openat(sd_rootfd, MT_LIBRARY_REL, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

void mt_state_mark_unsaved(mt_pending_t what) {
    pthread_mutex_lock(&pending_mutex);
    pending_mask |= (unsigned) what;
    pthread_mutex_unlock(&pending_mutex);
}

void mt_state_set_flush_hook(mt_pending_t what, void (*hook)(void)) {
    if (what == MT_PENDING_SETTINGS) flush_hooks[0] = hook;
    else if (what == MT_PENDING_PEQ) flush_hooks[1] = hook;
}

static void flush_pending(void) {
    pthread_mutex_lock(&pending_mutex);
    unsigned mask = pending_mask;
    pending_mask = 0;
    pthread_mutex_unlock(&pending_mutex);
    if ((mask & MT_PENDING_SETTINGS) && flush_hooks[0]) flush_hooks[0]();
    if ((mask & MT_PENDING_PEQ) && flush_hooks[1]) flush_hooks[1]();
}

/* Reads the card's identity into out (32 hex digits). With create, a card
 * without one (and writable) gets a new random one. */
static bool card_id_get(char out[33], bool create) {
    int fd = open(MT_CARD_ID_FILE, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[33];
        ssize_t n = read(fd, buf, 32);
        close(fd);
        if (n != 32) return false;
        for (int i = 0; i < 32; i++) {
            char c = buf[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        memcpy(out, buf, 32);
        out[32] = '\0';
        return true;
    }
    if (errno != ENOENT || !create || !mt_state_available()) return false;

    unsigned char raw[16];
    bool random_ok = false;
    int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (rfd >= 0) {
        random_ok = read(rfd, raw, sizeof(raw)) == (ssize_t) sizeof(raw);
        close(rfd);
    }
    if (!random_ok) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t a = (uint64_t) ts.tv_sec * 1000000007u ^ (uint64_t) ts.tv_nsec;
        uint64_t b = (uint64_t) getpid() * 2654435761u ^ (uint64_t) (uintptr_t) &ts;
        memcpy(raw, &a, 8);
        memcpy(raw + 8, &b, 8);
    }
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", raw[i]);
    if (!mt_state_write_file(MT_CARD_ID_FILE, hex, 32)) return false;
    memcpy(out, hex, 33);
    return true;
}

/* The mounted card is known not to be the bound one. An unreadable
 * identity is not proof: only a different or missing one is. */
static bool card_is_other(void) {
    if (!bound_card_known) return false;
    char id[33];
    if (card_id_get(id, false)) return strcmp(id, bound_card_id) != 0;
    struct stat st;
    return stat(MT_CARD_ID_FILE, &st) != 0 && errno == ENOENT;
}

/* A writable card is mounted and config is not bound: bind it when it is
 * the bound card or has no settings of its own; otherwise keep its
 * settings and report the conflict. */
static mt_notice_t bind_config(mt_notice_t notice) {
    char id[33];
    bool have_id = card_id_get(id, false);
    if (bound_card_known && have_id && strcmp(id, bound_card_id) == 0) {
        atomic_store(&config_bound, true);
        return notice;
    }
    struct stat st;
    if (stat(SETTINGS_FILE_PATH, &st) == 0) {
        /* The card has its own settings and is not the one this session's
         * settings belong to: keep the card's, don't overwrite them. */
        atomic_store(&config_conflict, true);
        return MT_NOTICE_RESTART_FOR_SETTINGS;
    }
    bound_card_known = card_id_get(bound_card_id, true);
    atomic_store(&config_bound, true);
    return notice;
}

bool mt_state_create_layout(void) {
    return mt_state_available() && mt_state_create_layout_at(MT_SD_ROOT);
}

void mt_state_init(void) {
    sd_card_state_t state = sd_card_refresh();
    /* With any readable card at boot, this session's settings are the
     * card's (loaded from it, or defaults when it has none). */
    bool readable = sd_card_state_readable(state);
    atomic_store(&config_bound, readable);
    atomic_store(&config_conflict, false);
    /* Read only: a card without an identity gets one at the first poll. */
    bound_card_known = readable && card_id_get(bound_card_id, false);
}

static mt_notice_t notice_for(sd_card_state_t state) {
    switch (state) {
    case SD_CARD_ABSENT: return MT_NOTICE_NO_CARD;
    case SD_CARD_NTFS_READONLY: return MT_NOTICE_NTFS_READONLY;
    case SD_CARD_READONLY_ERRORS: return MT_NOTICE_READONLY_ERRORS;
    case SD_CARD_UNSUPPORTED: return MT_NOTICE_UNSUPPORTED;
    default: return MT_NOTICE_NONE;
    }
}

mt_notice_t mt_state_poll(void) {
    /* Lent: the card is away on purpose; the state is re-read when it comes back. */
    if (atomic_load(&card_lent)) return MT_NOTICE_NONE;
    sd_card_state_t state = sd_card_refresh();
    bool first = !polled_once;
    sd_card_state_t before = polled_state;
    polled_once = true;
    polled_state = state;
    /* Away: config belongs to no card until one is checked. */
    if (state == SD_CARD_ABSENT) atomic_store(&config_bound, false);
    /* A card swapped between two polls looks unchanged. */
    bool swapped = !first && sd_card_state_writable(state) && atomic_load(&config_bound) && card_is_other();
    if (swapped) atomic_store(&config_bound, false);
    if (!first && state == before && !swapped) {
        /* The bound card got an identity only now (read-only at boot). */
        if (sd_card_state_writable(state) && atomic_load(&config_bound) && !bound_card_known)
            bound_card_known = card_id_get(bound_card_id, true);
        return MT_NOTICE_NONE;
    }

    mt_notice_t notice = notice_for(state);
    if (sd_card_state_writable(state) && (first || swapped || !sd_card_state_writable(before))) {
        mt_state_create_layout();
        if (!atomic_load(&config_bound) && !atomic_load(&config_conflict)) notice = bind_config(notice);
        if (atomic_load(&config_bound) && !bound_card_known) bound_card_known = card_id_get(bound_card_id, true);
        if (atomic_load(&config_bound) && !atomic_load(&config_conflict)) flush_pending();
    }
    /* The first poll after boot only speaks up when something is wrong. */
    return notice;
}

/* stdout/stderr while lent: where they pointed (a log on the card) and the
 * RAM file in their place. */
static char lent_log_paths[2][PATH_MAX];

static bool fd_target_on_card(int fd, char * out, size_t out_size) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, out_size - 1);
    if (n <= 0) return false;
    out[n] = '\0';
    /* The kernel reports the resolved path (the simulator's card is a
     * relative directory, its stdout usually the terminal). */
    char root[PATH_MAX];
    if (!realpath(MT_SD_ROOT, root)) snprintf(root, sizeof(root), "%s", MT_SD_ROOT);
    size_t len = strlen(root);
    return strncmp(out, root, len) == 0 && out[len] == '/';
}

static void stdio_to_ram(void) {
    fflush(stdout);
    fflush(stderr);
    int ram = -1;
    for (int fd = 1; fd <= 2; fd++) {
        lent_log_paths[fd - 1][0] = '\0';
        if (!fd_target_on_card(fd, lent_log_paths[fd - 1], sizeof(lent_log_paths[0]))) {
            lent_log_paths[fd - 1][0] = '\0';
            continue;
        }
        if (ram < 0) ram = open(MT_LENT_LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        if (ram < 0 || dup2(ram, fd) < 0) lent_log_paths[fd - 1][0] = '\0';
    }
    if (ram >= 0) close(ram);
}

/* Appends the RAM log to the card log and points stdout/stderr back at it.
 * Without the card (it did not come back) they stay in RAM. */
static void stdio_back_to_card(void) {
    fflush(stdout);
    fflush(stderr);
    bool copied = false;
    for (int fd = 1; fd <= 2; fd++) {
        const char * path = lent_log_paths[fd - 1];
        if (!path[0]) continue;
        int card = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (card < 0) continue;
        if (!copied) {
            int ram = open(MT_LENT_LOG_FILE, O_RDONLY | O_CLOEXEC);
            char buf[4096];
            ssize_t n;
            while (ram >= 0 && (n = read(ram, buf, sizeof(buf))) > 0)
                if (write(card, buf, (size_t) n) != n) break;
            if (ram >= 0) close(ram);
            copied = true;
        }
        if (dup2(card, fd) >= 0) lent_log_paths[fd - 1][0] = '\0';
        close(card);
    }
    if (!lent_log_paths[0][0] && !lent_log_paths[1][0]) unlink(MT_LENT_LOG_FILE);
}

void mt_state_set_card_lent(bool lent) {
    if (atomic_load(&card_lent) == lent) return;
    if (lent) {
        stdio_to_ram();
        atomic_store(&card_lent, true);
        return;
    }
    atomic_store(&card_lent, false);
    sd_card_state_t state = sd_card_refresh();
    stdio_back_to_card();
    polled_once = true;
    if (sd_card_state_writable(state) && atomic_load(&config_bound) && card_is_other()) {
        /* Another card came back: the next poll checks it like any card
         * that appears. */
        atomic_store(&config_bound, false);
        polled_state = SD_CARD_ABSENT;
        return;
    }
    polled_state = state;
    /* What was saved while lent was kept in memory: write it now. */
    if (sd_card_state_writable(state) && atomic_load(&config_bound) && !atomic_load(&config_conflict))
        flush_pending();
}

bool mt_state_card_lent(void) {
    return atomic_load(&card_lent);
}

bool mt_state_reset_config(void) {
    struct stat st;
    if (stat(MT_CONFIG_DIR, &st) != 0) return errno == ENOENT;
    if (!mt_state_available()) return false;
    return mt_state_reset_config_at(MT_SD_ROOT, NULL, 0);
}
