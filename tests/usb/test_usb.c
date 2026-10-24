/* USB modes tests, against fake trees in a temp dir:
 *
 *   android_usb   the R3 Pro gadget back end's write sequences (the stock
 *                 player's) for DAC, ADB, idle (charge only) and
 *                 the lent-card storage set-up; detect; host checks
 *   configfs      the X1600 back end against a fake configfs tree whose stock
 *                 scripts are simulated by the command runner: the commands
 *                 run per mode, the charge-only gadget (a LUN with no
 *                 medium) and its teardown, teardown of a leftover
 *                 function, detect, the host checks (a computer vs a charger)
 *   connection    the per-connection mode: charge only by default, the
 *                 prompt once per connection (not for a charger, not under
 *                 ADB), back to charge only after an unplug (debounced),
 *                 nothing kept from one connection to the next
 *   sys_server    storage_export_sys_server_ops through the real sys_client /
 *                 sys_server socket code against a fake sys_server (MOUNT:UMOUNT
 *                 / SETLUN / MOUNT with the stock's semantics, results sent
 *                 back to the client socket), host eject, no mount while the
 *                 LUN holds the card, no unmount of a busy card, the
 *                 busy-card scan (fds, cwd, memory maps)
 *   shared        storage_export_shared_ops: read-only remount while a
 *                 computer has the card, LUN eject and a plain (never lazy)
 *                 unmount after, refused while something holds the card
 *   lend          the pure state machine (every end, failures, retries, the
 *                 "declined until unplugged" latch), then the whole hand-over
 *                 through usb_storage_lend_poll(): closing hooks, the card
 *                 state layer (mt_state_card_lent()), export, eject, cable
 *                 pull (an unreadable power state is not one), remount and
 *                 reopen; the busy check naming another process, and
 *                 stdout moved to RAM while lent and back after
 *   uac_sa        GET_INFO parsing per ABI, prepare, DoP detection and the
 *                 native-DSD-to-DoP repacking */
#include "board.h"
#include "bluetooth_control.h"
#include "hiby_sys_client.h"
#include "power_events.h"
#include "hiby_sys_server.h"
#include "storage_paths.h"
#include "uac_sa.h"
#include "usb_connection.h"
#include "usb_gadget.h"
#include "usb_mode_control.h"
#include "usb_storage_lend.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* The app's BOARD; every test installs its own with usb_gadget_test_setup(). */
const board_desc_t BOARD = { .name = "test", .model = "test" };

/* ---- stubs for what the linked app code calls --------------------------- */

bool power_events_sys_message(const char * msg) {
    (void) msg;
    return false;
}
bool bt_control_get_connected_device_mac(char * out, size_t size) {
    (void) out;
    (void) size;
    return false;
}
static atomic_bool cable_in, cable_unknown;
bool usb_mode_control_cable_connected(void) { return atomic_load(&cable_in); }
bool usb_mode_control_cable_state(bool * connected) {
    if (atomic_load(&cable_unknown)) return false;
    *connected = atomic_load(&cable_in);
    return true;
}

/* ---- temp tree helpers -------------------------------------------------- */

static char tmp[PATH_MAX];
static char sys_root[PATH_MAX + 8], proc_root[PATH_MAX + 8];

static void sleep_ms(int ms) { usleep((useconds_t) ms * 1000); }

static void mkdirs(const char * path) {
    char buf[PATH_MAX * 2];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void write_file(const char * path, const char * fmt, ...) {
    char dir[PATH_MAX * 2];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdirs(dir);
    }
    FILE * f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void read_file(const char * path, char * out, size_t size) {
    out[0] = '\0';
    FILE * f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
}

static void sys_put(const char * rel, const char * value) {
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/%s", sys_root, rel);
    write_file(path, "%s", value);
}

static void sys_get(const char * rel, char * out, size_t size) {
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/%s", sys_root, rel);
    read_file(path, out, size);
}

static bool sys_is(const char * rel, const char * want) {
    char have[256];
    sys_get(rel, have, sizeof(have));
    return strcmp(have, want) == 0;
}

static void rm_rf(const char * path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR * d = opendir(path);
        struct dirent * e;
        while (d && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char sub[PATH_MAX * 2];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            rm_rf(sub);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void fresh_tree(void) {
    rm_rf(sys_root);
    rm_rf(proc_root);
    mkdirs(sys_root);
    mkdirs(proc_root);
}

/* ---- the write trace and the command runner ------------------------------ */

static char trace[16384];
static void trace_write(const char * rel, const char * value) {
    size_t n = strlen(trace);
    snprintf(trace + n, sizeof(trace) - n, "%s=%s\n", rel, value);
}

static char commands[4096];
static int (*run_effect)(char * const argv[]);

static int fake_run(char * const argv[], int timeout_ms) {
    (void) timeout_ms;
    size_t n = strlen(commands);
    for (int i = 0; argv[i]; i++) {
        snprintf(commands + n, sizeof(commands) - n, "%s%s", i ? " " : "", argv[i]);
        n = strlen(commands);
    }
    snprintf(commands + n, sizeof(commands) - n, "\n");
    return run_effect ? run_effect(argv) : 0;
}

static void reset_logs(void) {
    trace[0] = '\0';
    commands[0] = '\0';
}

/* ---- android_usb (R3 Pro) -------------------------------------------------- */

#define A0 "class/android_usb/android0"

static const board_android_usb_t fake_android_usb = {
    .gadget_dir = A0,
    .dac = { "32BB", "0004", "239", "2", "1", "uac_sa,hid" },
    .storage = { "32BB", "0101", "0", "0", "0", "mass_storage" },
    .idle = { "18d1", "d002", "0", "0", "0", "adb" },
    .manufacturer = "HiBy", .product = "R3PRO", .serial = "HiBy R3PRO",
    .uac_string_assoc = "class/android_usb/f_uac_sa/string_assoc",
    .uac_string_assoc_value = "HiBy R3PRO",
    .inquiry_vendor = "HiBy", .inquiry_product = "R3PRO U-DISK",
};

static char adb_flag[PATH_MAX + 16], sd_root[PATH_MAX + 16], lun_file[PATH_MAX + 16];
static char disk_dev[PATH_MAX + 16], part_dev[PATH_MAX + 16];
static board_desc_t r3pro_board;

/* K90adb, as the stock script: enable 0, ids, functions adb, enable 1. */
static int k90adb_effect(char * const argv[]) {
    if (strstr(argv[0], "K90adb") && argv[1] && strcmp(argv[1], "start") == 0) {
        sys_put(A0 "/enable", "0");
        sys_put(A0 "/idVendor", "18d1");
        sys_put(A0 "/idProduct", "d002");
        sys_put(A0 "/functions", "adb");
        sys_put(A0 "/enable", "1");
    }
    return 0;
}

static void android_tree(void) {
    fresh_tree();
    const char * attrs[] = { "enable", "idVendor", "idProduct", "bDeviceClass", "bDeviceSubClass",
                             "bDeviceProtocol", "iManufacturer", "iProduct", "iSerial",
                             "f_mass_storage/inquiry_string", NULL };
    for (int i = 0; attrs[i]; i++) {
        char rel[128];
        snprintf(rel, sizeof(rel), A0 "/%s", attrs[i]);
        sys_put(rel, "");
    }
    /* What K90adb left at boot. */
    sys_put(A0 "/enable", "1");
    sys_put(A0 "/idVendor", "18d1");
    sys_put(A0 "/idProduct", "d002");
    sys_put(A0 "/bDeviceClass", "0");
    sys_put(A0 "/functions", "adb\n");
    sys_put(A0 "/state", "CONFIGURED\n");
    sys_put("class/android_usb/f_uac_sa/string_assoc", "");
    sys_put("class/android_usb/f_uac_sa/dsd_native_enable", "off");
}

static void r3pro_setup(usb_sys_request_fn request) {
    r3pro_board = (board_desc_t) { .name = "r3pro-test" };
    r3pro_board.caps = CAP_USB_STORAGE | CAP_USB_DAC | CAP_USB_DAC_DSD;
    r3pro_board.paths.adb_flag = adb_flag;
    r3pro_board.paths.sd_root = sd_root;
    r3pro_board.system.hiby_sys_server = true;
    r3pro_board.usb_gadget = &usb_gadget_android_usb_ops;
    r3pro_board.storage_export = &storage_export_sys_server_ops;
    r3pro_board.usb.uac_abi = BOARD_UAC_SA_ABI_PREPARE_IOCTL;
    r3pro_board.usb.dsd_native_enable = "class/android_usb/f_uac_sa/dsd_native_enable";
    r3pro_board.usb.android_usb = &fake_android_usb;
    r3pro_board.usb.storage_lun_file = lun_file;
    r3pro_board.usb.storage_disk = disk_dev;
    r3pro_board.usb.storage_partition = part_dev;
    usb_gadget_test_setup(&r3pro_board, sys_root, proc_root, fake_run, request);
    run_effect = k90adb_effect;
}

static const char * const dac_sequence =
    "class/android_usb/f_uac_sa/dsd_native_enable=on\n"
    A0 "/enable=0\n"
    "class/android_usb/f_uac_sa/string_assoc=HiBy R3PRO\n"
    A0 "/iProduct=R3PRO\n"
    A0 "/iManufacturer=HiBy\n"
    A0 "/iSerial=HiBy R3PRO\n"
    A0 "/bDeviceClass=239\n"
    A0 "/bDeviceSubClass=2\n"
    A0 "/bDeviceProtocol=1\n"
    A0 "/idVendor=32BB\n"
    A0 "/idProduct=0004\n"
    A0 "/functions=uac_sa,hid\n"
    A0 "/enable=1\n";

static void test_android_usb(void) {
    android_tree();
    r3pro_setup(NULL);
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    CHECK(ops == &usb_gadget_android_usb_ops, "descriptor's gadget");

    /* Boot state: ADB off, idle in place: charge only, and a computer is
     * told from a charger by the idle set-up being configured. */
    usb_mode_t mode = USB_MODE_DAC;
    CHECK(ops->detect(&mode) && mode == USB_MODE_CHARGE, "idle = charge only (default) %d", mode);
    CHECK(ops->host_connected(), "a computer configured the idle set-up");
    reset_logs();
    CHECK(ops->apply(USB_MODE_CHARGE), "charge only (ADB off)");
    CHECK(strcmp(trace, "") == 0, "idle already in place: nothing written:\n%s", trace);
    CHECK(strcmp(commands, "/etc/init.d/K90adb stop\n") == 0, "K90adb stop: %s", commands);
    CHECK(!ops->storage_host_configured(), "charge only is not storage");
    reset_logs();
    CHECK(ops->apply(USB_MODE_STORAGE), "storage (ADB off)");
    CHECK(strcmp(trace, "") == 0, "storage: the idle set-up until the card is lent:\n%s", trace);
    CHECK(ops->detect(&mode) && mode == USB_MODE_CHARGE, "nothing lent yet: still idle %d", mode);

    /* DAC: the stock sequence. */
    write_file(adb_flag, "");
    reset_logs();
    CHECK(ops->apply(USB_MODE_DAC), "DAC");
    CHECK(strcmp(trace, dac_sequence) == 0, "DAC sequence:\n%s", trace);
    CHECK(access(adb_flag, F_OK) != 0, "ADB flag cleared");
    CHECK(strcmp(commands, "/etc/init.d/K90adb stop\n") == 0, "adbd stopped: %s", commands);
    CHECK(ops->detect(&mode) && mode == USB_MODE_DAC, "detect DAC");
    CHECK(!ops->storage_host_configured(), "DAC is not storage");
    CHECK(ops->host_connected(), "a host configured the port");

    /* Back to charge only: the idle set-up (class reset), no adbd. */
    reset_logs();
    CHECK(ops->apply(USB_MODE_CHARGE), "DAC -> charge only");
    CHECK(strstr(trace, A0 "/bDeviceClass=0\n") && strstr(trace, A0 "/functions=adb\n"), "idle again:\n%s", trace);
    CHECK(ops->detect(&mode) && mode == USB_MODE_CHARGE, "detect charge only");
    CHECK(ops->apply(USB_MODE_DAC), "DAC again");

    /* Back to ADB: class reset (K90adb does not), flag, K90adb. */
    reset_logs();
    CHECK(ops->apply(USB_MODE_ADB), "ADB");
    CHECK(strstr(trace, A0 "/bDeviceClass=0\n") && strstr(trace, A0 "/functions=adb\n"), "idle before K90adb:\n%s", trace);
    CHECK(strcmp(commands, "/etc/init.d/K90adb start\n") == 0, "K90adb start: %s", commands);
    CHECK(access(adb_flag, F_OK) == 0, "ADB flag set");
    CHECK(ops->detect(&mode) && mode == USB_MODE_ADB, "detect ADB");

    /* The lent card's set-up, with the INQUIRY string, and back. */
    reset_logs();
    CHECK(ops->set_storage_function(true), "storage function");
    CHECK(strstr(trace, A0 "/f_mass_storage/inquiry_string=HiBy    R3PRO U-DISK    0000\n") != NULL,
          "inquiry string:\n%s", trace);
    CHECK(strstr(trace, A0 "/idProduct=0101\n" A0 "/functions=mass_storage\n" A0 "/enable=1\n") != NULL,
          "storage ids and functions:\n%s", trace);
    CHECK(ops->storage_host_configured(), "storage host configured");
    CHECK(ops->detect(&mode) && mode == USB_MODE_STORAGE, "lent = storage");
    reset_logs();
    CHECK(ops->set_storage_function(false), "idle function");
    CHECK(sys_is(A0 "/functions", "adb") && sys_is(A0 "/idVendor", "18d1"), "idle again");

    /* A gadget that does not take the functions is a failure. */
    sys_put(A0 "/state", "DISCONNECTED\n");
    CHECK(!ops->host_connected(), "no host");
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/" A0 "/functions", sys_root);
    chmod(path, 0444);
    if (access(path, W_OK) != 0) CHECK(!ops->apply(USB_MODE_DAC), "unwritable functions fails");
    chmod(path, 0644);
}

/* ---- configfs (X1600) -------------------------------------------------------- */

#define CFG "kernel/config/usb_gadget"

static void cfg_gadget(const char * gadget, const char * function, const char * udc) {
    char path[PATH_MAX * 2], target[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/" CFG "/%s/functions/%s", sys_root, gadget, function);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/" CFG "/%s/configs/c.1", sys_root, gadget);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/" CFG "/%s/configs/c.1/%s", sys_root, gadget, function);
    snprintf(target, sizeof(target), "../../functions/%s", function);
    unlink(path);
    if (symlink(target, path) != 0) fprintf(stderr, "symlink %s: %s\n", path, strerror(errno));
    snprintf(path, sizeof(path), "%s/" CFG "/%s/UDC", sys_root, gadget);
    write_file(path, "%s\n", udc);
}

/* The stock scripts' effect on the tree. */
static int configfs_effect(char * const argv[]) {
    if (strstr(argv[0], "usb_dev_mass_storage.sh")) cfg_gadget("android0", "mass_storage.0", "13500000.otg_new");
    else if (strstr(argv[0], "uac_device_config.sh")) cfg_gadget("android0", "uac_sa.0", "13500000.otg_new");
    else if (strstr(argv[0], "S440adb") && strcmp(argv[1], "start") == 0) cfg_gadget("adb_demo", "ffs.adb", "");
    else if (strstr(argv[0], "usb_adb_enable.sh")) {
        char path[PATH_MAX * 2];
        snprintf(path, sizeof(path), "%s/" CFG "/adb_demo/UDC", sys_root);
        write_file(path, "13500000.otg_new\n");
    }
    return 0;
}

static void test_configfs(void) {
    fresh_tree();
    board_desc_t b = { .name = "x1600-test" };
    b.caps = CAP_USB_STORAGE | CAP_USB_DAC;
    b.usb_gadget = &usb_gadget_configfs_ops;
    b.storage_export = &storage_export_shared_ops;
    usb_gadget_test_setup(&b, sys_root, proc_root, fake_run, NULL);
    run_effect = configfs_effect;
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    mkdirs(sys_root);
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/" CFG, sys_root);
    mkdirs(path);

    usb_mode_t mode;
    CHECK(!ops->detect(&mode), "nothing bound");
    CHECK(!ops->host_connected(), "nothing bound: no computer");

    /* Charge only: the player's own gadget, a LUN with no medium, bound to
     * the controller; no stock script runs. */
    snprintf(path, sizeof(path), "%s/class/udc/13500000.otg_new", sys_root);
    mkdirs(path);
    sys_put("class/udc/13500000.otg_new/state", "not attached\n");
    reset_logs();
    CHECK(ops->apply(USB_MODE_CHARGE), "charge only");
    CHECK(strcmp(commands, "/etc/init.d/adb/S440adb stop\n") == 0, "charge only runs no start script:\n%s", commands);
    CHECK(ops->detect(&mode) && mode == USB_MODE_CHARGE, "detect charge only");
    CHECK(sys_is(CFG "/mt_charge/UDC", "13500000.otg_new\n"), "bound to the controller");
    char lun[PATH_MAX * 2];
    snprintf(lun, sizeof(lun), "%s/" CFG "/mt_charge/functions/mass_storage.0/lun.0/file", sys_root);
    CHECK(access(lun, F_OK) != 0, "the LUN never gets a medium");
    CHECK(sys_is(CFG "/mt_charge/functions/mass_storage.0/lun.0/removable", "1"), "removable LUN");
    snprintf(path, sizeof(path), "%s/" CFG "/mt_charge/configs/c.1/mass_storage.0", sys_root);
    CHECK(access(path, F_OK) == 0, "function linked");
    CHECK(!ops->storage_host_configured(), "charge only is not storage");
    CHECK(!ops->host_connected(), "a charger: not configured");
    sys_put("class/udc/13500000.otg_new/state", "powered\n");
    CHECK(!ops->host_connected(), "a charger: powered only");
    sys_put("class/udc/13500000.otg_new/state", "configured\n");
    CHECK(ops->host_connected(), "a computer configured the charge-only gadget");
    CHECK(!ops->storage_host_configured(), "still not storage");
    sys_put("class/udc/13500000.otg_new/state", "not attached\n");
    /* configfs drops a function's default groups with it; a plain directory
     * tree does not, so the fake LUN goes the way the kernel's would. */
    snprintf(path, sizeof(path), "%s/" CFG "/mt_charge/functions/mass_storage.0/lun.0", sys_root);
    rm_rf(path);

    reset_logs();
    CHECK(ops->apply(USB_MODE_STORAGE), "storage");
    CHECK(strcmp(commands, "/etc/init.d/adb/S440adb stop\n/usr/bin/usb_dev_mass_storage.sh start /dev/mmcblk0\n") == 0,
          "storage commands:\n%s", commands);
    CHECK(sys_is(CFG "/mt_charge/UDC", "none\n"), "charge-only gadget unbound");
    snprintf(path, sizeof(path), "%s/" CFG "/mt_charge/configs/c.1/mass_storage.0", sys_root);
    CHECK(access(path, F_OK) != 0, "charge-only link removed");
    snprintf(path, sizeof(path), "%s/" CFG "/mt_charge", sys_root);
    rm_rf(path); /* what is left of it, as configfs would have removed it */
    CHECK(ops->detect(&mode) && mode == USB_MODE_STORAGE, "detect storage");
    CHECK(!ops->storage_host_configured(), "not configured");
    sys_put("class/udc/13500000.otg_new/state", "configured\n");
    CHECK(ops->storage_host_configured(), "host configured");
    CHECK(ops->host_connected(), "a computer");
    CHECK(ops->set_storage_function == NULL, "shared card: no lending ops");

    /* DAC after storage: the leftover mass_storage link is torn down, the
     * UDC unbound, then the DAC script. */
    reset_logs();
    CHECK(ops->apply(USB_MODE_DAC), "DAC");
    CHECK(strstr(commands, "/usr/bin/uac_device_config.sh start\n") != NULL, "DAC script:\n%s", commands);
    snprintf(path, sizeof(path), "%s/" CFG "/android0/configs/c.1/mass_storage.0", sys_root);
    CHECK(access(path, F_OK) != 0, "storage link removed");
    CHECK(ops->detect(&mode) && mode == USB_MODE_DAC, "detect DAC");

    reset_logs();
    CHECK(ops->apply(USB_MODE_ADB), "ADB");
    CHECK(strstr(commands, "/etc/init.d/adb/S440adb start\n/sbin/usb_adb_enable.sh\n") != NULL, "ADB commands:\n%s",
          commands);
    CHECK(ops->detect(&mode) && mode == USB_MODE_ADB, "detect ADB");

    /* A start script that fails is reported. */
    run_effect = NULL;
    snprintf(path, sizeof(path), "%s/" CFG "/adb_demo/UDC", sys_root);
    write_file(path, "\n");
    snprintf(path, sizeof(path), "%s/" CFG "/android0/UDC", sys_root);
    write_file(path, "\n");
    CHECK(!ops->apply(USB_MODE_STORAGE), "no gadget bound: failure");
}

/* ---- the connection's mode (usb_connection.h) ------------------------------------- */

static usb_conn_event_t conn_step(usb_conn_t * c, bool cable, bool host, bool adb, uint32_t t) {
    usb_conn_inputs_t in = { .cable_known = true, .cable = cable, .host = host, .adb = adb,
                             .modes_offered = true, .mode = usb_mode_active() };
    return usb_conn_step(c, &in, t);
}

static void test_connection(void) {
    CHECK(usb_mode_active() == USB_MODE_CHARGE, "charge only at start-up");
    usb_conn_t c;
    usb_conn_init(&c);
    uint32_t t = 1000;

    /* A wall charger: power, never configured: nothing to ask, nothing to choose. */
    for (int i = 0; i < 5; i++) CHECK(conn_step(&c, true, false, false, t += 100) == USB_CONN_NOTHING, "charger");
    CHECK(usb_conn_cable(&c) && !usb_conn_computer(&c), "cable, no computer");

    /* A computer configures the port: the prompt, once. */
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_PROMPT, "computer: prompt");
    CHECK(usb_conn_computer(&c), "computer on the cable");
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_NOTHING, "prompt once");
    /* Dismissed (charge only), the host re-enumerating: still not again. */
    CHECK(conn_step(&c, true, false, false, t += 100) == USB_CONN_NOTHING, "re-enumeration");
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_NOTHING, "not again on this connection");

    /* Storage chosen; a glitch shorter than the debounce changes nothing. */
    usb_mode_set_active(USB_MODE_STORAGE);
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "glitch");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS - 200) == USB_CONN_NOTHING, "glitch");
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_NOTHING, "back: same connection, no prompt");
    CHECK(usb_mode_active() == USB_MODE_STORAGE, "mode kept over a glitch");
    /* An unreadable power state is not an unplug. */
    usb_conn_inputs_t unknown = { .cable_known = false, .mode = usb_mode_active() };
    for (int i = 0; i < 20; i++) CHECK(usb_conn_step(&c, &unknown, t += 100) == USB_CONN_NOTHING, "unknown");
    CHECK(usb_conn_computer(&c), "still the same connection");

    /* Unplugged: back to charge only, asked for until the switch starts. */
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "unplug seen");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_UNPLUGGED, "unplugged");
    CHECK(!usb_conn_cable(&c) && !usb_conn_computer(&c), "connection over");
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_UNPLUGGED, "again until started (switch busy)");
    usb_conn_revert_started(&c);
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "switch under way");
    usb_mode_set_active(USB_MODE_CHARGE); /* it landed */

    /* The next connection starts from scratch: prompt again. */
    CHECK(conn_step(&c, true, false, false, t += 100) == USB_CONN_NOTHING, "plugged in");
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_PROMPT, "new connection: prompt again");

    /* USB DAC ends the same way. */
    usb_mode_set_active(USB_MODE_DAC);
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "unplug seen");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_UNPLUGGED, "DAC unplugged");
    usb_conn_revert_started(&c);
    usb_mode_set_active(USB_MODE_CHARGE);

    /* Charge only unplugged: nothing to switch. */
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_PROMPT, "prompt");
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "unplug seen");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_NOTHING, "charge only: nothing");

    /* ADB owns the port: no prompt, and ADB outlives the unplug. */
    usb_mode_set_active(USB_MODE_ADB);
    CHECK(conn_step(&c, true, true, true, t += 100) == USB_CONN_NOTHING, "ADB: no prompt");
    CHECK(conn_step(&c, false, false, true, t += 100) == USB_CONN_NOTHING, "unplug seen");
    CHECK(conn_step(&c, false, false, true, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_NOTHING, "ADB stays");
    CHECK(usb_mode_active() == USB_MODE_ADB, "ADB kept");
    /* ADB turned off on a connection that started under it: not asked
     * then either (the computer was there before). */
    CHECK(conn_step(&c, true, true, true, t += 100) == USB_CONN_NOTHING, "ADB connection");
    usb_mode_set_active(USB_MODE_CHARGE);
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_NOTHING, "ADB off mid-connection: no prompt");

    /* A mode chosen before the computer showed up (no detection): no prompt. */
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "unplug seen");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_NOTHING, "unplugged");
    CHECK(conn_step(&c, true, false, false, t += 100) == USB_CONN_NOTHING, "cable");
    usb_mode_set_active(USB_MODE_STORAGE);
    CHECK(conn_step(&c, true, true, false, t += 100) == USB_CONN_NOTHING, "already chosen: no prompt");

    /* A board with nothing but charging: never asked. */
    usb_mode_set_active(USB_MODE_CHARGE);
    usb_conn_init(&c);
    usb_conn_inputs_t bare = { .cable_known = true, .cable = true, .host = true, .mode = USB_MODE_CHARGE };
    CHECK(usb_conn_step(&c, &bare, t += 100) == USB_CONN_NOTHING, "no modes: no prompt");

    /* A player restarted mid-session with no cable: the mode it found goes
     * back to charge only once the cable is known to be out. */
    usb_conn_init(&c);
    usb_mode_set_active(USB_MODE_DAC);
    CHECK(conn_step(&c, false, false, false, t += 100) == USB_CONN_NOTHING, "no cable seen");
    CHECK(conn_step(&c, false, false, false, t += USB_CONN_UNPLUG_DEBOUNCE_MS) == USB_CONN_UNPLUGGED,
          "restart without a cable: back to charge only");
    usb_mode_set_active(USB_MODE_CHARGE);
}

/* ---- fake sys_server ------------------------------------------------------------- */

static char server_path[PATH_MAX + 16], client_path[PATH_MAX + 16];
static int server_fd = -1;
static pthread_t server_thread;
static atomic_bool server_stop;
static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;
static char server_log[4096];
static atomic_bool umount_fails, setlun_fails;

static void mounts_set_opts(const char * opts) {
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/mounts", proc_root);
    if (opts) write_file(path, "rootfs / rootfs rw 0 0\n%s %s exfat %s 0 0\n", part_dev, sd_root, opts);
    else write_file(path, "rootfs / rootfs rw 0 0\n");
}

static void mounts_set(bool mounted) { mounts_set_opts(mounted ? "rw" : NULL); }

static bool mounts_have(void) {
    char path[PATH_MAX * 2], text[2048];
    snprintf(path, sizeof(path), "%s/mounts", proc_root);
    read_file(path, text, sizeof(text));
    char needle[PATH_MAX + 4];
    snprintf(needle, sizeof(needle), " %s ", sd_root);
    return strstr(text, needle) != NULL;
}

static void send_result(const char * msg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", client_path);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
        if (send(fd, msg, strlen(msg), 0) < 0) { /* the test's client is gone */ }
        char ack[8];
        if (recv(fd, ack, sizeof(ack), 0) < 0) { /* no answer */ }
    }
    close(fd);
}

/* sys_server's MOUNT handlers, as on the stock R3 Pro. */
static void handle(const char * cmd) {
    pthread_mutex_lock(&server_mutex);
    size_t n = strlen(server_log);
    snprintf(server_log + n, sizeof(server_log) - n, "%s\n", cmd);
    pthread_mutex_unlock(&server_mutex);
    char a[PATH_MAX], b[PATH_MAX], msg[PATH_MAX + 64];
    if (sscanf(cmd, "MOUNT:UMOUNT:%s", a) == 1) {
        if (atomic_load(&umount_fails)) {
            snprintf(msg, sizeof(msg), "MOUNT:UMOUNT:FAIL:%s", a);
        } else {
            mounts_set(false);
            rmdir(a);
            snprintf(msg, sizeof(msg), "MOUNT:UMOUNT:OK:%s", a);
        }
        send_result(msg);
    } else if (sscanf(cmd, "MOUNT:SETLUN:%s %s", a, b) == 2) {
        if (atomic_load(&setlun_fails)) {
            send_result("MOUNT:SETLUN:FAIL");
            return;
        }
        FILE * f = fopen(a, "w");
        if (f) {
            fputs(strcmp(b, "empty") == 0 ? "\n" : b, f);
            fclose(f);
        }
        send_result(f ? "MOUNT:SETLUN:OK" : "MOUNT:SETLUN:FAIL");
    } else if (sscanf(cmd, "MOUNT:MOUNT:%s %s", a, b) == 2) {
        /* "already mounted" when the directory exists, as sys_server. */
        if (access(b, F_OK) != 0) {
            mkdirs(b);
            mounts_set(true);
        }
        snprintf(msg, sizeof(msg), "MOUNT:MOUNT:OK:%s", b);
        send_result(msg);
    }
}

static void * server_main(void * arg) {
    (void) arg;
    while (!atomic_load(&server_stop)) {
        int fd = accept(server_fd, NULL, NULL);
        if (fd < 0) continue;
        char buf[1024];
        ssize_t got = recv(fd, buf, sizeof(buf) - 1, 0);
        if (got > 0) {
            buf[got] = '\0';
            if (send(fd, "OK", 2, 0) < 0) { /* sender gone */ }
            close(fd);
            handle(buf);
        } else {
            close(fd);
        }
    }
    return NULL;
}

static void server_start(void) {
    unlink(server_path);
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server_path);
    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(server_fd, 8) != 0) {
        fprintf(stderr, "fake sys_server: %s\n", strerror(errno));
        exit(2);
    }
    pthread_create(&server_thread, NULL, server_main, NULL);
}

static void server_stop_all(void) {
    atomic_store(&server_stop, true);
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
    hiby_sys_server_command("bye"); /* wakes accept() if shutdown did not */
    pthread_join(server_thread, NULL);
}

static void server_log_reset(void) {
    pthread_mutex_lock(&server_mutex);
    server_log[0] = '\0';
    pthread_mutex_unlock(&server_mutex);
}

static bool server_log_has(const char * line) {
    pthread_mutex_lock(&server_mutex);
    bool has = strstr(server_log, line) != NULL;
    pthread_mutex_unlock(&server_mutex);
    return has;
}

/* A card that is mounted, and its device nodes. */
static void card_tree(void) {
    android_tree();
    mkdirs(sd_root);
    mounts_set(true);
    write_file(disk_dev, "");
    write_file(part_dev, "");
    write_file(lun_file, "\n");
}

static void test_sys_server_export(void) {
    card_tree();
    r3pro_setup(NULL);
    const struct board_storage_export_ops * ops = usb_storage_export_ops();
    CHECK(ops == &storage_export_sys_server_ops && ops->lends_card, "lending back end");
    CHECK(ops->host_ejected(), "empty LUN at start");

    server_log_reset();
    reset_logs();
    CHECK(ops->export_card(), "export");
    char line[PATH_MAX * 3];
    snprintf(line, sizeof(line), "MOUNT:UMOUNT:%s\nMOUNT:SETLUN:%s %s\n", sd_root, lun_file, disk_dev);
    CHECK(server_log_has(line), "unmount then SETLUN of the whole disk:\n%s", server_log);
    CHECK(!mounts_have() && access(sd_root, F_OK) != 0, "card unmounted, mount point gone");
    char lun[PATH_MAX];
    read_file(lun_file, lun, sizeof(lun));
    CHECK(strcmp(lun, disk_dev) == 0, "LUN holds the disk: %s", lun);
    CHECK(sys_is(A0 "/functions", "mass_storage"), "port presents storage after SETLUN");
    CHECK(!ops->host_ejected(), "not ejected");

    /* The computer ejects: the LUN empties. */
    write_file(lun_file, "\n");
    CHECK(ops->host_ejected(), "ejected");

    server_log_reset();
    CHECK(ops->reclaim_card(), "reclaim");
    snprintf(line, sizeof(line), "MOUNT:SETLUN:%s empty\nMOUNT:MOUNT:%s %s\n", lun_file, part_dev, sd_root);
    CHECK(server_log_has(line), "SETLUN empty then mount of the partition:\n%s", server_log);
    CHECK(mounts_have(), "card mounted again");
    CHECK(sys_is(A0 "/functions", "adb"), "port idle again");

    /* No partition node: the whole disk is mounted. A leftover empty mount
     * point (sys_server would call it "already mounted") is removed first. */
    unlink(part_dev);
    mounts_set(false);
    mkdirs(sd_root);
    server_log_reset();
    CHECK(ops->reclaim_card(), "reclaim without a partition");
    snprintf(line, sizeof(line), "MOUNT:MOUNT:%s %s\n", disk_dev, sd_root);
    CHECK(server_log_has(line), "disk mounted:\n%s", server_log);
    CHECK(mounts_have(), "mounted after removing the stale mount point");
    write_file(part_dev, "");

    /* An unmount sys_server refuses: no SETLUN, no storage function. */
    atomic_store(&umount_fails, true);
    server_log_reset();
    sys_put(A0 "/functions", "adb");
    CHECK(!ops->export_card(), "export fails");
    CHECK(!server_log_has("MOUNT:SETLUN"), "no SETLUN after a failed unmount");
    CHECK(sys_is(A0 "/functions", "adb"), "port not switched");
    atomic_store(&umount_fails, false);

    /* Something opened a file on the card after the close-time check: no
     * UMOUNT (sys_server would detach it lazily and say OK). */
    char path[PATH_MAX * 2], target[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/88/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/88/fd/3", proc_root);
    snprintf(target, sizeof(target), "%s/Music/a.flac", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    server_log_reset();
    CHECK(!ops->export_card(), "busy card: export fails");
    CHECK(!server_log_has("MOUNT:UMOUNT"), "busy card never unmounted:\n%s", server_log);
    CHECK(mounts_have(), "still mounted");
    snprintf(path, sizeof(path), "%s/88", proc_root);
    rm_rf(path);

    /* SETLUN empty refused while the LUN still holds the disk: the card is
     * not mounted, the reclaim fails and is tried again. */
    CHECK(ops->export_card(), "export again");
    atomic_store(&setlun_fails, true);
    server_log_reset();
    CHECK(!ops->reclaim_card(), "LUN still holds the card: reclaim fails");
    CHECK(!server_log_has("MOUNT:MOUNT"), "never mounted while lent:\n%s", server_log);
    CHECK(!mounts_have(), "still unmounted");
    /* The answer was lost but the LUN let go: mounted. */
    write_file(lun_file, "\n");
    CHECK(ops->reclaim_card(), "LUN empty: reclaimed");
    CHECK(mounts_have(), "mounted");
    atomic_store(&setlun_fails, false);
}

static void test_busy_scan(void) {
    fresh_tree();
    r3pro_setup(NULL);
    char path[PATH_MAX * 2], target[PATH_MAX * 2];
    mkdirs(sd_root);
    /* pid 12: a file on the card; pid 13: elsewhere; pid 14: cwd on the card. */
    snprintf(path, sizeof(path), "%s/12/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/12/fd/3", proc_root);
    snprintf(target, sizeof(target), "%s/Music/a.flac", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    snprintf(path, sizeof(path), "%s/12/comm", proc_root);
    write_file(path, "player\n");
    snprintf(path, sizeof(path), "%s/13/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/13/fd/1", proc_root);
    if (symlink("/tmp/log", path) != 0) perror("symlink");
    snprintf(path, sizeof(path), "%s/14", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/14/cwd", proc_root);
    if (symlink(sd_root, path) != 0) perror("symlink");
    snprintf(path, sizeof(path), "%s/self", proc_root); /* not a pid */
    mkdirs(path);
    /* A sibling directory with the same prefix is not the card. */
    snprintf(path, sizeof(path), "%s/15/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/15/fd/4", proc_root);
    snprintf(target, sizeof(target), "%s_other/x", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    /* pid 16: a file on the card mapped into memory, no fd; pid 17 maps
     * only files elsewhere. */
    snprintf(path, sizeof(path), "%s/16/maps", proc_root);
    write_file(path, "00400000-00452000 r-xp 00000000 1f:02 123 /usr/bin/player\n"
                     "77f00000-77f10000 r--p 00000000 b3:01 456      %s/.mellowtone/library/db (deleted)\n"
                     "7ffd0000-7fff0000 rw-p 00000000 00:00 0          [stack]\n", sd_root);
    snprintf(path, sizeof(path), "%s/17/maps", proc_root);
    write_file(path, "00400000-00452000 r-xp 00000000 1f:02 123 /usr/bin/other\n");

    int pids[4] = { 0 };
    char names[4][32];
    int n = usb_busy_scan(sd_root, pids, names, 4);
    CHECK(n == 3, "three busy processes: %d", n);
    bool saw12 = false, saw14 = false, saw16 = false;
    for (int i = 0; i < n && i < 4; i++) {
        if (pids[i] == 12) saw12 = strcmp(names[i], "player") == 0;
        if (pids[i] == 14) saw14 = true;
        if (pids[i] == 16) saw16 = true;
    }
    CHECK(saw12 && saw14 && saw16, "pids 12 (player), 14 and 16 (mapped)");
    char other[32];
    CHECK(!usb_storage_lend_card_free(sd_root, other, sizeof(other)), "card not free");
    CHECK(other[0], "another process named");
    rm_rf(proc_root);
    mkdirs(proc_root);
    CHECK(usb_storage_lend_card_free(sd_root, other, sizeof(other)) && !other[0], "card free");

    /* Only this process holds the card (close_all() missed a file): busy,
     * but no other process to name. With another one, that one is named. */
    snprintf(path, sizeof(path), "%s/%d/fd", proc_root, (int) getpid());
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/%d/fd/7", proc_root, (int) getpid());
    snprintf(target, sizeof(target), "%s/.mellowtone/library/database_0.tcd", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    snprintf(path, sizeof(path), "%s/%d/comm", proc_root, (int) getpid());
    write_file(path, "mellowtone\n");
    CHECK(usb_busy_log_files(sd_root, (int) getpid()) == 1, "own file listed");
    CHECK(!usb_storage_lend_card_free(sd_root, other, sizeof(other)) && !other[0], "only this process: %s", other);
    snprintf(path, sizeof(path), "%s/176/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/176/fd/1", proc_root);
    snprintf(target, sizeof(target), "%s/.logs/bt_debug.log", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    snprintf(path, sizeof(path), "%s/176/comm", proc_root);
    write_file(path, "syslogd\n");
    CHECK(!usb_storage_lend_card_free(sd_root, other, sizeof(other)) && strcmp(other, "syslogd") == 0,
          "the other process named: %s", other);
    rm_rf(proc_root);
    mkdirs(proc_root);
}

static void fd_target(int fd, char * out, size_t size) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, size - 1);
    out[n > 0 ? n : 0] = '\0';
}

/* stdout/stderr on a log on the card (the launcher's player.out) move to a
 * RAM file while the card is lent, and back after with the RAM part
 * appended. */
static void test_lend_stdio(void) {
    card_tree();
    mt_state_init();
    char dir[PATH_MAX * 2], log_path[PATH_MAX * 2], real_log[PATH_MAX], target[PATH_MAX], text[256];
    snprintf(dir, sizeof(dir), "%s/.mellowtone/logs", sd_root);
    mkdirs(dir);
    snprintf(log_path, sizeof(log_path), "%s/player.out", dir);
    write_file(log_path, "");
    if (!realpath(log_path, real_log)) snprintf(real_log, sizeof(real_log), "%s", log_path);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int log_fd = open(log_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    CHECK(saved >= 0 && log_fd >= 0, "stdout set up");
    if (saved < 0 || log_fd < 0) return;
    dup2(log_fd, STDOUT_FILENO);
    close(log_fd);
    printf("before\n");
    fflush(stdout);

    mt_state_set_card_lent(true);
    fd_target(STDOUT_FILENO, target, sizeof(target));
    CHECK(strcmp(target, MT_LENT_LOG_FILE) == 0, "stdout in RAM while lent: %s", target);
    printf("while lent\n");
    fflush(stdout);
    read_file(log_path, text, sizeof(text));
    CHECK(strcmp(text, "before\n") == 0, "card log untouched while lent: %s", text);

    mt_state_set_card_lent(false);
    fd_target(STDOUT_FILENO, target, sizeof(target));
    CHECK(strcmp(target, real_log) == 0, "stdout back on the card log: %s", target);
    printf("after\n");
    fflush(stdout);
    read_file(log_path, text, sizeof(text));
    CHECK(strcmp(text, "before\nwhile lent\nafter\n") == 0, "RAM part appended: %s", text);
    CHECK(access(MT_LENT_LOG_FILE, F_OK) != 0, "RAM log removed");

    dup2(saved, STDOUT_FILENO);
    close(saved);
}

/* ---- shared card (X1600) ------------------------------------------------------------ */

static atomic_bool remount_fails;

/* busybox mount -o remount,ro / umount on the fake /proc/mounts. */
static int shared_effect(char * const argv[]) {
    if (strcmp(argv[0], "mount") == 0 && argv[1] && argv[2] && strcmp(argv[2], "remount,ro") == 0) {
        if (atomic_load(&remount_fails)) return 255;
        mounts_set_opts("ro,relatime");
        return 0;
    }
    if (strcmp(argv[0], "umount") == 0) {
        if (argv[1] && argv[1][0] == '-') return 1; /* no option is ever expected */
        mounts_set(false);
        return 0;
    }
    return configfs_effect(argv);
}

static void test_shared_export(void) {
    fresh_tree();
    board_desc_t b = { .name = "x1600-test" };
    b.caps = CAP_USB_STORAGE | CAP_USB_DAC;
    b.paths.sd_root = sd_root;
    b.usb_gadget = &usb_gadget_configfs_ops;
    b.storage_export = &storage_export_shared_ops;
    usb_gadget_test_setup(&b, sys_root, proc_root, fake_run, NULL);
    run_effect = shared_effect;
    const struct board_storage_export_ops * ops = usb_storage_export_ops();
    CHECK(!ops->lends_card && ops->session_started && ops->session_ended, "shared back end");

    /* The storage gadget is bound with the card in its LUN, the card mounted. */
    mkdirs(sd_root);
    cfg_gadget("android0", "mass_storage.0", "13500000.otg_new");
    char lun[PATH_MAX * 2], text[256];
    snprintf(lun, sizeof(lun), "%s/" CFG "/android0/functions/mass_storage.0/lun.0/file", sys_root);
    write_file(lun, "/dev/mmcblk0\n");
    mounts_set(true);

    /* A computer has the card: read-only for the player, retried while refused. */
    atomic_store(&remount_fails, true);
    reset_logs();
    CHECK(!ops->session_started(), "remount refused: not yet");
    atomic_store(&remount_fails, false);
    reset_logs();
    CHECK(ops->session_started(), "read-only");
    char want[PATH_MAX * 2];
    snprintf(want, sizeof(want), "mount -o remount,ro %s\n", sd_root);
    CHECK(strcmp(commands, want) == 0, "remount command:\n%s", commands);
    reset_logs();
    CHECK(ops->session_started() && commands[0] == '\0', "already read-only: nothing run:\n%s", commands);

    /* The session ends while something still maps a file on the card: the
     * LUN is emptied at once, the card stays mounted (no lazy unmount). */
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/42/maps", proc_root);
    write_file(path, "77f00000-77f10000 r--p 00000000 b3:01 456 %s/Music/a.flac\n", sd_root);
    reset_logs();
    CHECK(!ops->session_ended(), "busy card: not released");
    CHECK(strstr(commands, "umount") == NULL, "busy card never unmounted:\n%s", commands);
    CHECK(mounts_have(), "still mounted");
    read_file(lun, text, sizeof(text));
    CHECK(strcmp(text, "\n") == 0, "LUN emptied: '%s'", text);
    snprintf(path, sizeof(path), "%s/42", proc_root);
    rm_rf(path);

    reset_logs();
    CHECK(ops->session_ended(), "released");
    snprintf(want, sizeof(want), "umount %s\n", sd_root);
    CHECK(strcmp(commands, want) == 0, "plain unmount:\n%s", commands);
    CHECK(!mounts_have(), "unmounted");

    /* Nothing mounted: nothing to do either way. */
    reset_logs();
    CHECK(ops->session_ended() && ops->session_started() && commands[0] == '\0', "no card: nothing run:\n%s",
          commands);
}

/* ---- the lending state machine ------------------------------------------------------ */

static usb_lend_action_t step(usb_lend_t * l, usb_lend_inputs_t in, uint32_t t) { return usb_lend_step(l, &in, t); }

static void test_lend_machine(void) {
    usb_lend_t l;
    usb_lend_init(&l, false);
    usb_lend_inputs_t base = { .cable = true, .host = true, .want = true };

    /* Nothing without all three. */
    usb_lend_inputs_t in = base;
    in.want = false;
    CHECK(step(&l, in, 0) == USB_LEND_DO_NOTHING, "not wanted");
    in = base;
    in.host = false;
    CHECK(step(&l, in, 0) == USB_LEND_DO_NOTHING, "no host");

    /* Close (pending, then done), export, lent. */
    CHECK(step(&l, base, 0) == USB_LEND_DO_CLOSE && l.state == USB_LEND_CLOSING, "closing");
    CHECK(step(&l, base, 10) == USB_LEND_DO_CLOSE, "close again while pending");
    in = base;
    in.close = USB_LEND_OK;
    CHECK(step(&l, in, 20) == USB_LEND_DO_EXPORT && l.state == USB_LEND_EXPORTING, "export");
    in.work = USB_LEND_PENDING;
    CHECK(step(&l, in, 30) == USB_LEND_DO_NOTHING, "exporting");
    in.work = USB_LEND_OK;
    CHECK(step(&l, in, 40) == USB_LEND_DO_SHOW && l.state == USB_LEND_LENT, "lent");

    /* Eject: reclaim, reopen, then no new lend until unplugged. */
    in = base;
    in.ejected = true;
    CHECK(step(&l, in, 50) == USB_LEND_DO_RECLAIM && l.end == USB_LEND_END_EJECT, "eject -> reclaim");
    in.ejected = false;
    in.work = USB_LEND_OK;
    CHECK(step(&l, in, 60) == USB_LEND_DO_REOPEN && l.state == USB_LEND_REOPENING, "reopen");
    in.reopened = true;
    CHECK(step(&l, in, 70) == USB_LEND_DO_NOTHING && l.state == USB_LEND_IDLE, "idle");
    CHECK(step(&l, base, 80) == USB_LEND_DO_NOTHING && l.declined, "declined while the host stays");
    in = base;
    in.cable = false;
    in.host = false;
    step(&l, in, 90);
    CHECK(!l.declined, "unplug clears the latch");
    CHECK(step(&l, base, 100) == USB_LEND_DO_CLOSE, "lends again after a replug");

    /* Cable pull while lent: debounced, then reclaim with UNPLUG. */
    usb_lend_init(&l, true);
    CHECK(l.state == USB_LEND_LENT, "resumed lent");
    in = base;
    in.cable = false;
    CHECK(step(&l, in, 1000) == USB_LEND_DO_NOTHING, "unplug seen");
    CHECK(step(&l, in, 1500) == USB_LEND_DO_NOTHING, "still debouncing");
    in.cable = true;
    CHECK(step(&l, in, 1600) == USB_LEND_DO_NOTHING && l.state == USB_LEND_LENT, "a glitch is not an unplug");
    in.cable = false;
    step(&l, in, 2000);
    CHECK(step(&l, in, 3000) == USB_LEND_DO_RECLAIM && l.end == USB_LEND_END_UNPLUG, "unplugged");
    /* Reclaim fails twice, then succeeds. */
    in.work = USB_LEND_FAILED;
    CHECK(step(&l, in, 3100) == USB_LEND_DO_RECLAIM && l.reclaim_tries == 2, "retry 1");
    CHECK(step(&l, in, 3200) == USB_LEND_DO_RECLAIM && l.reclaim_tries == 3, "retry 2");
    CHECK(step(&l, in, 3300) == USB_LEND_DO_REOPEN, "gives up after 3 tries, reopens anyway");
    in.reopened = true;
    step(&l, in, 3400);
    CHECK(l.state == USB_LEND_IDLE && !l.declined, "unplug end: not declined");

    /* The Disconnect button. */
    usb_lend_init(&l, true);
    in = base;
    in.user_disconnect = true;
    CHECK(step(&l, in, 0) == USB_LEND_DO_RECLAIM && l.end == USB_LEND_END_USER && l.declined, "user disconnect");

    /* Export failure: undo, the computer never had the card. */
    usb_lend_init(&l, false);
    step(&l, base, 0);
    in = base;
    in.close = USB_LEND_OK;
    step(&l, in, 1);
    in.work = USB_LEND_FAILED;
    CHECK(step(&l, in, 2) == USB_LEND_DO_RECLAIM && l.end == USB_LEND_END_FAILED, "export failed -> reclaim");
    in.work = USB_LEND_OK;
    CHECK(step(&l, in, 3) == USB_LEND_DO_REOPEN && l.end == USB_LEND_END_FAILED, "reopen, end stays FAILED");

    /* Close failure. */
    usb_lend_init(&l, false);
    step(&l, base, 0);
    in = base;
    in.close = USB_LEND_FAILED;
    CHECK(step(&l, in, 1) == USB_LEND_DO_REOPEN && l.end == USB_LEND_END_FAILED && l.declined, "close failed");

    /* Unplugged while still closing. */
    usb_lend_init(&l, false);
    step(&l, base, 0);
    in = base;
    in.cable = false;
    CHECK(step(&l, in, 1) == USB_LEND_DO_REOPEN, "cable out while closing");
}

/* ---- the whole hand-over through usb_storage_lend_poll() -------------------------- */

static int closes, reopens, shows_on, shows_off;
static usb_lend_end_t last_end;
static usb_lend_result_t close_answer = USB_LEND_OK;

static usb_lend_result_t hook_close(void) {
    closes++;
    if (close_answer != USB_LEND_OK) return close_answer;
    /* What gui_usb.c does after closing everything. */
    mt_state_set_card_lent(true);
    return usb_storage_lend_card_free(sd_root, NULL, 0) ? USB_LEND_OK : USB_LEND_FAILED;
}

static void hook_reopen(usb_lend_end_t end) {
    reopens++;
    last_end = end;
    mt_state_set_card_lent(false);
}

static void hook_show(bool on) {
    if (on) shows_on++;
    else shows_off++;
}

static const usb_lend_hooks_t hooks = { hook_close, hook_reopen, hook_show };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static bool poll_until(usb_lend_state_t want, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 20) {
        usb_storage_lend_poll(true, now_ms());
        if (usb_storage_lend_state() == want) return true;
        sleep_ms(20);
    }
    return usb_storage_lend_state() == want;
}

static void test_lend_flow(void) {
    card_tree();
    r3pro_setup(hiby_sys_request);
    usb_storage_lend_set_hooks(&hooks);
    atomic_store(&cable_in, true);
    mt_state_init();
    CHECK(usb_storage_lend_supported(), "lending supported");
    usb_storage_lend_start();
    CHECK(usb_storage_lend_state() == USB_LEND_IDLE && !mt_state_card_lent(), "starts idle");
    CHECK(mt_state_available(), "card writable before");

    /* A computer configures the port: close, export, lent. */
    sys_put(A0 "/state", "CONFIGURED\n");
    CHECK(poll_until(USB_LEND_LENT, 5000), "lent: %s", usb_lend_state_name(usb_storage_lend_state()));
    CHECK(closes == 1 && shows_on == 1, "closed once, screen shown (%d, %d)", closes, shows_on);
    CHECK(mt_state_card_lent() && !mt_state_available(), "writers paused while lent");
    CHECK(!mounts_have() && sys_is(A0 "/functions", "mass_storage"), "card exported");
    CHECK(usb_storage_lend_busy(), "busy");

    /* The power state cannot be read for longer than the unplug debounce:
     * the cable keeps its last state, the card stays lent. */
    atomic_store(&cable_unknown, true);
    for (int t = 0; t < 1500; t += 20) {
        usb_storage_lend_poll(true, now_ms());
        sleep_ms(20);
    }
    CHECK(usb_storage_lend_state() == USB_LEND_LENT && reopens == 0, "unknown power is not an unplug: %s",
          usb_lend_state_name(usb_storage_lend_state()));
    atomic_store(&cable_unknown, false);

    /* The computer ejects. */
    write_file(lun_file, "\n");
    CHECK(poll_until(USB_LEND_IDLE, 8000), "back: %s", usb_lend_state_name(usb_storage_lend_state()));
    CHECK(reopens == 1 && last_end == USB_LEND_END_EJECT && shows_off == 1, "reopened after eject");
    CHECK(mounts_have() && sys_is(A0 "/functions", "adb"), "card mounted, port idle");
    CHECK(!mt_state_card_lent(), "writers resume");
    /* Still configured: not lent again. */
    for (int i = 0; i < 10; i++) usb_storage_lend_poll(true, now_ms());
    CHECK(usb_storage_lend_state() == USB_LEND_IDLE && closes == 1, "declined until unplugged");

    /* Unplug, plug back: lent again; then the cable is pulled. */
    atomic_store(&cable_in, false);
    usb_storage_lend_poll(true, now_ms());
    atomic_store(&cable_in, true);
    CHECK(poll_until(USB_LEND_LENT, 5000), "lent again");
    CHECK(closes == 2, "closed again");
    atomic_store(&cable_in, false);
    sys_put(A0 "/state", "DISCONNECTED\n");
    CHECK(poll_until(USB_LEND_IDLE, 8000), "back after the cable pull");
    CHECK(reopens == 2 && last_end == USB_LEND_END_UNPLUG, "unplug end");
    CHECK(mounts_have(), "mounted after the pull");

    /* Disconnect on the device. */
    atomic_store(&cable_in, true);
    sys_put(A0 "/state", "CONFIGURED\n");
    CHECK(poll_until(USB_LEND_LENT, 5000), "lent a third time");
    usb_storage_lend_user_disconnect();
    CHECK(poll_until(USB_LEND_IDLE, 8000), "back after Disconnect");
    CHECK(last_end == USB_LEND_END_USER, "user end");

    /* A file still open on the card: nothing is exported. */
    atomic_store(&cable_in, false);
    usb_storage_lend_poll(true, now_ms());
    atomic_store(&cable_in, true);
    char path[PATH_MAX * 2], target[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/77/fd", proc_root);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/77/fd/5", proc_root);
    snprintf(target, sizeof(target), "%s/.mellowtone/library/db", sd_root);
    if (symlink(target, path) != 0) perror("symlink");
    server_log_reset();
    for (int i = 0; i < 20; i++) {
        usb_storage_lend_poll(true, now_ms());
        sleep_ms(10);
    }
    CHECK(usb_storage_lend_state() == USB_LEND_IDLE && last_end == USB_LEND_END_FAILED, "busy card: not lent (%s)",
          usb_lend_state_name(usb_storage_lend_state()));
    CHECK(!server_log_has("MOUNT:UMOUNT"), "busy card never unmounted");
    CHECK(!mt_state_card_lent() && mounts_have(), "card stays ours");
    snprintf(path, sizeof(path), "%s/77", proc_root);
    rm_rf(path);

    /* The player restarted during a lend: it resumes lent. */
    write_file(lun_file, "%s", disk_dev);
    usb_storage_lend_start();
    CHECK(usb_storage_lend_state() == USB_LEND_LENT && mt_state_card_lent(), "resumed lent");
    atomic_store(&cable_in, false);
    CHECK(poll_until(USB_LEND_IDLE, 8000), "reclaimed after the resumed lend");
    CHECK(!mt_state_card_lent(), "unlent");
}

/* ---- uac_sa ----------------------------------------------------------------------- */

static void put32(uint8_t * p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static int prepare_answers[4];
static int prepare_calls;
static int fake_ioctl(int fd, unsigned long request, void * arg) {
    (void) fd;
    if (request != UAC_SA_IOC_PREPARE) return -1;
    *(int *) arg = prepare_answers[prepare_calls < 3 ? prepare_calls : 3];
    prepare_calls++;
    return 0;
}

static void test_uac_sa(void) {
    uac_sa_format_t f;
    int pcm[3] = { 1, 44100, 32 };
    CHECK(uac_sa_parse_info(pcm, BOARD_UAC_SA_ABI_READ_EPERM, false, &f) && f.kind == UAC_SA_STREAM_PCM &&
              f.rate == 44100 && f.container_bits == 32,
          "PCM 44.1/32");
    int pcm24[3] = { 1, 96000, 24 };
    CHECK(uac_sa_parse_info(pcm24, BOARD_UAC_SA_ABI_PREPARE_IOCTL, true, &f) && f.container_bits == 32, "24 -> 32");
    int pcm16[3] = { 7, 48000, 16 };
    CHECK(uac_sa_parse_info(pcm16, BOARD_UAC_SA_ABI_READ_EPERM, false, &f) && f.container_bits == 16,
          "READ_EPERM ignores the format field");
    CHECK(!uac_sa_parse_info(pcm16, BOARD_UAC_SA_ABI_PREPARE_IOCTL, true, &f), "unknown format refused (3.10)");
    int dsd[3] = { 2, 88200, 1 };
    CHECK(uac_sa_parse_info(dsd, BOARD_UAC_SA_ABI_PREPARE_IOCTL, true, &f) && f.kind == UAC_SA_STREAM_DSD &&
              f.rate == 88200 && f.container_bits == 32,
          "native DSD64");
    CHECK(strcmp(uac_sa_dsd_name(88200), "DSD64") == 0 && strcmp(uac_sa_dsd_name(176400), "DSD128") == 0,
          "DSD names");
    CHECK(!uac_sa_parse_info(dsd, BOARD_UAC_SA_ABI_PREPARE_IOCTL, false, &f), "DSD refused without the capability");
    CHECK(!uac_sa_parse_info(dsd, BOARD_UAC_SA_ABI_READ_EPERM, true, &f) || f.kind == UAC_SA_STREAM_PCM,
          "no DSD on READ_EPERM");
    int bad[3] = { 1, 8000, 32 };
    CHECK(!uac_sa_parse_info(bad, BOARD_UAC_SA_ABI_READ_EPERM, false, &f), "8 kHz refused");
    int bad_dsd[3] = { 2, 705600, 1 };
    CHECK(!uac_sa_parse_info(bad_dsd, BOARD_UAC_SA_ABI_PREPARE_IOCTL, true, &f), "DSD512 refused");

    uac_sa_io_t io = *uac_sa_io_default();
    io.ioctl = fake_ioctl;
    prepare_answers[0] = 0;
    prepare_answers[1] = 5;
    prepare_answers[2] = -1;
    prepare_calls = 0;
    CHECK(uac_sa_prepare(&io, 3, BOARD_UAC_SA_ABI_PREPARE_IOCTL) == 0, "not armed");
    CHECK(uac_sa_prepare(&io, 3, BOARD_UAC_SA_ABI_PREPARE_IOCTL) == 1, "armed");
    CHECK(uac_sa_prepare(&io, 3, BOARD_UAC_SA_ABI_PREPARE_IOCTL) == -1, "error");
    CHECK(uac_sa_prepare(&io, 3, BOARD_UAC_SA_ABI_READ_EPERM) == 1 && prepare_calls == 3, "READ_EPERM never asks");

    /* DoP: markers alternate, both channels. */
    uint8_t frames[32 * 8];
    for (int i = 0; i < 32; i++) {
        uint32_t marker = (i % 2) ? 0xFA : 0x05;
        put32(frames + i * 8, marker << 24 | 0x1234u << 8);
        put32(frames + i * 8 + 4, marker << 24 | 0xABCDu << 8);
    }
    CHECK(uac_sa_is_dop(frames, 32), "DoP detected");
    CHECK(!uac_sa_is_dop(frames, 8), "too few frames to decide");
    put32(frames + 11 * 8, 0x05u << 24); /* frame 11 should carry FA: breaks the alternation */
    put32(frames + 11 * 8 + 4, 0x05u << 24);
    CHECK(!uac_sa_is_dop(frames, 32), "broken alternation");
    memset(frames, 0, sizeof(frames));
    CHECK(!uac_sa_is_dop(frames, 32), "silence PCM is not DoP");
    int32_t out[64];
    for (int i = 0; i < 2; i++) {
        put32(frames + i * 8, 0x05u << 24 | 0x1234u << 8);
        put32(frames + i * 8 + 4, 0xFAu << 24 | 0xABCDu << 8);
    }
    uac_sa_dop_passthrough(frames, 1, out);
    CHECK(out[0] == 0x00051234 && (uint32_t) out[1] == 0xFFFAABCDu, "DoP bits untouched: %08x %08x",
          (unsigned) out[0], (unsigned) out[1]);

    /* Native DSD: L bytes 11 22 33 44, R 55 66 77 88 (time order) -> two
     * DoP frames: [05 1122, 05 5566], [FA 3344, FA 7788]. */
    const uint8_t native[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F };
    uint8_t marker = 0;
    CHECK(uac_sa_dsd_to_dop(native, 2, out, &marker) == 4, "4 DoP frames");
    CHECK(out[0] == 0x00051122 && out[1] == 0x00055566, "frame 0: %08x %08x", (unsigned) out[0], (unsigned) out[1]);
    CHECK((uint32_t) out[2] == 0xFFFA3344u && (uint32_t) out[3] == 0xFFFA7788u, "frame 1: %08x %08x",
          (unsigned) out[2], (unsigned) out[3]);
    CHECK(out[4] == 0x000599AA && out[5] == 0x0005DDEE, "frame 2");
    CHECK((uint32_t) out[6] == 0xFFFABBCCu && (uint32_t) out[7] == 0xFFFAF00Fu, "frame 3");
    CHECK(marker == 0x05, "alternation carried: next is 05");
    uac_sa_dsd_to_dop(native, 1, out, &marker);
    CHECK((out[0] >> 16 & 0xff) == 0x05 && (out[2] >> 16 & 0xff) == 0xFA, "continues across calls");
}

int main(void) {
    char tmpl[] = "/tmp/mt-usb-test.XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    snprintf(tmp, sizeof(tmp), "%s", tmpl);
    snprintf(sys_root, sizeof(sys_root), "%s/sys", tmp);
    snprintf(proc_root, sizeof(proc_root), "%s/proc", tmp);
    snprintf(adb_flag, sizeof(adb_flag), "%s/adb_on", tmp);
    /* storage_paths.c's host card is ./music: the card root of every test. */
    if (chdir(tmp) != 0) return 2;
    snprintf(sd_root, sizeof(sd_root), "%s/music", tmp);
    snprintf(lun_file, sizeof(lun_file), "%s/lun_file", tmp);
    snprintf(disk_dev, sizeof(disk_dev), "%s/dev/mmcblk0", tmp);
    snprintf(part_dev, sizeof(part_dev), "%s/dev/mmcblk0p1", tmp);
    snprintf(server_path, sizeof(server_path), "%s/sys_server", tmp);
    snprintf(client_path, sizeof(client_path), "%s/sys_client", tmp);
    usb_gadget_test_trace(trace_write);

    hiby_sys_server_test_set_path(server_path);
    hiby_sys_client_test_set_path(client_path);
    server_start();
    if (!hiby_sys_client_start()) {
        fprintf(stderr, "cannot start the sys_client socket\n");
        return 2;
    }

    test_uac_sa();
    test_android_usb();
    test_configfs();
    test_connection();
    test_sys_server_export();
    test_busy_scan();
    test_shared_export();
    test_lend_machine();
    test_lend_flow();
    test_lend_stdio();

    server_stop_all();
    usb_gadget_test_setup(NULL, NULL, NULL, NULL, NULL);
    if (chdir("/") != 0) { /* nothing */ }
    rm_rf(tmp);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("usb: all tests passed\n");
    return 0;
}
