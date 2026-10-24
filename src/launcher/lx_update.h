#ifndef LX_UPDATE_H
#define LX_UPDATE_H

/* Version slots, player update packages and firmware images. */

#include "lx.h"

#include <limits.h>

/* Test hook: called after each step that changes the slots on flash, with
 * the step's name; returning false stops right there, as a power cut would
 * (nothing is cleaned up). NULL on the device. */
typedef bool (*lx_step_hook_t)(const char * step);
extern lx_step_hook_t lx_step_hook;

typedef void (*lx_progress_fn)(void * ctx, int percent, const char * what);

/* ---- lx_slots.c ----
 * Flash holds one version, <slots_root>/current. The previous one is kept
 * on the card, SD/.mellowtone/backup/player, so an update needs room in
 * flash for the new version only.
 *
 * Install: the current version is copied to the card as player.new and read
 * back (checked); swap.journal is written; player.new replaces the backup;
 * current is removed; the package unpacks into next.partial -> next, which
 * becomes current; the journal is removed.
 * Roll back (menu, three quick crashes, or three short boots): current is saved to the card as
 * player.new; journal; current removed; the journal is marked committed;
 * the backup is copied back (checked) as next -> current; player.new
 * replaces the backup (the two versions trade places); journal removed.
 *
 * current is removed by renaming it to current.old first, so a current/
 * that exists is always complete. Directory renames are followed by a
 * directory sync. lx_slots_recover()
 * finishes any of these from wherever a power cut stopped it: an install
 * with a complete current or next keeps it, else the backup is copied back;
 * a roll back stopped before current was removed is dropped. With no backup
 * to copy back (the card is gone), the factory player starts. */
bool lx_slots_recover(const lx_config_t * cfg);
/* Install, part 1: room in flash for new_bytes, the backup on the card,
 * the journal, current removed. False (err set) changes nothing in flash. */
bool lx_slots_install_begin(const lx_config_t * cfg, const char * version, long long new_bytes, char * err,
                            size_t err_size);
/* Install, part 2: next/ is complete: it becomes current; journal removed. */
bool lx_slots_commit_next(const lx_config_t * cfg, const char * what);
/* Install failed after part 1: the backup is copied back as current. */
bool lx_slots_install_abort(const lx_config_t * cfg, char * err, size_t err_size);
/* current <-> the card backup (see above). False when there is no backup or
 * it could not be done; lx_slots_swap_pending() then tells whether the next
 * start completes it. */
bool lx_slots_rollback(const lx_config_t * cfg);
/* A journaled install or roll back is unfinished: lx_slots_recover()
 * completes it at the next start. */
bool lx_slots_swap_pending(const lx_config_t * cfg);
/* A previous version is on the card. */
bool lx_slots_backup_available(const lx_config_t * cfg);
/* The version of "current" (flash) or "previous" (the card backup):
 * MANIFEST's version, else VERSION's first line, else "unknown". False when
 * it does not exist. */
bool lx_slot_version(const lx_config_t * cfg, const char * slot, char * out, size_t size);

/* ---- lx_pkg.c ----
 * SD/.mellowtone/update/mellowtone-<board>-<version>.tar: a ustar archive
 * with a MANIFEST:
 *     mellowtone-package 1
 *     board r3pro
 *     version 0.2.0
 *     file <sha256> <path>          (one per regular file; "mellowtone" required)
 * tools/release/make_update_package.sh writes one. */
typedef struct {
    char path[PATH_MAX];
    char version[64];
} lx_pkg_ref_t;

/* The newest package for this board on the card. */
bool lx_pkg_find(const lx_config_t * cfg, lx_pkg_ref_t * out);
/* Checks board, version, checksums; unpacks to next; swaps; deletes the
 * package; logs to update.log. err gets a one-line reason on failure. */
bool lx_pkg_install(const lx_config_t * cfg, const lx_pkg_ref_t * pkg, lx_progress_fn progress, void * ctx,
                    char * err, size_t err_size);

/* ---- lx_fw.c ---- */
#define LX_FW_MAX_PARTS 8
#define LX_FW_BOARD_MARKER "mellowtone.in" /* X1600 image root: "board=<name>" (boards/common/repack_upt.sh) */
typedef struct {
    char path[PATH_MAX];
    char file[128];          /* name on the card */
    long long size;
    char product[48];        /* VERSION.TXT name= (R3 Pro), the board marker (X1600), or "" */
    char version[64];        /* VERSION.TXT ver=, or ota_version */
    int nparts;
    struct {
        char name[32];       /* kernel, rootfs, ... */
        char file[128];      /* inside the image */
        char md5[33];
        long long size;
        bool md5_ok;
    } parts[LX_FW_MAX_PARTS];
    char warning[120];       /* shown, does not refuse */
    char error[160];         /* refuses the install */
} lx_fw_info_t;

/* The image recovery would apply: <board>.upt, then cfg->fw_names in order,
 * then (fw_any_upt) any other *.upt at the card root. */
bool lx_fw_find(const lx_config_t * cfg, char * path, size_t size);
/* Reads the parts list (no checksums yet). False with info->error set when
 * the image is unreadable or not for this board. */
bool lx_fw_inspect(const lx_config_t * cfg, const char * path, lx_fw_info_t * info);
/* md5 of every part against the list. */
bool lx_fw_verify(lx_fw_info_t * info, int fw_format, lx_progress_fn progress, void * ctx);

#endif
