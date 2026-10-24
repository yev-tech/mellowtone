/* The installed version, its backup on the card, and the journaled
 * install / roll back (lx_update.h). */
#define _GNU_SOURCE
#include "lx_update.h"

#include "mbedtls/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

lx_step_hook_t lx_step_hook;

#define JOURNAL "swap.journal"
#define PHASE_COMMITTED "committed"

static bool step(const char * name) {
    return !lx_step_hook || lx_step_hook(name);
}

typedef struct {
    /* flash */
    char root[PATH_MAX], current[PATH_MAX], dropped[PATH_MAX], next[PATH_MAX], partial[PATH_MAX], journal[PATH_MAX];
    char legacy_previous[PATH_MAX], legacy_old[PATH_MAX];
    /* card; card[0] == '\0' without a card root */
    char card[PATH_MAX], backup[PATH_MAX], backup_partial[PATH_MAX], backup_new[PATH_MAX], backup_old[PATH_MAX];
} slot_paths_t;

static bool paths(const lx_config_t * cfg, slot_paths_t * p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->root, sizeof(p->root), "%s", cfg->slots_root);
    if (!lx_join(p->current, sizeof(p->current), p->root, "current") ||
        !lx_join(p->dropped, sizeof(p->dropped), p->root, "current.old") ||
        !lx_join(p->next, sizeof(p->next), p->root, "next") ||
        !lx_join(p->partial, sizeof(p->partial), p->root, "next.partial") ||
        !lx_join(p->journal, sizeof(p->journal), p->root, JOURNAL) ||
        !lx_join(p->legacy_previous, sizeof(p->legacy_previous), p->root, "previous") ||
        !lx_join(p->legacy_old, sizeof(p->legacy_old), p->root, "previous.old"))
        return false;
    if (cfg->sd_root && cfg->sd_root[0]) {
        int n = snprintf(p->card, sizeof(p->card), "%s/.mellowtone/backup", cfg->sd_root);
        if (n <= 0 || (size_t) n >= sizeof(p->card) || !lx_join(p->backup, sizeof(p->backup), p->card, "player") ||
            !lx_join(p->backup_partial, sizeof(p->backup_partial), p->card, "player.partial") ||
            !lx_join(p->backup_new, sizeof(p->backup_new), p->card, "player.new") ||
            !lx_join(p->backup_old, sizeof(p->backup_old), p->card, "player.old"))
            return false;
    }
    return true;
}

static bool rename_synced(const char * dir, const char * from, const char * to) {
    if (rename(from, to) != 0) {
        lx_log("slots: rename %s -> %s failed: %s", from, to, strerror(errno));
        return false;
    }
    lx_fsync_dir(dir);
    return true;
}

static bool has_player(const char * dir) {
    char exe[PATH_MAX];
    return lx_join(exe, sizeof(exe), dir, LX_PLAYER_EXE) && lx_exists(exe);
}

/* Removes current in one step (a rename), then its files: a current/ that
 * exists is always complete, whatever a power cut interrupted. */
static bool drop_current(const slot_paths_t * p) {
    if (lx_is_dir(p->current)) {
        if (!lx_rm_rf(p->dropped) || !rename_synced(p->root, p->current, p->dropped)) return false;
    }
    lx_rm_rf(p->dropped);
    lx_fsync_dir(p->root);
    return true;
}

/* ---- copying a version tree, checked ------------------------------------------- */

static bool sha_file(const char * path, unsigned char out[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    static unsigned char buf[64 * 1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    bool ok = true;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) ok = false;
        if (r <= 0) break;
        mbedtls_sha256_update(&sha, buf, (size_t) r);
    }
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
    close(fd);
    return ok;
}

/* Copies one file, hashing what was read; then reads the copy back and
 * compares, so a bad card or flash is caught here and not at the next start. */
static bool copy_file_checked(const char * from, const char * to, mode_t mode, char * err, size_t err_size) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        snprintf(err, err_size, "cannot read %s: %s", from, strerror(errno));
        return false;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        snprintf(err, err_size, "cannot write %s: %s", to, strerror(errno));
        close(in);
        return false;
    }
    static unsigned char buf[64 * 1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    bool ok = true;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) ok = false;
        if (r <= 0) break;
        mbedtls_sha256_update(&sha, buf, (size_t) r);
        for (ssize_t off = 0; off < r && ok;) {
            ssize_t w = write(out, buf + off, (size_t) (r - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) ok = false;
            else off += w;
        }
        if (!ok) break;
    }
    unsigned char want[32], got[32];
    mbedtls_sha256_finish(&sha, want);
    mbedtls_sha256_free(&sha);
    if (fsync(out) != 0) ok = false;
    if (close(out) != 0) ok = false;
    close(in);
    chmod(to, mode);
    if (!ok) {
        snprintf(err, err_size, "write failed for %s: %s", to, errno ? strerror(errno) : "short");
        return false;
    }
    if (!sha_file(to, got) || memcmp(want, got, sizeof(got)) != 0) {
        snprintf(err, err_size, "the copy of %s does not read back correctly", from);
        return false;
    }
    return true;
}

static bool copy_tree_checked(const char * from, const char * to, char * err, size_t err_size) {
    if (!lx_mkdir_p(to)) {
        snprintf(err, err_size, "cannot create %s", to);
        return false;
    }
    DIR * d = opendir(from);
    if (!d) {
        snprintf(err, err_size, "cannot read %s", from);
        return false;
    }
    bool ok = true;
    struct dirent * e;
    while (ok && (e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char src[PATH_MAX], dst[PATH_MAX];
        struct stat st;
        if (!lx_join(src, sizeof(src), from, e->d_name) || !lx_join(dst, sizeof(dst), to, e->d_name) ||
            lstat(src, &st) != 0) {
            snprintf(err, err_size, "cannot read %s/%s", from, e->d_name);
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            ok = copy_tree_checked(src, dst, err, err_size);
        } else if (S_ISREG(st.st_mode)) {
            ok = copy_file_checked(src, dst, (st.st_mode & 0111) ? 0755 : 0644, err, err_size);
        }
    }
    closedir(d);
    if (ok) lx_fsync_dir(to);
    return ok;
}

static long long tree_bytes(const char * dir) {
    DIR * d = opendir(dir);
    if (!d) return 0;
    long long total = 0;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char child[PATH_MAX];
        struct stat st;
        if (!lx_join(child, sizeof(child), dir, e->d_name) || lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += tree_bytes(child);
        else if (S_ISREG(st.st_mode)) total += st.st_size;
    }
    closedir(d);
    return total;
}

static long long free_bytes(const char * dir) {
    struct statvfs v;
    if (statvfs(dir, &v) != 0) return -1;
    return (long long) v.f_bavail * (long long) v.f_frsize;
}

/* ---- the card backup ------------------------------------------------------------ */

static bool card_writable(const slot_paths_t * p, const lx_config_t * cfg) {
    return p->card[0] && lx_is_dir(cfg->sd_root) && access(cfg->sd_root, W_OK) == 0;
}

/* player.new becomes player; the old backup is dropped. Each rename is
 * followed by a directory sync, and every state in between is finished by
 * calling this again. */
static bool rotate_backup(const slot_paths_t * p) {
    if (lx_is_dir(p->backup_new)) {
        if (lx_is_dir(p->backup)) {
            if (!lx_rm_rf(p->backup_old) || !rename_synced(p->card, p->backup, p->backup_old)) return false;
        }
        if (!rename_synced(p->card, p->backup_new, p->backup)) return false;
    } else if (!lx_is_dir(p->backup) && lx_is_dir(p->backup_old)) {
        /* Not reachable through our own steps; never drop the only backup. */
        return rename_synced(p->card, p->backup_old, p->backup);
    }
    lx_rm_rf(p->backup_old);
    lx_fsync_dir(p->card);
    return true;
}

/* The current version copied to the card as player.new (checked). */
static bool save_current(const lx_config_t * cfg, const slot_paths_t * p, char * err, size_t err_size) {
    if (!card_writable(p, cfg)) {
        snprintf(err, err_size, "no writable card for the backup of the current version");
        return false;
    }
    long long need = tree_bytes(p->current) + 256 * 1024;
    if (!lx_mkdir_p(p->card)) {
        snprintf(err, err_size, "cannot create %s", p->card);
        return false;
    }
    lx_rm_rf(p->backup_partial);
    lx_rm_rf(p->backup_new);
    long long room = free_bytes(p->card);
    if (room >= 0 && room < need) {
        snprintf(err, err_size, "not enough space on the card for the backup: %lld KB needed, %lld KB free",
                 need / 1024, room / 1024);
        return false;
    }
    if (!copy_tree_checked(p->current, p->backup_partial, err, err_size)) {
        if (!lx_step_hook) lx_rm_rf(p->backup_partial);
        return false;
    }
    return rename_synced(p->card, p->backup_partial, p->backup_new);
}

/* The card backup copied to flash as current (current must be gone). */
static bool restore_backup(const slot_paths_t * p, char * err, size_t err_size) {
    if (lx_is_dir(p->next)) return rename_synced(p->root, p->next, p->current);
    if (!has_player(p->backup)) {
        snprintf(err, err_size, "no backup of the previous version on the card");
        return false;
    }
    lx_rm_rf(p->partial);
    if (!copy_tree_checked(p->backup, p->partial, err, err_size)) {
        if (!lx_step_hook) lx_rm_rf(p->partial);
        return false;
    }
    char exe[PATH_MAX];
    if (lx_join(exe, sizeof(exe), p->partial, LX_PLAYER_EXE)) chmod(exe, 0755);
    if (!rename_synced(p->root, p->partial, p->next)) return false;
    if (!step("next-ready")) return false;
    return rename_synced(p->root, p->next, p->current);
}

/* A previous version left in flash by the two-slot launcher moves to the
 * card once (it is the backup unless the card already has one). */
static void migrate_legacy_previous(const lx_config_t * cfg, const slot_paths_t * p) {
    if (!lx_is_dir(p->legacy_previous)) {
        lx_rm_rf(p->legacy_old);
        return;
    }
    if (!card_writable(p, cfg)) return; /* tried again at the next start */
    if (!has_player(p->backup)) {
        char err[200];
        lx_mkdir_p(p->card);
        lx_rm_rf(p->backup_partial);
        if (!copy_tree_checked(p->legacy_previous, p->backup_partial, err, sizeof(err)) ||
            !rename_synced(p->card, p->backup_partial, p->backup)) {
            lx_log("slots: moving the previous version to the card failed: %s", err);
            lx_rm_rf(p->backup_partial);
            return;
        }
        lx_update_log(cfg, "moved the previous version to the card");
    }
    lx_rm_rf(p->legacy_previous);
    lx_rm_rf(p->legacy_old);
    lx_fsync_dir(p->root);
}

/* ---- the journal ------------------------------------------------------------------ */

static bool journal_write(const slot_paths_t * p, const char * op, bool committed) {
    char text[160];
    snprintf(text, sizeof(text), "%s\n%s", op, committed ? PHASE_COMMITTED "\n" : "");
    return lx_write_file_sync(p->journal, text);
}

static bool journal_remove(const slot_paths_t * p) {
    if (unlink(p->journal) != 0 && errno != ENOENT) return false;
    lx_fsync_dir(p->root);
    return true;
}

/* Finishes a journaled install or roll back from wherever it stopped. */
static bool finish_journal(const lx_config_t * cfg, const slot_paths_t * p) {
    char text[200] = "";
    lx_read_file(p->journal, text, sizeof(text));
    bool rollback = strncmp(text, "rollback", 8) == 0;
    bool committed = strstr(text, "\n" PHASE_COMMITTED) != NULL;
    text[strcspn(text, "\n")] = '\0';
    lx_log("slots: finishing an interrupted %s", text);
    char err[200] = "";
    bool ok = true;
    if (rollback && !committed && lx_is_dir(p->current)) {
        /* Stopped before the current version was removed (a roll back marks
         * the journal committed as soon as it is gone): nothing changed. */
        lx_rm_rf(p->backup_new);
        lx_rm_rf(p->next);
    } else {
        if (!rollback) ok = rotate_backup(p); /* an install rotates before touching flash */
        if (ok && !lx_is_dir(p->current)) ok = restore_backup(p, err, sizeof(err));
        if (ok && rollback) ok = rotate_backup(p);
        lx_rm_rf(p->next);
    }
    if (ok) ok = journal_remove(p);
    else if (!has_player(p->current) && !has_player(p->backup)) {
        /* Nothing to restore from (the card is gone or has no backup): the
         * factory player starts; the journal is of no further use. Any other
         * failure keeps it, and the next start tries again. */
        journal_remove(p);
    }
    lx_update_log(cfg, "recovered an interrupted %s: %s%s%s", text, ok ? "done" : "FAILED", err[0] ? ": " : "", err);
    return ok;
}

bool lx_slots_recover(const lx_config_t * cfg) {
    slot_paths_t p;
    if (!paths(cfg, &p)) return false;
    if (!lx_is_dir(p.root)) return true;
    bool ok = true;
    if (lx_exists(p.partial) && !lx_rm_rf(p.partial)) ok = false;
    if (lx_exists(p.dropped) && !lx_rm_rf(p.dropped)) ok = false;
    if (p.card[0]) lx_rm_rf(p.backup_partial);
    if (lx_exists(p.journal)) {
        if (!finish_journal(cfg, &p)) ok = false;
    } else {
        if (lx_exists(p.next)) {
            /* Unpacked but never committed: the package is still on the card. */
            lx_log("slots: removing an uncommitted next/");
            if (!lx_rm_rf(p.next)) ok = false;
        }
        if (p.card[0]) {
            /* A backup saved for an install or roll back that never started. */
            lx_rm_rf(p.backup_new);
            if (lx_is_dir(p.backup_old) && !lx_is_dir(p.backup)) rename_synced(p.card, p.backup_old, p.backup);
            lx_rm_rf(p.backup_old);
        }
    }
    migrate_legacy_previous(cfg, &p);
    lx_fsync_dir(p.root);
    return ok;
}

/* ---- install and roll back ----------------------------------------------------------- */

bool lx_slots_install_begin(const lx_config_t * cfg, const char * version, long long new_bytes, char * err,
                            size_t err_size) {
    slot_paths_t p;
    if (!paths(cfg, &p)) return false;
    bool have_current = lx_is_dir(p.current);
    long long room = free_bytes(p.root);
    long long need = new_bytes + 256 * 1024 - (have_current ? tree_bytes(p.current) : 0);
    if (room >= 0 && room < need) {
        snprintf(err, err_size, "not enough space: %lld KB needed, %lld KB free", need / 1024, room / 1024);
        return false;
    }
    if (have_current) {
        if (!save_current(cfg, &p, err, err_size)) return false;
        if (!step("backup-saved")) return false;
    }
    char op[96];
    snprintf(op, sizeof(op), "install %s", version ? version : "");
    if (!journal_write(&p, op, false)) {
        snprintf(err, err_size, "cannot write %s", p.journal);
        return false;
    }
    if (!step("journal")) return false;
    if (!rotate_backup(&p)) {
        snprintf(err, err_size, "cannot replace the backup on the card");
        return false;
    }
    if (!step("backup-rotated")) return false;
    if (!drop_current(&p)) {
        snprintf(err, err_size, "cannot remove the current version");
        return false;
    }
    return step("current-removed");
}

bool lx_slots_commit_next(const lx_config_t * cfg, const char * what) {
    (void) what;
    slot_paths_t p;
    if (!paths(cfg, &p) || !lx_is_dir(p.next)) return false;
    if (!rename_synced(p.root, p.next, p.current)) return false;
    if (!step("next-to-current")) return false;
    if (!journal_remove(&p)) return false;
    return step("journal-removed");
}

bool lx_slots_install_abort(const lx_config_t * cfg, char * err, size_t err_size) {
    slot_paths_t p;
    if (!paths(cfg, &p)) return false;
    lx_rm_rf(p.next);
    lx_rm_rf(p.partial);
    if (!lx_is_dir(p.current) && !restore_backup(&p, err, err_size)) return false;
    return journal_remove(&p);
}

bool lx_slots_rollback(const lx_config_t * cfg) {
    slot_paths_t p;
    char err[200] = "";
    if (!paths(cfg, &p) || !has_player(p.backup)) return false;
    lx_rm_rf(p.next);
    if (lx_is_dir(p.current)) {
        if (!save_current(cfg, &p, err, sizeof(err))) {
            lx_log("slots: roll back: %s", err);
            lx_update_log(cfg, "rollback: %s", err);
            return false;
        }
        if (!step("backup-saved")) return false;
    }
    if (!journal_write(&p, "rollback", false)) return false;
    if (!step("journal")) return false;
    if (!drop_current(&p)) return false;
    if (!step("current-removed")) return false;
    /* From here on the journal finishes the roll back. */
    if (!journal_write(&p, "rollback", true)) return false;
    if (!step("committed")) return false;
    if (!restore_backup(&p, err, sizeof(err))) {
        lx_log("slots: roll back: %s", err);
        return false;
    }
    if (!step("next-to-current")) return false;
    if (!rotate_backup(&p)) return false;
    if (!step("backup-rotated")) return false;
    if (!journal_remove(&p)) return false;
    return step("journal-removed");
}

bool lx_slots_swap_pending(const lx_config_t * cfg) {
    slot_paths_t p;
    return paths(cfg, &p) && lx_exists(p.journal);
}

bool lx_slots_backup_available(const lx_config_t * cfg) {
    slot_paths_t p;
    return paths(cfg, &p) && p.card[0] && has_player(p.backup);
}

bool lx_slot_version(const lx_config_t * cfg, const char * slot, char * out, size_t size) {
    char dir[PATH_MAX], file[PATH_MAX], buf[4096];
    snprintf(out, size, "unknown");
    if (strcmp(slot, "previous") == 0) {
        slot_paths_t p;
        if (!paths(cfg, &p) || !p.card[0]) return false;
        snprintf(dir, sizeof(dir), "%s", p.backup);
    } else if (!lx_join(dir, sizeof(dir), cfg->slots_root, slot)) {
        return false;
    }
    if (!lx_is_dir(dir)) return false;
    if (lx_join(file, sizeof(file), dir, LX_MANIFEST) && lx_read_file(file, buf, sizeof(buf))) {
        for (char * line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            if (strncmp(line, "version ", 8) == 0) {
                snprintf(out, size, "%s", line + 8);
                return true;
            }
        }
    }
    if (lx_join(file, sizeof(file), dir, LX_VERSION_FILE) && lx_read_file(file, buf, sizeof(buf))) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0]) snprintf(out, size, "%s", buf);
    }
    return true;
}
