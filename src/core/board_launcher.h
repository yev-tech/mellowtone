#ifndef BOARD_LAUNCHER_H
#define BOARD_LAUNCHER_H

/* The board's part of mellowtone-launcher: BOARD.launcher. Data
 * (where the version slots, the boot state and the factory copy live, how a
 * firmware image looks) plus a few hooks the launcher calls at boot. The
 * player reads the data too: "Restart into recovery menu" writes the
 * one-shot flag in boot_dir, and About shows "Update available" when a
 * firmware image or a player update package is on the card.
 *
 * The hooks (src/launcher/hooks/) use libc only, so linking them into the
 * player through board.c costs nothing else. */

#include <stdbool.h>

#include "board.h"

typedef void (*board_launcher_log_fn)(const char * fmt, ...) __attribute__((format(printf, 1, 2)));

/* firmware_format: what the card's .upt is and how its parts are checked. */
typedef enum {
    /* ISO 9660 with UPDATE.TXT (name=/file_path=/md5= per part) and an optional
     * VERSION.TXT, applied by the recovery kernel's recoveryd (R3 Pro). */
    BOARD_FW_ISO_UPDATE_TXT = 0,
    /* ISO 9660 with ota_v0/ota_update.in (img_name/img_size/img_md5) and each
     * image split into <img_name>.NNNN.<md5> chunks (X1600, boards/common/repack_upt.sh). */
    BOARD_FW_INGENIC_OTA,
} board_fw_format_t;

/* One-shot flag in boot_dir: the launcher opens its menu at the next start
 * (written by the player's "Restart into recovery menu"). */
#define BOARD_LAUNCHER_MENU_FLAG "open_menu"
/* Boot-loop counter in boot_dir (two small writes per boot). */
#define BOARD_LAUNCHER_BOOT_COUNTER "boot_attempts"
/* The player's exit status asking the launcher to reboot (idle_shutdown.c). */
#define BOARD_LAUNCHER_EXIT_REBOOT 75

struct board_launcher_ops {
    const char * name;

    /* ---- data ---- */
    const char * slots_root;    /* flash: the installed version, current/ (the previous one is on the card) */
    const char * boot_dir;      /* flash: boot counter and the one-shot menu flag */
    const char * factory_dir;   /* rootfs: factory player (+ launcher) of an image install, or NULL */
    const char * stock_player;  /* the vendor player, last before the menu (bundle installs), or NULL */
    const char * const * firmware_images; /* card-root names in the recovery's order, NULL-terminated */
    bool firmware_any_upt;      /* also any other *.upt at the card root (X1600: the recovery
                                   flow upstream used took any name) */
    board_fw_format_t firmware_format;
    const char * firmware_product; /* the image's product name must be this (VERSION.TXT), or NULL */
    bool fb_slcd;               /* smart LCD (jzfb): both driver pages and a pan per frame, as
                                   patches/lvgl_r3pro_slcd_pan.patch does; never blank */
    const char * splash_jpeg;   /* painted at start (screen-sized baseline JPEG), or NULL */
    bool ntp;                   /* set the clock from NTP once Wi-Fi is up (otherwise the
                                   system already does it, e.g. the X1600 udhcpc hook) */

    /* ---- hooks, each may be NULL ---- */
    /* Before the card is checked: R3 Pro forces USB device mode; X1600 mounts
     * the card. Runs once, in the launcher's process. */
    void (*early)(const board_desc_t * board, board_launcher_log_fn log);
    /* Background work for the whole session (R3 Pro: the ADB watch and the
     * debug power log). Starts threads and returns. */
    void (*session)(const board_desc_t * board, board_launcher_log_fn log, const char * debug_flag);
    /* Switch ADB on now (the flag in BOARD.paths.adb_flag is the caller's). */
    bool (*enable_adb)(const board_desc_t * board, board_launcher_log_fn log);
    /* Reboot into the recovery that applies the card's image. Does not
     * return on success. */
    bool (*enter_recovery)(const board_desc_t * board, board_launcher_log_fn log);
    /* Release the card before a reboot/poweroff the launcher does itself
     * (X1600: unmount, clearing the FAT dirty mark), or NULL. */
    void (*release_sd)(const board_desc_t * board);
};

/* Hooks a descriptor can point .launcher at (src/launcher/hooks/). */
extern const struct board_launcher_ops launcher_hiby_x1000_ops; /* R3 Pro, stock HiBy base system */
extern const struct board_launcher_ops launcher_x1600_ops;      /* R1, R3 Pro II, R3 II 2025 */

#endif
