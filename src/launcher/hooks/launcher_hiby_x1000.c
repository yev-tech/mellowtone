/* Launcher hooks for the R3 Pro (Ingenic X1000, stock HiBy base system):
 * what the launcher does here besides starting the player -- USB device-mode
 * fix, ADB watch, debug power log -- and the stock way into recovery. */
#define _GNU_SOURCE
#include "board_launcher.h"
#include "hooks_common.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#define DWC "/sys/devices/platform/jz-dwc2"
#define ANDROID0 "/sys/class/android_usb/android0"
#define PSY "/sys/class/power_supply"

static board_launcher_log_fn hook_log;
static const board_desc_t * hook_board;
static char debug_flag_path[256];

/* The dwc2 controller can come up as USB host with its own 5 V VBUS on
 * ("gadget pullup defered, current mode: host"): then the charger ignores
 * the cable, nothing charges and ADB never enumerates. No stock userspace
 * writes otg_id_pull; force device mode. */
static void usb_device_mode(board_launcher_log_fn log) {
    char pull[16], vbus[16];
    log("dwc2: otg_id_pull=%s vbus=%s -> device mode", blh_read(DWC "/otg_id_pull", pull, sizeof(pull)),
        blh_read(DWC "/vbus", vbus, sizeof(vbus)));
    blh_write(DWC "/otg_id_pull", "high");
    blh_write(DWC "/vbus", "off");
}

static void hiby_early(const board_desc_t * board, board_launcher_log_fn log) {
    (void) board;
    usb_device_mode(log);
}

static int k90adb_start(void) {
    char * argv[] = { (char *) "/etc/init.d/K90adb", (char *) "start", NULL };
    return blh_run(argv, 15000);
}

static bool adb_flag_set(void) {
    const char * flag = hook_board ? hook_board->paths.adb_flag : NULL;
    return flag && access(flag, F_OK) == 0;
}

/* The shell launcher's adb_watch: logs every USB state change, keeps the
 * port in device mode and, while the ADB flag exists, reruns K90adb (at most
 * every 30 s) when the cable is in but the gadget has not been CONFIGURED
 * for 10 s. */
static void * adb_watch(void * arg) {
    (void) arg;
    if (adb_flag_set()) {
        blh_sleep_ms(8000);
        k90adb_start();
    }
    char last[512] = "", now[512];
    int bad = 0;
    long since = -1000;
    bool last_host = false;
    for (;;) {
        char st[32], on[8], udc[32], pull[16], vbus[16], fn[64], en[8], otg[8];
        blh_read(ANDROID0 "/state", st, sizeof(st));
        blh_read(PSY "/usb/online", on, sizeof(on));
        blh_read(DWC "/dwc2/udc/dwc2/state", udc, sizeof(udc));
        blh_read(DWC "/otg_id_pull", pull, sizeof(pull));
        blh_read(DWC "/vbus", vbus, sizeof(vbus));
        blh_read(ANDROID0 "/functions", fn, sizeof(fn));
        blh_read(ANDROID0 "/enable", en, sizeof(en));
        blh_read(DWC "/dwc2/otg_id", otg, sizeof(otg));
        snprintf(now, sizeof(now), "online=%s pull=%s vbus=%s state=%s udc=%s fn=%s en=%s otg=%s", on, pull, vbus,
                 st, udc, fn, en, otg);
        if (strcmp(now, last) != 0) {
            hook_log("usb: %s", now);
            snprintf(last, sizeof(last), "%s", now);
        }
        /* otg_id 1 = the controller switched to host (ID read as grounded);
         * the otg_id_pull readback stays "low" even in device mode. */
        bool host = strcmp(otg, "1") == 0;
        if (host && !last_host) usb_device_mode(hook_log);
        last_host = host;
        if (strcmp(on, "1") == 0 && strcmp(st, "CONFIGURED") != 0) bad += 2;
        else bad = 0;
        long t = blh_uptime_s();
        if (adb_flag_set() && bad >= 10 && t - since >= 30) {
            hook_log("adb: cable in but not configured, forcing device mode + K90adb start");
            usb_device_mode(hook_log);
            k90adb_start();
            since = t;
        }
        blh_sleep_ms(2000);
    }
    return NULL;
}

/* One line a minute while debug logging is on (the card's flag). */
static void * power_log(void * arg) {
    (void) arg;
    for (;;) {
        if (debug_flag_path[0] && access(debug_flag_path, F_OK) == 0) {
            char cap[16], mv[24], status[24], ma[24], online[8], state[32], lim[16], cur[16], load[64];
            long mvl = 0, mal = 0;
            sscanf(blh_read(PSY "/battery/voltage_now", mv, sizeof(mv)), "%ld", &mvl);
            sscanf(blh_read(PSY "/axp_battery/current_now", ma, sizeof(ma)), "%ld", &mal);
            blh_read("/proc/loadavg", load, sizeof(load));
            char * sp = strchr(load, ' ');
            if (sp) *sp = '\0';
            hook_log("power: cap=%s mv=%ld %s ma=%ld usb=%s/%s lim=%s/%s load=%s",
                     blh_read(PSY "/battery/capacity", cap, sizeof(cap)), mvl / 1000,
                     blh_read(PSY "/battery/status", status, sizeof(status)), mal / 1000,
                     blh_read(PSY "/usb/online", online, sizeof(online)),
                     blh_read(ANDROID0 "/state", state, sizeof(state)),
                     blh_read(PSY "/usb/charge_control_limit_max", lim, sizeof(lim)),
                     blh_read(PSY "/usb/constant_charge_current_max", cur, sizeof(cur)), load);
        }
        blh_sleep_ms(60000);
    }
    return NULL;
}

static void start_thread(void * (*fn)(void *)) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&t, &attr, fn, NULL) != 0 && hook_log) hook_log("hooks: thread start failed");
    pthread_attr_destroy(&attr);
}

static void hiby_session(const board_desc_t * board, board_launcher_log_fn log, const char * debug_flag) {
    hook_board = board;
    hook_log = log;
    snprintf(debug_flag_path, sizeof(debug_flag_path), "%s", debug_flag ? debug_flag : "");
    start_thread(adb_watch);
    start_thread(power_log);
}

static bool hiby_enable_adb(const board_desc_t * board, board_launcher_log_fn log) {
    (void) board;
    usb_device_mode(log);
    int rc = k90adb_start();
    log("adb: K90adb start exit=%d", rc);
    return rc == 0;
}

/* As the stock player: the "recovery" signature through the jz reset proc
 * file, then a reboot. U-Boot sees the signature and boots the recovery
 * kernel in mtd3, whose recoveryd applies r3pro.upt / update.upt from the
 * card. The write itself may restart at once,
 * so everything is synced first. */
static bool hiby_enter_recovery(const board_desc_t * board, board_launcher_log_fn log) {
    (void) board;
    sync();
    if (!blh_write("/proc/jz/reset/reset", "recovery")) {
        log("recovery: writing /proc/jz/reset/reset failed");
        return false;
    }
    sync();
    blh_sleep_ms(1000);
    reboot(RB_AUTOBOOT);
    log("recovery: reboot failed");
    return false;
}

static const char * const hiby_images[] = { "r3pro.upt", "update.upt", NULL };

const struct board_launcher_ops launcher_hiby_x1000_ops = {
    .name = "hiby-x1000",
    .slots_root = "/data/mellowtone",
    .boot_dir = "/data/mellowtone-boot",
    .factory_dir = "/usr/mellowtone/factory", /* image install: <paths.image_root>/factory */
    .stock_player = "/usr/bin/hiby_player", /* bundle install: the stock player is still there */
    .firmware_images = hiby_images,
    .firmware_any_upt = false,
    .firmware_format = BOARD_FW_ISO_UPDATE_TXT,
    .firmware_product = "r3pro",
    .fb_slcd = true,
    /* Our logo from launcher start until the player paints; the stock boot
     * logo before that comes from the firmware. */
    .splash_jpeg = "/data/mellowtone/themes/boot_animation/en/0.jpg",
    .ntp = true,
    .early = hiby_early,
    .session = hiby_session,
    .enable_adb = hiby_enable_adb,
    .enter_recovery = hiby_enter_recovery,
    .release_sd = NULL, /* reboot/poweroff go through init, which unmounts the card */
};
