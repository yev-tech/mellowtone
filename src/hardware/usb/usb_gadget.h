#ifndef USB_GADGET_H
#define USB_GADGET_H

/* USB device modes, back-end side. The descriptor picks:
 *
 *   .usb_gadget       how the port is set up
 *       usb_gadget_configfs_ops     configfs android0/adb_demo through the
 *                                   stock scripts (X1600; usb_gadget_configfs.c)
 *       usb_gadget_android_usb_ops  legacy /sys/class/android_usb/android0
 *                                   (kernel 3.10, R3 Pro; usb_gadget_android_usb.c)
 *   .storage_export   how the card reaches a computer
 *       storage_export_shared_ops      the card stays mounted while the computer
 *                                      has it, read-only for the player; it is
 *                                      unmounted and mounted again afterwards
 *                                      (X1600)
 *       storage_export_sys_server_ops  the card is lent: sys_server unmounts it,
 *                                      SETLUN exports the whole disk, the gadget
 *                                      switches to mass storage; back again on
 *                                      eject, unplug or the player's button (R3 Pro)
 *
 * The UI-facing API stays usb_mode_control.h; the lending sequence (closing
 * and reopening everything on the card) is usb_storage_lend.h.
 *
 * Every path below is relative to one sysfs root, every board value comes
 * from one descriptor and every command goes through one runner, so host
 * tests run the real back ends against fake trees: usb_gadget_test_setup(). */

#include "board.h"
#include "usb_mode_control.h"

#include <stdbool.h>
#include <stddef.h>

struct board_usb_gadget_ops {
    const char * name;
    /* Brings the port into `mode` (usb_mode_control_apply(), worker thread):
     * CHARGE is the default -- configfs binds its charge-only gadget (a LUN
     * with no medium), android_usb presents its idle set-up (ADB off); in
     * STORAGE configfs binds the storage gadget itself, android_usb leaves
     * lending the card to .storage_export. True once the result is verified. */
    bool (*apply)(usb_mode_t mode);
    /* The live mode, when it is unambiguous. */
    bool (*detect)(usb_mode_t * out);
    /* A computer configured the port in Storage mode and has the card. */
    bool (*storage_host_configured)(void);
    /* A computer configured the port, whatever it presents: how a computer
     * is told from a wall charger (usb_connection.h), and the moment to lend
     * the card (lending back ends). */
    bool (*host_connected)(void);
    /* While the card is lent: switch the port to its mass-storage set-up and
     * back to idle. NULL where apply(STORAGE) already presents the card. */
    bool (*set_storage_function)(bool on);
    /* Shared card: empty the mass-storage LUN, so a computer plugged in
     * again sees no disk until apply(STORAGE) presents it anew. NULL where
     * the card is lent. */
    void (*eject_storage)(void);
};

struct board_storage_export_ops {
    const char * name;
    /* The card is unmounted while a computer has it, so everything the
     * player keeps open on it must be closed first (usb_storage_lend.h). */
    bool lends_card;
    /* lends_card only (worker thread): hand the card over / take it back.
     * export_card fails without side effects left behind except what
     * reclaim_card undoes; reclaim_card is safe to call after any failure. */
    bool (*export_card)(void);
    bool (*reclaim_card)(void);
    /* lends_card only: the computer ejected the card (LUN empty again). */
    bool (*host_ejected)(void);
    /* !lends_card: a computer configured the port and may write the card
     * while it stays mounted here. Remounts it read-only so nothing in the
     * player can write it meanwhile; true once it is (or nothing is
     * mounted). Called again until it succeeds. NULL: nothing to do. */
    bool (*session_started)(void);
    /* !lends_card: the computer's session ended (cable out or mode changed).
     * What the kernel cached about the card (FAT, exFAT allocation bitmap,
     * directories) may no longer match it, so the card is unmounted, never
     * lazily: true once it is not mounted (the caller mounts it again),
     * false while something still holds it. NULL: nothing to do. */
    bool (*session_ended)(void);
};

/* The board's back ends (the descriptor's, or a test's). */
const struct board_usb_gadget_ops * usb_gadget_ops(void);
const struct board_storage_export_ops * usb_storage_export_ops(void);
const board_desc_t * usb_board(void);

/* ---- shared helpers for the back ends -------------------------------- */

/* Absolute path of rel (relative to the sysfs root, "/sys" on the device). */
bool usb_sysfs_path(const char * rel, char * out, size_t out_size);
/* Writes value exactly (no newline: what the stock player writes). */
bool usb_sysfs_write(const char * rel, const char * value);
/* First line, without the newline; false if missing. */
bool usb_sysfs_read(const char * rel, char * out, size_t out_size);
/* Path of rel under the proc root ("/proc" on the device). */
bool usb_proc_path(const char * rel, char * out, size_t out_size);
/* Runs argv (exec_allow applies); exit code, -1 when it could not run. */
int usb_run(char * const argv[], int timeout_ms);

/* sys_server request with one of two results ("<prefix>OK", "<prefix>FAIL"). */
typedef bool (*usb_sys_request_fn)(const char * cmd, const char * const * result_prefixes, int timeout_ms,
                                   char * out, size_t out_size);

/* Processes (pid, name) holding a file or directory under `root` open,
 * mapping one into memory, or using it as their working directory, read
 * from <proc root>/<pid>/{cwd,fd,maps,comm}. Writes up to max entries into pids/names (names may be NULL) and
 * returns how many it found (also beyond max). */
int usb_busy_scan(const char * root, int * pids, char (*names)[32], int max);
/* What one of those processes holds under `root` (cwd, each fd's file, each
 * mapped file), one line each to stderr; returns how many. */
int usb_busy_log_files(const char * root, int pid);

/* ---- tests ------------------------------------------------------------- */

typedef int (*usb_run_fn)(char * const argv[], int timeout_ms);
/* board NULL = BOARD; sysfs_root NULL = "/sys"; proc_root NULL = "/proc";
 * run NULL = the real subprocess runner; request NULL = hiby_sys_request().
 * The descriptor's absolute paths (card, LUN file, mount point) are used as
 * they are: a test descriptor points them into its temp tree. Not
 * thread-safe. */
void usb_gadget_test_setup(const board_desc_t * board, const char * sysfs_root, const char * proc_root,
                           usb_run_fn run, usb_sys_request_fn request);
usb_sys_request_fn usb_sys_request(void);
/* Tests only: called with every usb_sysfs_write() (rel, value), in order. */
void usb_gadget_test_trace(void (*trace)(const char * rel, const char * value));

#endif /* USB_GADGET_H */
