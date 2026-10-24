/* Launcher hooks for the X1600 boards (R1, R3 Pro II, R3 II 2025): what has
 * to happen before the player starts (mount the card, which nothing earlier
 * in boot does), the stock ADB scripts, and the bootmode.sh way into
 * recovery. */
#define _GNU_SOURCE
#include "board_launcher.h"
#include "hooks_common.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SD_PARTITION "/dev/mmcblk0p1"
#define SD_WHOLE_DISK "/dev/mmcblk0"

/* Deadlines of the bootloader's SD readiness wait: give up after 5 s when
 * there is no sign of a card at all; with an MMC host reporting one, or a
 * block node present, keep trying up to 20 s (slow cards after a cold boot). */
#define SD_NO_CARD_DEADLINE_MS 5000
#define SD_HARD_DEADLINE_MS 20000
#define SD_POLL_MS 100
#define SD_MOUNT_TIMEOUT_MS 3000

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool sd_mounted(const char * mount_point) {
    char parent[256];
    snprintf(parent, sizeof(parent), "%s/..", mount_point);
    struct stat p, m;
    return stat(parent, &p) == 0 && stat(mount_point, &m) == 0 && p.st_dev != m.st_dev;
}

/* sysfs says an SD card sits in an MMC host (before its block node exists). */
static bool mmc_card_present(void) {
    DIR * hosts = opendir("/sys/class/mmc_host");
    if (!hosts) return false;
    bool found = false;
    struct dirent * h;
    while (!found && (h = readdir(hosts))) {
        if (h->d_name[0] == '.') continue;
        char hp[300];
        snprintf(hp, sizeof(hp), "/sys/class/mmc_host/%s", h->d_name);
        DIR * hd = opendir(hp);
        if (!hd) continue;
        size_t n = strlen(h->d_name);
        struct dirent * c;
        while ((c = readdir(hd))) {
            if (strncmp(c->d_name, h->d_name, n) != 0 || c->d_name[n] != ':') continue;
            char tp[600], type[16];
            snprintf(tp, sizeof(tp), "%s/%s/type", hp, c->d_name);
            if (strcmp(blh_read(tp, type, sizeof(type)), "SD") == 0) { found = true; break; }
        }
        closedir(hd);
    }
    closedir(hosts);
    return found;
}

static void try_mount(const char * node, const char * mp) {
    char * vfat[] = { (char *) "mount", (char *) "-t", (char *) "vfat", (char *) "-o",
                      (char *) "rw,relatime,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed",
                      (char *) node, (char *) mp, NULL };
    char * exfat[] = { (char *) "mount", (char *) "-t", (char *) "exfat", (char *) "-o", (char *) "rw,relatime",
                       (char *) node, (char *) mp, NULL };
    char * ntfs[] = { (char *) "/usr/bin/ntfs-3g", (char *) "-o", (char *) "rw,relatime,big_writes,umask=0022",
                      (char *) node, (char *) mp, NULL };
    char * const * tries[] = { vfat, exfat, ntfs };
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        blh_run(tries[i], SD_MOUNT_TIMEOUT_MS);
        if (sd_mounted(mp)) return;
    }
}

static void x1600_early(const board_desc_t * board, board_launcher_log_fn log) {
    const char * mp = board->paths.sd_root;
    char parent[256];
    snprintf(parent, sizeof(parent), "%s", mp);
    char * slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        mkdir(parent, 0755);
    }
    mkdir(mp, 0755);
    long long start = now_ms();
    bool evidence = false;
    while (!sd_mounted(mp)) {
        long long elapsed = now_ms() - start;
        bool node = false;
        const char * nodes[] = { SD_PARTITION, SD_WHOLE_DISK };
        for (size_t i = 0; i < 2 && !sd_mounted(mp); i++) {
            if (access(nodes[i], F_OK) != 0) continue;
            node = true;
            try_mount(nodes[i], mp);
            if (sd_mounted(mp)) log("sd: mounted %s after %lld ms", nodes[i], now_ms() - start);
        }
        if (sd_mounted(mp)) break;
        if (node || mmc_card_present()) evidence = true;
        if ((!evidence && elapsed >= SD_NO_CARD_DEADLINE_MS) || elapsed >= SD_HARD_DEADLINE_MS) {
            log("sd: not mounted after %lld ms (%s)", elapsed, evidence ? "card seen" : "no card");
            return;
        }
        blh_sleep_ms(SD_POLL_MS);
    }
}

static bool x1600_enable_adb(const board_desc_t * board, board_launcher_log_fn log) {
    (void) board;
    char * start[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
    int rc = blh_run(start, 15000);
    /* S440adb backgrounds adbserver.sh, which mounts functionfs and starts
     * adbd; the UDC bind (a separate stock script) must wait for it. */
    for (int i = 0; i < 50 && access("/dev/usb-ffs/adb", F_OK) != 0; i++) blh_sleep_ms(100);
    char * bind[] = { (char *) "/sbin/usb_adb_enable.sh", NULL };
    int rc2 = blh_run(bind, 15000);
    log("adb: S440adb start exit=%d, usb_adb_enable exit=%d", rc, rc2);
    return rc == 0 && rc2 == 0;
}

/* The kernel marks a FAT volume dirty while it is mounted writable and
 * clears the mark only on unmount or a read-only remount. */
static void x1600_release_sd(const board_desc_t * board) {
    sync();
    const char * mp = board->paths.sd_root;
    if (umount2(mp, 0) == 0) return;
    if (errno == EINVAL || errno == ENOENT) return;
    mount(NULL, mp, NULL, MS_REMOUNT | MS_RDONLY, NULL);
}

static bool x1600_enter_recovery(const board_desc_t * board, board_launcher_log_fn log) {
    char * argv[] = { (char *) "/usr/bin/bootmode.sh", (char *) "Recovery", NULL };
    int rc = blh_run(argv, 15000);
    if (rc != 0) {
        log("recovery: bootmode.sh Recovery exit=%d", rc);
        return false;
    }
    x1600_release_sd(board);
    reboot(RB_AUTOBOOT);
    log("recovery: reboot failed");
    return false;
}

/* <board>.upt is always tried first (launcher); update.upt, then any .upt. */
static const char * const x1600_images[] = { "update.upt", NULL };

const struct board_launcher_ops launcher_x1600_ops = {
    .name = "x1600",
    .slots_root = "/data/mellowtone",       /* /data -> usr/data, the writable partition */
    .boot_dir = "/data/mellowtone-boot",
    .factory_dir = "/usr/mellowtone",       /* boards/common/repack_upt.sh installs it there */
    .stock_player = NULL,                   /* Mellowtone images ship no stock player */
    .firmware_images = x1600_images,
    .firmware_any_upt = true,
    .firmware_format = BOARD_FW_INGENIC_OTA,
    .firmware_product = NULL,               /* the image names no product; lx_fw.c checks its mellowtone.in board marker */
    .fb_slcd = false,
    .splash_jpeg = "/usr/resource/litegui/theme2/boot_animation/en/0.jpg",
    .ntp = false,                           /* the image's udhcpc hook runs ntpdate */
    .early = x1600_early,
    .session = NULL,
    .enable_adb = x1600_enable_adb,
    .enter_recovery = x1600_enter_recovery,
    .release_sd = x1600_release_sd,
};
