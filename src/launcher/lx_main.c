/* mellowtone-launcher: started by the init shim (boards/<board>/launcher/),
 * runs the board's hooks, then lx_run() (lx_boot.c) with the device's
 * processes, clock, power, framebuffer and keys. */
#define _GNU_SOURCE
#include "lx.h"
#include "lx_fb.h"
#include "lx_input.h"
#include "board.h"
#include "board_launcher.h"
#include "hooks/hooks_common.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef LX_BUILD_STAMP
#define LX_BUILD_STAMP "dev"
#endif

#define REDEPLOY_MARKER "/tmp/mellowtone_redeploy" /* the developer deploy script's marker */

static const struct board_launcher_ops * ops;

static void log_fn(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_fn(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lx_vlog(fmt, ap);
    va_end(ap);
}

/* ---- platform ----------------------------------------------------------------- */

static int64_t pf_now(void * ctx) {
    (void) ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pf_sleep(void * ctx, int ms) {
    (void) ctx;
    blh_sleep_ms(ms);
}

extern char ** environ;

static long pf_spawn(void * ctx, const char * path, const char * cwd, const char * log_path, const char * slot,
                     const char * notice) {
    (void) ctx;
    /* Everything the child needs is prepared here: the session hooks run
     * threads, so between fork and exec only async-signal-safe calls. */
    static char slot_env[64], notice_env[96];
    static char * envp[256];
    int n = 0;
    for (char ** e = environ; e && *e && n < 250; e++)
        if (strncmp(*e, "MELLOWTONE_SLOT=", 16) != 0 && strncmp(*e, "MELLOWTONE_NOTICE=", 18) != 0) envp[n++] = *e;
    snprintf(slot_env, sizeof(slot_env), "MELLOWTONE_SLOT=%s", slot);
    envp[n++] = slot_env;
    if (notice) {
        snprintf(notice_env, sizeof(notice_env), "MELLOWTONE_NOTICE=%s", notice);
        envp[n++] = notice_env;
    }
    envp[n] = NULL;
    char * const argv[] = { (char *) path, NULL };
    int log_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        /* The player goes when the launcher goes: the shim then starts the
         * factory player, and two players must never share the hardware. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(127);
        signal(SIGPIPE, SIG_DFL);
        if (cwd && chdir(cwd) != 0 && chdir("/") != 0) _exit(127);
        if (null_fd >= 0) dup2(null_fd, STDIN_FILENO);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        execve(path, argv, envp);
        _exit(127);
    }
    if (log_fd >= 0) close(log_fd);
    if (null_fd >= 0) close(null_fd);
    return pid < 0 ? -1 : pid;
}

static bool pf_wait(void * ctx, long handle, int timeout_ms, int * status) {
    (void) ctx;
    int64_t end = timeout_ms < 0 ? -1 : pf_now(NULL) + timeout_ms;
    for (;;) {
        pid_t r = waitpid((pid_t) handle, status, end < 0 ? 0 : WNOHANG);
        if (r == (pid_t) handle) return true;
        if (r < 0 && errno != EINTR) {
            *status = 0x7f00; /* lost: report as exit 127 */
            return true;
        }
        if (end >= 0) {
            if (pf_now(NULL) >= end) return false;
            blh_sleep_ms(250);
        }
    }
}

static bool pf_enter_recovery(void * ctx) {
    (void) ctx;
    return ops && ops->enter_recovery && ops->enter_recovery(&BOARD, log_fn);
}

static bool pf_enable_adb(void * ctx) {
    (void) ctx;
    return ops && ops->enable_adb && ops->enable_adb(&BOARD, log_fn);
}

static int pf_battery(void * ctx) {
    (void) ctx;
    char path[160], buf[16];
    if (BOARD.power.battery_node) {
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", BOARD.power.battery_node);
        if (blh_read(path, buf, sizeof(buf))[0]) return atoi(buf);
    }
    return -1;
}

static lx_key_t pf_next_key(void * ctx, int timeout_ms, int * tap_y) {
    (void) ctx;
    return lx_input_next(timeout_ms, tap_y);
}

static const char * button_names[16];

static lx_fb_t * pf_display_open(void * ctx) {
    (void) ctx;
    lx_fb_t * fb = lx_fb_open_dev("/dev/fb0", BOARD.screen.w, BOARD.screen.h, BOARD.screen.rotation,
                                  ops && ops->fb_slcd);
    if (fb) lx_input_open(button_names, board_has(CAP_TOUCH) ? BOARD.input.touch : NULL, BOARD.screen.h);
    return fb;
}

static void pf_display_close(void * ctx, lx_fb_t * fb) {
    (void) ctx;
    lx_input_close();
    lx_fb_close(fb);
}

/* ---- boot helpers ----------------------------------------------------------------- */

static bool is_mount_point(const char * dir) {
    char parent[300];
    snprintf(parent, sizeof(parent), "%s/..", dir);
    struct stat a, b;
    return stat(dir, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

/* Clock from NTP once Wi-Fi has a default route, saved to the RTC (UTC):
 * the shell launcher's ntp_sync, in a child process. */
static void ntp_start(const char * iface) {
    /* Double fork: the worker is reparented to init and reaped there. */
    pid_t pid = fork();
    if (pid > 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return;
    }
    if (pid < 0) return;
    if (fork() != 0) _exit(0);
    setsid();
    char want[32];
    snprintf(want, sizeof(want), "%s\t00000000", iface);
    int i;
    for (i = 0; i < 60; i++) {
        char route[4096];
        if (lx_read_file("/proc/net/route", route, sizeof(route)) && strstr(route, want)) break;
        blh_sleep_ms(5000);
    }
    if (i >= 60) {
        lx_log("ntp: no network, clock not set");
        _exit(0);
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        time_t before = time(NULL);
        char * argv[] = { (char *) "ntpd", (char *) "-n", (char *) "-q", (char *) "-p", (char *) "0.north-america.pool.ntp.org",
                          (char *) "-p", (char *) "1.north-america.pool.ntp.org", (char *) "-p",
                          (char *) "2.north-america.pool.ntp.org", (char *) "-p", (char *) "time.nist.gov", NULL };
        if (blh_run(argv, 30000) == 0) {
            char * hw[] = { (char *) "hwclock", (char *) "-w", (char *) "-u", NULL };
            blh_run(hw, 10000);
            lx_log("ntp: clock set (moved %lds), saved to RTC", (long) (time(NULL) - before));
            _exit(0);
        }
        blh_sleep_ms(20000);
    }
    lx_log("ntp: no time server answered");
    _exit(0);
}

static void rotate(const char * path, long max) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= max) return;
    char old[PATH_MAX];
    snprintf(old, sizeof(old), "%s.old", path);
    rename(path, old);
}

static void finish(lx_end_t end) {
    bool via_init = BOARD.system.reboot_via_init;
    switch (end) {
    case LX_END_HALT:
        if (via_init) {
            /* The player ran poweroff/reboot through init, which is shutting
             * everything down now. */
            lx_log("end: system shutting down");
            blh_sleep_ms(120000);
            exit(0);
        }
        /* X1600: a clean player exit asks for power off (the old bootloader). */
        lx_log("end: power off");
        if (ops && ops->release_sd) ops->release_sd(&BOARD);
        reboot(RB_POWER_OFF);
        break;
    case LX_END_POWEROFF:
        lx_log("end: power off");
        sync();
        if (ops && ops->release_sd) ops->release_sd(&BOARD);
        if (via_init) execl("/sbin/poweroff", "poweroff", (char *) NULL);
        reboot(RB_POWER_OFF);
        break;
    case LX_END_REBOOT:
        lx_log("end: reboot");
        sync();
        if (ops && ops->release_sd) ops->release_sd(&BOARD);
        if (via_init) execl("/sbin/reboot", "reboot", (char *) NULL);
        reboot(RB_AUTOBOOT);
        break;
    }
    for (;;) pause();
}

int main(int argc, char ** argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("mellowtone-launcher %s (%s)\n", LX_BUILD_STAMP, BOARD.name);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    ops = BOARD.launcher;
    if (!ops) {
        fprintf(stderr, "mellowtone-launcher: no launcher ops for %s\n", BOARD.name);
        return 2;
    }
    lx_log_set_file("/tmp/mellowtone-launcher.log");
    lx_log("start %s on %s", LX_BUILD_STAMP, BOARD.name);

    lx_fb_t * splash = NULL;
    /* An install-root path moves to the image root on an image install; a
     * missing file leaves whatever is on screen (the stock boot logo). */
    const char * splash_jpeg = board_asset_path(ops->splash_jpeg);
    if (splash_jpeg && access(splash_jpeg, R_OK) == 0) {
        splash = lx_fb_open_dev("/dev/fb0", BOARD.screen.w, BOARD.screen.h, BOARD.screen.rotation, ops->fb_slcd);
        if (splash) {
            if (!lx_fb_jpeg(splash, splash_jpeg)) lx_fb_fill(splash, 0x121212);
            lx_fb_flush(splash);
        }
    }
    if (ops->early) ops->early(&BOARD, log_fn);

    /* The card: mounted by the system (R3 Pro) or by early (X1600). Logs go
     * there, never to flash; without a card, to RAM for this boot. */
    for (int i = 0; i < 10 && !is_mount_point(BOARD.paths.sd_root); i++) blh_sleep_ms(1000);
    bool card = is_mount_point(BOARD.paths.sd_root);
    char state[256], logs[300], debug_flag[320], boot_log[340];
    snprintf(state, sizeof(state), "%s/.mellowtone", BOARD.paths.sd_root);
    snprintf(logs, sizeof(logs), "%s/logs", state);
    snprintf(debug_flag, sizeof(debug_flag), "%s/config/debug_logging", state);
    if (!card || access(BOARD.paths.sd_root, W_OK) != 0 || !lx_mkdir_p(logs)) snprintf(logs, sizeof(logs), "/tmp");
    snprintf(boot_log, sizeof(boot_log), "%s/boot.log", logs);
    rotate(boot_log, 64 * 1024);
    lx_log_set_file(boot_log);
    lx_log("boot: %s %s, card %s, logs in %s", BOARD.model, LX_BUILD_STAMP, card ? "mounted" : "missing", logs);

    /* Forks before the session threads start. */
    if (ops->ntp && board_has(CAP_WIFI)) ntp_start(BOARD.wifi.iface ? BOARD.wifi.iface : "wlan0");
    if (ops->session) ops->session(&BOARD, log_fn, debug_flag);

    int nb = 0;
    for (const board_input_device_t * d = BOARD.input.buttons; d && d->name && nb < 15; d++) button_names[nb++] = d->name;
    button_names[nb] = NULL;
    bool held = BOARD.input.menu_key &&
                lx_input_key_held(button_names, BOARD.input.menu_key, 1000);
    if (held) lx_log("boot: menu key held");
    if (splash) lx_fb_close(splash);

    char factory[300] = "";
    if (ops->factory_dir) snprintf(factory, sizeof(factory), "%s/%s", ops->factory_dir, LX_PLAYER_EXE);
    lx_config_t cfg = {
        .board = BOARD.name,
        .model = BOARD.model,
        .slots_root = ops->slots_root,
        .boot_dir = ops->boot_dir,
        .sd_root = BOARD.paths.sd_root,
        .install_root = board_install_root(), /* the player's working directory */
        .factory_player = factory[0] ? factory : NULL,
        .stock_player = ops->stock_player,
        .adb_flag = BOARD.paths.adb_flag,
        .log_dir = logs,
        .redeploy_marker = REDEPLOY_MARKER,
        .fw_names = ops->firmware_images,
        .fw_any_upt = ops->firmware_any_upt,
        .fw_format = (int) ops->firmware_format,
        .fw_product = ops->firmware_product,
        .launcher_version = LX_BUILD_STAMP,
    };
    lx_platform_t pf = {
        .ctx = NULL,
        .now_ms = pf_now,
        .sleep_ms = pf_sleep,
        .spawn = pf_spawn,
        .wait = pf_wait,
        .enter_recovery = pf_enter_recovery,
        .enable_adb = pf_enable_adb,
        .battery_percent = pf_battery,
        .next_key = pf_next_key,
        .display_open = pf_display_open,
        .display_close = pf_display_close,
    };
    lx_t lx = { .cfg = &cfg, .pf = &pf, .menu_idle_ms = 10 * 60 * 1000 };
    finish(lx_run(&lx, held));
    return 0;
}
