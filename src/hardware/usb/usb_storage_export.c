/* Storage hand-over back ends: see usb_gadget.h.
 *
 * storage_export_sys_server_ops follows the stock R3 Pro (sys_server's
 * MOUNT handlers), with the gadget switch moved after SETLUN so the computer only sees
 * a disk once it is really there, and the switch back before the card is
 * taken back:
 *
 *   export   sync; nothing on the card (/proc); MOUNT:UMOUNT:<sd root>
 *            (sys_server, result on sys_client); wait for the mount (and its
 *            directory) to go; MOUNT:SETLUN:<lun file> <whole disk>;
 *            functions -> mass storage
 *   reclaim  functions -> idle; MOUNT:SETLUN:<lun file> empty, and the LUN
 *            file read back empty; MOUNT:MOUNT:<partition, or the disk>
 *            <sd root>
 *
 * sys_server's UMOUNT falls back to a lazy unmount and still says OK, so the
 * caller must have closed everything on the card first (usb_storage_lend.c
 * checks /proc, and export checks again right before UMOUNT). Its MOUNT only
 * mounts when the mount point directory does not exist (UMOUNT removes it):
 * a leftover empty directory is removed before asking. The card is never
 * mounted while the LUN may still hold it.
 *
 * storage_export_shared_ops (X1600): the stock storage script exports the
 * whole card while it stays mounted here, so the computer writes the same
 * blocks the player's file system driver has cached. While the computer has
 * it the card is read-only for the player (the caller also pauses its
 * writers: mt_state_set_card_lent()); afterwards it is unmounted and mounted
 * again, so nothing cached from before (FAT, exFAT allocation bitmap,
 * directories) is ever written back over what the computer changed. */
#include "usb_gadget.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UMOUNT_TIMEOUT_MS 8000  /* sys_server retries EBUSY for 2 s, then detaches */
#define SETLUN_TIMEOUT_MS 5000
#define MOUNT_TIMEOUT_MS 15000  /* mount, or mount + ntfs-3g */
#define MOUNT_GONE_WAIT_STEPS 50 /* x 100 ms: the stock player waits 5 s too */
#define REMOUNT_TIMEOUT_MS 5000

/* ---- the card's mount ---------------------------------------------------- */

/* The mount point's line in /proc/mounts: whether it is mounted, and (when
 * read_only is given) whether read-only. The kernel lists the resolved path,
 * so a mount point reached through a symlink (/data -> /usr/data on the
 * X1600 boards) matches by either spelling. */
static bool mount_entry(const char * mount_point, bool * read_only) {
    char path[PATH_MAX];
    if (!usb_proc_path("mounts", path, sizeof(path))) return false;
    char real[PATH_MAX];
    if (!realpath(mount_point, real)) real[0] = '\0';
    FILE * f = fopen(path, "re");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char dev[256], dir[256], type[64], opts[512];
        int n = sscanf(line, "%255s %255s %63s %511s", dev, dir, type, opts);
        if (n < 2 || (strcmp(dir, mount_point) != 0 && (!real[0] || strcmp(dir, real) != 0))) continue;
        found = true;
        if (read_only) *read_only = n == 4 && (strcmp(opts, "ro") == 0 || strncmp(opts, "ro,", 3) == 0);
    }
    fclose(f);
    return found;
}

static bool is_mounted(const char * mount_point) { return mount_entry(mount_point, NULL); }

/* Logs the processes still holding the card; true when there are none. */
static bool card_free(const char * mount_point) {
    int pids[8];
    char names[8][32];
    int n = usb_busy_scan(mount_point, pids, names, 8);
    for (int i = 0; i < n && i < 8; i++)
        fprintf(stderr, "usb_storage: %s still in use by pid %d (%s)\n", mount_point, pids[i], names[i]);
    return n == 0;
}

/* ---- shared (X1600) --------------------------------------------------- */

static bool shared_session_started(void) {
    const char * mount_point = usb_board()->paths.sd_root;
    bool read_only = false;
    if (!mount_point || !mount_entry(mount_point, &read_only) || read_only) return true;
    sync();
    char * argv[] = { (char *) "mount", (char *) "-o", (char *) "remount,ro", (char *) mount_point, NULL };
    int rc = usb_run(argv, REMOUNT_TIMEOUT_MS);
    if (mount_entry(mount_point, &read_only) && !read_only) {
        /* A file still open for writing refuses it: the caller tries again. */
        fprintf(stderr, "usb_storage: remounting %s read-only failed (exit %d)\n", mount_point, rc);
        return false;
    }
    fprintf(stderr, "usb_storage: %s is read-only while the computer has it\n", mount_point);
    return true;
}

static bool shared_session_ended(void) {
    const char * mount_point = usb_board()->paths.sd_root;
    /* A computer plugged in again must not reach the card while it is being
     * taken back: the LUN stays empty until apply(STORAGE) runs again. */
    const struct board_usb_gadget_ops * gadget = usb_gadget_ops();
    if (gadget && gadget->eject_storage) gadget->eject_storage();
    if (!mount_point || !is_mounted(mount_point)) return true;
    sync();
    if (!card_free(mount_point)) return false;
    /* Never lazily: a detached mount would still write its stale view. */
    char * argv[] = { (char *) "umount", (char *) mount_point, NULL };
    int rc = usb_run(argv, UMOUNT_TIMEOUT_MS);
    if (is_mounted(mount_point)) {
        fprintf(stderr, "usb_storage: unmounting %s failed (exit %d)\n", mount_point, rc);
        return false;
    }
    return true;
}

const struct board_storage_export_ops storage_export_shared_ops = {
    .name = "shared",
    .lends_card = false,
    .session_started = shared_session_started,
    .session_ended = shared_session_ended,
};

/* ---- sys_server (R3 Pro) ---------------------------------------------- */

/* Sends cmd and waits for "<ok>..." or "<fail>...". */
static bool sys_request(const char * cmd, const char * ok, const char * fail, int timeout_ms) {
    const char * prefixes[] = { ok, fail, NULL };
    char result[256];
    if (!usb_sys_request()(cmd, prefixes, timeout_ms, result, sizeof(result))) {
        fprintf(stderr, "usb_storage: \"%s\": no answer\n", cmd);
        return false;
    }
    bool good = strncmp(result, ok, strlen(ok)) == 0;
    fprintf(stderr, "usb_storage: \"%s\" -> \"%s\"\n", cmd, result);
    return good;
}

static bool set_lun(const char * value) {
    const board_desc_t * b = usb_board();
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "MOUNT:SETLUN:%s %s", b->usb.storage_lun_file, value);
    return sys_request(cmd, "MOUNT:SETLUN:OK", "MOUNT:SETLUN:FAIL", SETLUN_TIMEOUT_MS);
}

/* The LUN file reads empty: no computer can reach the card through it. An
 * unreadable file is not empty. */
static bool lun_empty(void) {
    const char * lun = usb_board()->usb.storage_lun_file;
    if (!lun) return false;
    FILE * f = fopen(lun, "re");
    if (!f) return false;
    char line[PATH_MAX];
    bool empty = !fgets(line, sizeof(line), f) || line[strspn(line, " \t\r\n")] == '\0';
    fclose(f);
    return empty;
}

static bool sys_server_export(void) {
    const board_desc_t * b = usb_board();
    const char * mount_point = b->paths.sd_root;
    if (!mount_point || !b->usb.storage_lun_file || !b->usb.storage_disk) return false;
    sync();

    /* Checked again right before UMOUNT: sys_server detaches a busy card
     * lazily and reports success, and whatever still had it open would then
     * write a card the computer owns. */
    if (!card_free(mount_point)) return false;
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "MOUNT:UMOUNT:%s", mount_point);
    if (!sys_request(cmd, "MOUNT:UMOUNT:OK", "MOUNT:UMOUNT:FAIL", UMOUNT_TIMEOUT_MS)) return false;
    for (int i = 0; i < MOUNT_GONE_WAIT_STEPS && (is_mounted(mount_point) || access(mount_point, F_OK) == 0); i++)
        usleep(100000);
    if (is_mounted(mount_point)) {
        fprintf(stderr, "usb_storage: %s is still mounted after MOUNT:UMOUNT\n", mount_point);
        return false;
    }

    if (!set_lun(b->usb.storage_disk)) return false;
    const struct board_usb_gadget_ops * gadget = usb_gadget_ops();
    if (!gadget || !gadget->set_storage_function || !gadget->set_storage_function(true)) {
        fprintf(stderr, "usb_storage: could not switch the port to mass storage\n");
        return false;
    }
    return true;
}

static bool sys_server_reclaim(void) {
    const board_desc_t * b = usb_board();
    const char * mount_point = b->paths.sd_root;
    if (!mount_point || !b->usb.storage_lun_file || !b->usb.storage_disk) return false;

    /* The computer loses the disk first, then the LUN lets go of it. */
    const struct board_usb_gadget_ops * gadget = usb_gadget_ops();
    if (gadget && gadget->set_storage_function && !gadget->set_storage_function(false))
        fprintf(stderr, "usb_storage: could not switch the port back to idle\n");
    if (!set_lun("empty")) {
        /* The answer may be lost while the LUN did let go: the file says.
         * Still holding the card: never mount it, the state machine tries
         * the reclaim again. */
        if (!lun_empty()) {
            fprintf(stderr, "usb_storage: SETLUN empty failed and the LUN still holds the card; not mounting\n");
            return false;
        }
        fprintf(stderr, "usb_storage: SETLUN empty failed, but the LUN is empty\n");
    }

    if (is_mounted(mount_point)) return true;
    const char * part = b->usb.storage_partition;
    const char * dev = part && access(part, F_OK) == 0 ? part : b->usb.storage_disk;
    for (int attempt = 0; attempt < 2; attempt++) {
        /* sys_server mounts only when the directory is missing. */
        if (access(mount_point, F_OK) == 0 && rmdir(mount_point) != 0 && errno != ENOENT)
            fprintf(stderr, "usb_storage: rmdir %s: %s\n", mount_point, strerror(errno));
        char cmd[PATH_MAX * 2 + 32];
        snprintf(cmd, sizeof(cmd), "MOUNT:MOUNT:%s %s", dev, mount_point);
        bool ok = sys_request(cmd, "MOUNT:MOUNT:OK", "MOUNT:MOUNT:FAIL", MOUNT_TIMEOUT_MS);
        for (int i = 0; ok && i < 20 && !is_mounted(mount_point); i++) usleep(100000);
        if (is_mounted(mount_point)) return true;
    }
    fprintf(stderr, "usb_storage: %s did not come back\n", mount_point);
    return false;
}

const struct board_storage_export_ops storage_export_sys_server_ops = {
    .name = "sys_server",
    .lends_card = true,
    .export_card = sys_server_export,
    .reclaim_card = sys_server_reclaim,
    .host_ejected = lun_empty,
    .session_started = NULL,
    .session_ended = NULL,
};
