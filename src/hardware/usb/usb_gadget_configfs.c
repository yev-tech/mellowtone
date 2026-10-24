/* configfs USB gadget back end (X1600: R1, R3 Pro II, R3 II 2025), moved
 * unchanged from usb_mode_control.c except that every /sys path goes
 * through the sysfs root and every command through usb_run() (usb_gadget.h),
 * so the host tests can drive it against a fake configfs tree. */
#include "usb_gadget.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SETTLE_RETRY_LIMIT 20 /* ~1s at 50ms between checks */

/* Storage and DAC both live under this same configfs gadget name (confirmed
 * by reading usb_dev_mass_storage.sh and uac_device_config.sh directly off
 * the device) -- ADB uses a separate "adb_demo" directory. Relative to the
 * sysfs root. */
#define GADGETS_REL "kernel/config/usb_gadget"
#define ANDROID0_REL GADGETS_REL "/android0"
#define ADB_DEMO_REL GADGETS_REL "/adb_demo"
/* Charge only: the player's own gadget, so a computer can configure the
 * port (and be told from a wall charger) while it gets nothing: its one
 * function is a mass-storage LUN that never holds a medium. No stock
 * script knows it; configfs_apply() builds and removes it. */
#define CHARGE_REL GADGETS_REL "/mt_charge"
/* The Linux Foundation's composite-gadget ids: no vendor driver claims
 * them, the computer just sees a card reader with no card. */
#define CHARGE_ID_VENDOR "0x1d6b"
#define CHARGE_ID_PRODUCT "0x0104"

/* Matches subprocess_run()'s own default -- not reused directly since
 * SUBPROCESS_TIMEOUT_MS is private to subprocess.c and there's no public
 * "run with the default timeout, but checked" entry point. */
#define SCRIPT_TIMEOUT_MS 15000

typedef struct {
    char dir[PATH_MAX];
    char udc[PATH_MAX + 8];
} gadget_paths_t;

static gadget_paths_t gadget(const char * rel) {
    gadget_paths_t g;
    if (!usb_sysfs_path(rel, g.dir, sizeof(g.dir))) g.dir[0] = '\0';
    snprintf(g.udc, sizeof(g.udc), "%s/UDC", g.dir);
    return g;
}

static bool path_exists(const char * path) {
    return access(path, F_OK) == 0;
}

/* A gadget's UDC sysfs file holds the bound UDC's name (e.g.
 * "13500000.otg_new") once `echo $UDC > UDC` has actually taken, empty
 * otherwise -- the one place in configfs that says "the kernel accepted
 * this gadget's descriptors and it's live", as opposed to "the script that
 * was supposed to set it up merely ran to completion". */
static bool udc_is_bound(const char * udc_path) {
    FILE * f = fopen(udc_path, "r");
    if (!f) return false;
    char buf[64] = { 0 };
    bool read_ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!read_ok) return false;

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = '\0';
    if (len == 0 || strcmp(buf, "none") == 0) return false;
    return true;
}

/* Polls udc_is_bound() for up to ~1s instead of checking exactly once --
 * `echo $UDC > UDC` returning to the shell script doesn't necessarily mean
 * the kernel has fully finished processing the bind by the time this
 * process's subprocess_run_checked() call returns and this gets checked;
 * a real device switch showed a start script exit 0 with a function
 * correctly linked but UDC reading back empty on the first immediate
 * check, reported as a spurious "can't switch" failure. */
static bool udc_becomes_bound(const char * udc_path) {
    for (int i = 0; i < SETTLE_RETRY_LIMIT; i++) {
        if (udc_is_bound(udc_path)) return true;
        usleep(50000);
    }
    return false;
}

/* True if some function link under <gadget_dir>/configs/c.1 starts with
 * `name_prefix` (e.g. "uac_sa." or "mass_storage.") -- checking for a
 * SPECIFIC expected function, not just "is anything bound", is what catches
 * the case a real device hit: android0's UDC bound but to a mass_storage.N
 * function left over from a different mode, while this app believed (and
 * told the user) DAC had succeeded. */
static bool function_linked(const char * gadget_dir, const char * name_prefix) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return false;

    bool found = false;
    size_t prefix_len = strlen(name_prefix);
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, name_prefix, prefix_len) == 0) { found = true; break; }
    }
    closedir(dir);
    return found;
}

/* Generic teardown of a configfs gadget directory -- doesn't
 * assume any particular function name is inside, unlike the stock stop
 * scripts (uac_device_config.sh's uac_stop(), usb_dev_mass_storage.sh's
 * storage_stop()), which each hardcode exactly one function name and
 * silently fail to finish (their targeted rm/rmdir just no-op on a path
 * that doesn't exist, and the final rmdir on the gadget dir itself then
 * fails since it's non-empty) if a MISMATCHED function from a different
 * mode's incomplete previous teardown is what's actually there. Confirmed
 * on a real device: this is exactly how android0 ended up permanently
 * stuck with a stale mass_storage.0 that DAC's own stop script couldn't
 * remove, silently corrupting every subsequent switch attempt. Called
 * after the stock stop scripts as a guarantee, not a replacement for
 * them -- they still do the real process-killing side effects (killall
 * adbd/adbserver.sh) this doesn't attempt to duplicate. */
static void write_udc(const char * gadget_dir, const char * value) {
    char path[PATH_MAX + 8];
    snprintf(path, sizeof(path), "%s/UDC", gadget_dir);
    FILE * udc_f = fopen(path, "w");
    if (udc_f) {
        fputs(value, udc_f);
        fclose(udc_f);
    }
}

/* Empty lun.0/file is the gadget-side eject. A host that still has MUSIC
 * mounted can make the first write fail; unbind UDC (below) then retry.
 * forced_eject is optional on this kernel -- ignore if the attr is absent. */
static void eject_mass_storage_luns(const char * gadget_dir) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "mass_storage.", 13) != 0) continue;
        char attr[PATH_MAX + 300];
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/forced_eject", gadget_dir, entry->d_name);
        FILE * ej = fopen(attr, "w");
        if (ej) { fputs("1\n", ej); fclose(ej); }
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
        FILE * lun_f = fopen(attr, "w");
        if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
    }
    closedir(dir);
}

static bool rmdir_mass_storage_functions(const char * gadget_dir) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    bool leftover = false;
    DIR * dir = opendir(path);
    if (!dir) return true;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char func_path[PATH_MAX + 300];
        snprintf(func_path, sizeof(func_path), "%s/functions/%s", gadget_dir, entry->d_name);
        if (strncmp(entry->d_name, "mass_storage.", 13) == 0) {
            char lun_path[PATH_MAX + 320];
            snprintf(lun_path, sizeof(lun_path), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
            FILE * lun_f = fopen(lun_path, "w");
            if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
        }
        if (rmdir(func_path) != 0 && path_exists(func_path)) {
            fprintf(stderr, "usb_mode_control: rmdir %s failed (%s)\n", func_path, strerror(errno));
            leftover = true;
        }
    }
    closedir(dir);
    return !leftover;
}

static bool force_clean_gadget_dir(const char * gadget_dir) {
    char path[PATH_MAX + 32];

    /* Eject the LUN while still bound so a PC sees media-removed rather
     * than a surprise disconnect, then unbind. Stock storage_stop() does
     * the opposite and, if rmdir is EBUSY, re-binds UDC -- which is why
     * ADB/DAC only worked after the SD card was physically removed. */
    eject_mass_storage_luns(gadget_dir);
    write_udc(gadget_dir, "\n");
    write_udc(gadget_dir, "none\n");
    usleep(200000);
    eject_mass_storage_luns(gadget_dir);

    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    DIR * cfg = opendir(path);
    if (cfg) {
        struct dirent * entry;
        while ((entry = readdir(cfg)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char link_path[PATH_MAX + 300];
            snprintf(link_path, sizeof(link_path), "%s/configs/c.1/%s", gadget_dir, entry->d_name);
            unlink(link_path);
        }
        closedir(cfg);
    }

    for (int i = 0; i < 10; i++) {
        if (rmdir_mass_storage_functions(gadget_dir)) break;
        usleep(200000);
        eject_mass_storage_luns(gadget_dir);
    }

    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/strings/0x409", gadget_dir);
    rmdir(path);
    rmdir(gadget_dir);
    return !path_exists(gadget_dir);
}

static bool configfs_has_gadgets(void) {
    char path[PATH_MAX];
    if (!usb_sysfs_path(GADGETS_REL, path, sizeof(path))) return false;
    DIR * dir = opendir(path);
    if (!dir) return false;
    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') { found = true; break; }
    }
    closedir(dir);
    return found;
}

/* ADB and DAC start scripts require configfs to be unmounted before running.
 * Unmount configfs if no active gadgets remain. */
static bool unmount_empty_configfs(void) {
    char gadgets[PATH_MAX], config[PATH_MAX];
    if (!usb_sysfs_path(GADGETS_REL, gadgets, sizeof(gadgets)) ||
        !usb_sysfs_path("kernel/config", config, sizeof(config)))
        return false;
    if (!path_exists(gadgets)) return true;
    if (configfs_has_gadgets()) return false;
    char * argv[] = { (char *) "/bin/umount", config, NULL };
    usb_run(argv, 3000);
    if (path_exists(gadgets)) {
        char * lazy[] = { (char *) "/bin/umount", (char *) "-l", config, NULL };
        usb_run(lazy, 3000);
    }
    return !path_exists(gadgets);
}

static bool wait_path(const char * path, int attempts) {
    for (int i = 0; i < attempts; i++) {
        if (path_exists(path)) return true;
        usleep(50000);
    }
    return false;
}

static bool start_adb_gadget(void) {
    gadget_paths_t adb = gadget(ADB_DEMO_REL);
    char * argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
    int exit_code = usb_run(argv, SCRIPT_TIMEOUT_MS);
    if (exit_code != 0) {
        fprintf(stderr, "usb_mode_control: S440adb start exit=%d\n", exit_code);
        return false;
    }
    /* S440adb backgrounds adbserver.sh, which mounts functionfs then starts
     * adbd. The UDC bind is a separate stock script and must wait until
     * that mount exists -- otherwise udc_becomes_bound() times out and the
     * UI reports "Failed to switch to ADB" even though setup is in flight. */
    wait_path("/dev/usb-ffs/adb", SETTLE_RETRY_LIMIT);
    char * bind_argv[] = { (char *) "/sbin/usb_adb_enable.sh", NULL };
    exit_code = usb_run(bind_argv, 5000);
    bool ok = udc_becomes_bound(adb.udc) && function_linked(adb.dir, "ffs.");
    fprintf(stderr, "usb_mode_control: ADB bind exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
    return ok;
}

/* mkdir that is satisfied by a directory already there (configfs creates
 * a gadget's and a function's subdirectories itself). */
static bool make_dir(const char * path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool write_value(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "usb_mode_control: open %s: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fputs(value, f) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* The first (on these boards the only) device controller. */
static bool first_udc(char * out, size_t out_size) {
    char path[PATH_MAX];
    if (!usb_sysfs_path("class/udc", path, sizeof(path))) return false;
    DIR * dir = opendir(path);
    if (!dir) return false;
    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        found = snprintf(out, out_size, "%s", entry->d_name) < (int) out_size;
        break;
    }
    closedir(dir);
    return found;
}

/* configfs is mounted by the stock start scripts and unmounted again before
 * ADB/DAC; the charge-only gadget may come first. */
static bool mount_configfs(void) {
    char gadgets[PATH_MAX], config[PATH_MAX];
    if (!usb_sysfs_path(GADGETS_REL, gadgets, sizeof(gadgets)) ||
        !usb_sysfs_path("kernel/config", config, sizeof(config)))
        return false;
    if (path_exists(gadgets)) return true;
    char * argv[] = { (char *) "/bin/mount", (char *) "-t", (char *) "configfs", (char *) "none", config, NULL };
    int exit_code = usb_run(argv, 3000);
    if (!path_exists(gadgets)) {
        fprintf(stderr, "usb_mode_control: mounting configfs failed (exit %d)\n", exit_code);
        return false;
    }
    return true;
}

static bool start_charge_gadget(void) {
    gadget_paths_t g = gadget(CHARGE_REL);
    if (!g.dir[0] || !mount_configfs()) return false;
    char udc[NAME_MAX + 1];
    if (!first_udc(udc, sizeof(udc))) {
        fprintf(stderr, "usb_mode_control: no USB device controller for charge only\n");
        return false;
    }
    char path[PATH_MAX + 64], target[PATH_MAX + 64];
    bool ok = make_dir(g.dir);
#define CHARGE_PATH(rel) (snprintf(path, sizeof(path), "%s/%s", g.dir, rel), path)
    ok = ok && write_value(CHARGE_PATH("idVendor"), CHARGE_ID_VENDOR);
    ok = ok && write_value(CHARGE_PATH("idProduct"), CHARGE_ID_PRODUCT);
    ok = ok && make_dir(CHARGE_PATH("strings")) && make_dir(CHARGE_PATH("strings/0x409"));
    if (ok) {
        write_value(CHARGE_PATH("strings/0x409/manufacturer"), "HiBy");
        write_value(CHARGE_PATH("strings/0x409/product"), usb_board()->model ? usb_board()->model : "HiBy");
    }
    ok = ok && make_dir(CHARGE_PATH("functions")) && make_dir(CHARGE_PATH("functions/mass_storage.0")) &&
         make_dir(CHARGE_PATH("functions/mass_storage.0/lun.0"));
    /* Removable with no medium: nothing to mount, nothing to show. */
    if (ok) write_value(CHARGE_PATH("functions/mass_storage.0/lun.0/removable"), "1");
    ok = ok && make_dir(CHARGE_PATH("configs")) && make_dir(CHARGE_PATH("configs/c.1")) &&
         make_dir(CHARGE_PATH("configs/c.1/strings")) && make_dir(CHARGE_PATH("configs/c.1/strings/0x409"));
    if (ok) {
        write_value(CHARGE_PATH("configs/c.1/strings/0x409/configuration"), "Charge only");
        write_value(CHARGE_PATH("configs/c.1/MaxPower"), "500");
        snprintf(target, sizeof(target), "%s/functions/mass_storage.0", g.dir);
        ok = (symlink(target, CHARGE_PATH("configs/c.1/mass_storage.0")) == 0 || errno == EEXIST);
    }
#undef CHARGE_PATH
    if (ok) {
        char value[NAME_MAX + 2];
        snprintf(value, sizeof(value), "%s\n", udc);
        write_udc(g.dir, value);
    }
    ok = ok && udc_becomes_bound(g.udc) && function_linked(g.dir, "mass_storage.");
    fprintf(stderr, "usb_mode_control: charge only on %s -> %s\n", udc, ok ? "ok" : "FAILED");
    return ok;
}

static bool configfs_apply(usb_mode_t mode) {
    gadget_paths_t android0 = gadget(ANDROID0_REL);
    gadget_paths_t adb = gadget(ADB_DEMO_REL);
    gadget_paths_t charge = gadget(CHARGE_REL);
    fprintf(stderr, "usb_mode_control: switching to mode=%d\n", (int) mode);

    /* Deliberately does NOT call uac_device_config.sh's own "stop" --
     * confirmed on a real device that uac_stop()'s very first line (`echo 0
     * > /sys/class/usb_gadget/android0/soft_disconnect`, a stale reference
     * to the legacy sysfs class path rather than the configfs one this
     * script otherwise uses throughout) blocks indefinitely on this
     * device/kernel. Every call ate the full subprocess timeout and never
     * reached its actual cleanup code, which is how android0 ended up
     * permanently stuck with stale leftover functions poisoning every
     * later switch. force_clean_gadget_dir() below (which only touches the
     * safe configfs paths, confirmed working directly against this exact
     * leftover state) handles android0 teardown unconditionally instead,
     * for both DAC and Storage. */

    char * adb_stop_argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "stop", NULL };
    usb_run(adb_stop_argv, SCRIPT_TIMEOUT_MS);

    /* Do not call usb_dev_mass_storage.sh stop. If rmdir of mass_storage.0
     * fails because the host still has the volume mounted, that script
     * re-binds UDC and returns 0 -- Storage stays up and every later
     * ADB/DAC start refuses. Eject + force_clean below never re-bind. */
    sync();

    /* Guarantee a clean slate regardless of whether the stock stop scripts
     * actually finished -- see force_clean_gadget_dir()'s own comment. */
    if (path_exists(android0.dir)) {
        fprintf(stderr, "usb_mode_control: android0 survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(android0.dir); i++) {
            if (force_clean_gadget_dir(android0.dir)) break;
            usleep(200000);
        }
    }
    if (path_exists(adb.dir)) {
        fprintf(stderr, "usb_mode_control: adb_demo survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(adb.dir); i++) {
            if (force_clean_gadget_dir(adb.dir)) break;
            usleep(200000);
        }
    }
    if (path_exists(charge.dir)) {
        for (int i = 0; i < 10 && path_exists(charge.dir); i++) {
            if (force_clean_gadget_dir(charge.dir)) break;
            usleep(200000);
        }
    }
    /* A leftover unbound gadget dir is not fatal: Storage reuses android0,
     * ADB lives under a different name, and the DAC start script now drops
     * an unbound android0 itself. Only a still-bound UDC blocks the switch. */
    if (udc_is_bound(android0.udc) || udc_is_bound(adb.udc) || udc_is_bound(charge.udc)) {
        fprintf(stderr, "usb_mode_control: UDC still bound after teardown; refusing to start mode=%d\n", (int) mode);
        return false;
    }

    /* Prefer an unmounted configfs so stock start scripts are happy.
     * If umount fails on an empty leftover (common after Storage, or a
     * recovery flash with the cable still in), ADB/DAC start scripts now
     * tolerate that and mkdir their gadget on the existing mount. */
    if (mode != USB_MODE_STORAGE && mode != USB_MODE_CHARGE && !unmount_empty_configfs())
        fprintf(stderr, "usb_mode_control: configfs still mounted before mode=%d; continuing\n", (int) mode);

    int exit_code = -1;
    switch (mode) {
        case USB_MODE_CHARGE:
            return start_charge_gadget();
        case USB_MODE_STORAGE: {
            char * argv[] = { (char *) "/usr/bin/usb_dev_mass_storage.sh", (char *) "start", (char *) "/dev/mmcblk0", NULL };
            exit_code = usb_run(argv, SCRIPT_TIMEOUT_MS);
            bool ok = exit_code == 0 && udc_becomes_bound(android0.udc) && function_linked(android0.dir, "mass_storage.");
            fprintf(stderr, "usb_mode_control: Storage start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
        case USB_MODE_ADB:
            return start_adb_gadget();
        case USB_MODE_DAC: {
            /* No -v/-p/-m/-n overrides -- the script's own defaults
             * (VID 0x32BB/PID 0x0004, "Hiby"/"R1") already match what the
             * stock binary passes explicitly, confirmed by reading the
             * script directly off the device. */
            char * argv[] = { (char *) "/usr/bin/uac_device_config.sh", (char *) "start", NULL };
            exit_code = usb_run(argv, SCRIPT_TIMEOUT_MS);
            bool ok = exit_code == 0 && udc_becomes_bound(android0.udc) && function_linked(android0.dir, "uac_sa.");
            fprintf(stderr, "usb_mode_control: DAC start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
    }
    return false;
}

static bool configfs_detect(usb_mode_t * out_mode) {
    gadget_paths_t android0 = gadget(ANDROID0_REL);
    gadget_paths_t adb = gadget(ADB_DEMO_REL);
    gadget_paths_t charge = gadget(CHARGE_REL);
    if (udc_is_bound(charge.udc) && function_linked(charge.dir, "mass_storage.")) {
        *out_mode = USB_MODE_CHARGE;
        return true;
    }
    if (udc_is_bound(adb.udc) && function_linked(adb.dir, "ffs.")) {
        *out_mode = USB_MODE_ADB;
        return true;
    }
    if (udc_is_bound(android0.udc)) {
        if (function_linked(android0.dir, "uac_sa.")) {
            *out_mode = USB_MODE_DAC;
            return true;
        }
        if (function_linked(android0.dir, "mass_storage.")) {
            *out_mode = USB_MODE_STORAGE;
            return true;
        }
    }
    return false;
}

/* Read a complete bounded sysfs value, accepting its optional final newline.
 * Reject truncation, embedded NULs and read errors rather than matching a
 * valid-looking prefix of unavailable or malformed state. */
static bool read_storage_host_value(const char * path, char * out, size_t size) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    size_t len = fread(out, 1, size - 1, f);
    bool ok = !ferror(f) && feof(f) && memchr(out, '\0', len) == NULL;
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;
    if (len && out[len - 1] == '\n') --len;
    out[len] = '\0';
    return len != 0;
}

/* The controller `g` is bound to reports exactly "configured": a computer
 * configured the port. */
static bool gadget_host_configured(const gadget_paths_t * g) {
    char controller[258]; /* NAME_MAX controller plus optional newline and NUL. */
    if (!read_storage_host_value(g->udc, controller, sizeof(controller))) return false;
    size_t len = strlen(controller);
    if (len > 255 || strcmp(controller, "none") == 0 ||
        strcmp(controller, ".") == 0 || strcmp(controller, "..") == 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)controller[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }

    char rel[300], path[PATH_MAX];
    snprintf(rel, sizeof(rel), "class/udc/%s/state", controller);
    if (!usb_sysfs_path(rel, path, sizeof(path))) return false;
    char state[32];
    return read_storage_host_value(path, state, sizeof(state)) &&
           strcmp(state, "configured") == 0;
}

static bool configfs_storage_host_configured(void) {
    usb_mode_t mode;
    if (!configfs_detect(&mode) || mode != USB_MODE_STORAGE) return false;
    gadget_paths_t android0 = gadget(ANDROID0_REL);
    return gadget_host_configured(&android0);
}

/* Whichever gadget is live: a computer configured it. */
static bool configfs_host_connected(void) {
    usb_mode_t mode;
    if (!configfs_detect(&mode)) return false;
    gadget_paths_t g = gadget(mode == USB_MODE_ADB ? ADB_DEMO_REL : mode == USB_MODE_CHARGE ? CHARGE_REL : ANDROID0_REL);
    return gadget_host_configured(&g);
}

/* The storage gadget stays bound after the cable is pulled, its LUN still
 * holding the card: a computer plugged in again would reach it at once. */
static void configfs_eject_storage(void) {
    gadget_paths_t android0 = gadget(ANDROID0_REL);
    if (path_exists(android0.dir)) eject_mass_storage_luns(android0.dir);
}

const struct board_usb_gadget_ops usb_gadget_configfs_ops = {
    .name = "configfs",
    .apply = configfs_apply,
    .detect = configfs_detect,
    .storage_host_configured = configfs_storage_host_configured,
    .host_connected = configfs_host_connected, /* the card is shared (read-only meanwhile), never lent */
    .set_storage_function = NULL, /* apply(STORAGE) binds the storage gadget itself */
    .eject_storage = configfs_eject_storage,
};
