/* Battery and charger tests: the battery reader, power_events,
 * charge control and both charger back ends, against a fake sysfs tree in a
 * temp dir (power_test_setup()), fake uevents on a socketpair
 * (power_events_config_t.uevent_fd) and a fake I2C bus for the AXP2101 and
 * MP2731 registers (charger_axp2101_test_hooks()).
 *
 *   reader       node choice, status quirk (MP2731), external power (online,
 *                UDC), battery current
 *   events       uevent parsing, cache dropped on a relevant uevent / CHARGE:*
 *                only, slow fallback poll
 *   hysteresis   limit 85, resume 80, plug/unplug, jitter, unknown values
 *   stop/resume  charge control over the sysfs back end (charge_enabled,
 *                charge_behaviour), event-driven, DAC inhibit
 *   sysfs ops    voltage / current cap capture and restore, op flags, the
 *                disabled R3 Pro chgmilli candidate
 *   policy       the R3 Pro input-current table for host / charger / none,
 *                applied on an android_usb uevent
 *   axp2101      the exact register traffic for the voltage cap, throttle
 *                and retry, safe current with its baseline file (board-tagged,
 *                never captured at or below the cap),
 *                MP2731, confirmed-off, set_charging for the DAC inhibit */
#include "battery.h"
#include "board.h"
#include "charge_control.h"
#include "charger.h"
#include "power_events.h"
#include "power_sysfs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* The app's BOARD (board.c, power_board()'s default); every test installs
 * its own descriptor with power_test_setup(). */
const board_desc_t BOARD = { .name = "test", .model = "test" };

/* The AXP2101 back end's baseline file goes through the card checks only
 * without a test path; never reached here. */
bool mt_state_prepare_write(const char * path) {
    (void) path;
    return false;
}

/* ---- fake sysfs ------------------------------------------------------------ */

static char root[256];

static void mkdirs(const char * rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    for (char * p = path + strlen(root) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    mkdir(path, 0755);
}

static void put(const char * rel, const char * fmt, ...) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", rel);
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
    fputc('\n', f);
    fclose(f);
}

static int get_int(const char * rel) {
    int v = -999;
    if (!power_sysfs_read_int(rel, &v)) return -999;
    return v;
}

static void get_str(const char * rel, char * out, size_t size) {
    if (!power_sysfs_read(rel, out, size)) snprintf(out, size, "(none)");
}

static void fresh_tree(void) {
    if (root[0]) {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) { /* best effort */ }
    }
    const char * tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/mt-power-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        exit(2);
    }
    mkdirs("class/power_supply");
}

static void use(const board_desc_t * board) {
    power_test_setup(board, root);
    battery_test_reset();
    charge_control_test_reset();
    charger_sysfs_test_reset(false);
}

static void battery_node(const char * name, int capacity, const char * status) {
    char rel[256];
    snprintf(rel, sizeof(rel), "class/power_supply/%s/type", name);
    put(rel, "Battery");
    snprintf(rel, sizeof(rel), "class/power_supply/%s/capacity", name);
    put(rel, "%d", capacity);
    snprintf(rel, sizeof(rel), "class/power_supply/%s/status", name);
    put(rel, "%s", status);
}

static void supply_node(const char * name, const char * type, int online) {
    char rel[256];
    snprintf(rel, sizeof(rel), "class/power_supply/%s/type", name);
    put(rel, "%s", type);
    snprintf(rel, sizeof(rel), "class/power_supply/%s/online", name);
    put(rel, "%d", online);
}

/* ---- power events ------------------------------------------------------------ */

static int uevent_peer = -1;

static void events_start(int poll_ms) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        perror("socketpair");
        exit(2);
    }
    uevent_peer = sv[1];
    power_events_config_t cfg = { .uevent_fd = sv[0], .poll_interval_ms = poll_ms, .fast_interval_ms = 50,
                                  .coalesce_ms = 10 };
    CHECK(power_events_start(&cfg), "start");
}

static void events_stop(void) {
    power_events_stop();
    if (uevent_peer >= 0) close(uevent_peer);
    uevent_peer = -1;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits until the generation passes `after`; false on timeout. */
static bool wait_dispatch(unsigned after, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 5) {
        if (power_events_generation() != after) {
            sleep_ms(5); /* let the subscribers finish */
            return true;
        }
        sleep_ms(5);
    }
    return false;
}

static void send_uevent(const char * subsystem, const char * extra_key) {
    char buf[512];
    size_t n = 0;
    n += (size_t) snprintf(buf + n, sizeof(buf) - n, "change@/devices/platform/test/%s", subsystem) + 1;
    n += (size_t) snprintf(buf + n, sizeof(buf) - n, "ACTION=change") + 1;
    n += (size_t) snprintf(buf + n, sizeof(buf) - n, "DEVPATH=/devices/platform/test/%s", subsystem) + 1;
    n += (size_t) snprintf(buf + n, sizeof(buf) - n, "SUBSYSTEM=%s", subsystem) + 1;
    if (extra_key) n += (size_t) snprintf(buf + n, sizeof(buf) - n, "%s", extra_key) + 1;
    if (send(uevent_peer, buf, n, 0) < 0) perror("send");
}

/* Sends a uevent and waits for its dispatch. */
static bool uevent_and_wait(const char * subsystem, const char * extra_key) {
    unsigned g = power_events_generation();
    send_uevent(subsystem, extra_key);
    return wait_dispatch(g, 2000);
}

/* ---- tests ------------------------------------------------------------------- */

static void test_parse(void) {
    static const char msg[] = "change@/devices/platform/axp/power_supply/battery\0ACTION=change\0"
                              "DEVPATH=/devices/platform/axp/power_supply/battery\0SUBSYSTEM=power_supply\0"
                              "POWER_SUPPLY_NAME=battery\0POWER_SUPPLY_CAPACITY=58";
    power_uevent_t ev;
    CHECK(power_uevent_parse(msg, sizeof(msg), &ev), "parse");
    CHECK(strcmp(ev.action, "change") == 0, "action %s", ev.action);
    CHECK(strcmp(ev.subsystem, "power_supply") == 0, "subsystem %s", ev.subsystem);
    CHECK(strcmp(ev.name, "battery") == 0, "name %s", ev.name);
    CHECK(power_uevent_relevant(&ev), "relevant");

    static const char usb[] = "change@/devices/virtual/android_usb/android0\0ACTION=change\0"
                              "SUBSYSTEM=android_usb\0USB_STATE=CONFIGURED";
    CHECK(power_uevent_parse(usb, sizeof(usb), &ev) && power_uevent_relevant(&ev), "android_usb");
    CHECK(strcmp(ev.usb_state, "CONFIGURED") == 0, "usb_state %s", ev.usb_state);

    static const char input[] = "add@/devices/virtual/input/input9\0ACTION=add\0SUBSYSTEM=input";
    CHECK(power_uevent_parse(input, sizeof(input), &ev) && !power_uevent_relevant(&ev), "input not relevant");

    static const char udev[] = "libudev\0\xfe\xed\xca\xfe";
    CHECK(!power_uevent_parse(udev, sizeof(udev), &ev), "libudev rejected");
    CHECK(!power_uevent_parse("garbage", 7, &ev), "no NUL rejected");
    static const char nosub[] = "change@/x\0ACTION=change";
    CHECK(!power_uevent_parse(nosub, sizeof(nosub), &ev), "no SUBSYSTEM rejected");

    CHECK(!power_events_sys_message("WIFI:SCAN:OK"), "WIFI not power");
    CHECK(!power_events_sys_message("BT:CODEC 1"), "BT not power");
}

static void test_reader(void) {
    fresh_tree();
    battery_node("battery", 57, "Discharging");
    battery_node("axp_battery", 99, "Full");
    supply_node("mp2731-charger", "USB", 1);
    put("class/power_supply/mp2731-charger/status", "Charging");
    put("class/power_supply/axp_battery/current_now", "-123000");

    /* R3 Pro II: battery node preferred over another Battery; status from the charger. */
    board_desc_t quirk = { .name = "quirk" };
    quirk.power.battery_node = "battery";
    quirk.power.charger_node = "mp2731-charger";
    quirk.power.quirks = BOARD_POWER_STATUS_FROM_CHARGER;
    use(&quirk);
    CHECK(battery_get_percent() == 57, "percent %d", battery_get_percent());
    CHECK(battery_is_charging(), "charging via MP2731 status");
    CHECK(!battery_is_full(), "not full");
    char status[24];
    CHECK(battery_get_status(status, sizeof(status)) && strcmp(status, "Charging") == 0, "status %s", status);
    CHECK(battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED, "online=1");
    int ua = 0;
    CHECK(!battery_get_current_ua(&ua), "no current without CAP_BATTERY_CURRENT");

    /* Without the quirk the battery node's own status counts. */
    board_desc_t plain = quirk;
    plain.power.quirks = 0;
    use(&plain);
    CHECK(!battery_is_charging(), "battery status Discharging");

    /* Unknown preferred node: the (only remaining) Battery-typed node. */
    board_desc_t other = plain;
    other.power.battery_node = "nonexistent";
    put("class/power_supply/axp_battery/type", "Mains"); /* leave one Battery */
    put("class/power_supply/axp_battery/online", "0");
    use(&other);
    CHECK(battery_get_percent() == 57, "fallback node %d", battery_get_percent());

    /* Battery current from the descriptor's node (R3 Pro: axp_battery). */
    board_desc_t cur = plain;
    cur.caps = CAP_BATTERY_CURRENT;
    cur.power.current_node = "axp_battery";
    use(&cur);
    CHECK(battery_get_current_ua(&ua) && ua == -123000, "current %d", ua);
    cur.power.current_node = NULL; /* battery node: no current_now there */
    use(&cur);
    CHECK(!battery_get_current_ua(&ua), "no current_now on the battery node");

    /* External power: everything offline -> disconnected; a UDC in a
     * connected state -> connected; an unreadable online -> unknown. */
    put("class/power_supply/mp2731-charger/online", "0");
    use(&plain);
    CHECK(battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED, "all offline");
    put("class/udc/13500000.otg/state", "configured");
    CHECK(battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED, "UDC configured");
    put("class/udc/13500000.otg/state", "not attached");
    put("class/power_supply/mp2731-charger/online", "?");
    CHECK(battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_UNKNOWN, "unreadable online");
}

static void test_events(void) {
    fresh_tree();
    battery_node("battery", 57, "Discharging");
    supply_node("usb", "USB", 0);
    board_desc_t b = { .name = "events" };
    b.power.battery_node = "battery";
    use(&b);

    events_start(60000);
    battery_init();
    sleep_ms(50); /* the START dispatch */
    CHECK(battery_get_percent() == 57, "initial %d", battery_get_percent());

    /* Cached: a change without an event is not seen ... */
    put("class/power_supply/battery/capacity", "58");
    CHECK(battery_get_percent() == 57, "cached %d", battery_get_percent());
    /* ... nor after an unrelated uevent ... */
    unsigned g = power_events_generation();
    send_uevent("input", NULL);
    CHECK(!wait_dispatch(g, 150), "input uevent dispatched");
    CHECK(battery_get_percent() == 57, "after input uevent %d", battery_get_percent());
    /* ... but a power_supply uevent drops the cache. */
    CHECK(uevent_and_wait("power_supply", "POWER_SUPPLY_NAME=battery"), "power_supply dispatch");
    CHECK(battery_get_percent() == 58, "after uevent %d", battery_get_percent());

    /* CHARGE:* from the R3 Pro's sys_client socket. */
    put("class/power_supply/battery/capacity", "59");
    g = power_events_generation();
    CHECK(power_events_sys_message("CHARGE:USB_INSTD"), "CHARGE:USB_INSTD");
    CHECK(wait_dispatch(g, 2000), "sys_server dispatch");
    CHECK(battery_get_percent() == 59, "after CHARGE %d", battery_get_percent());

    /* A burst is coalesced into few dispatches. */
    g = power_events_generation();
    for (int i = 0; i < 20; i++) send_uevent("power_supply", NULL);
    CHECK(wait_dispatch(g, 2000), "burst dispatch");
    sleep_ms(100);
    unsigned dispatched = power_events_generation() - g;
    CHECK(dispatched >= 1 && dispatched <= 5, "burst of 20 -> %u dispatches", dispatched);
    events_stop();

    /* The slow fallback poll catches drift without any event. */
    use(&b);
    events_start(150);
    battery_init();
    sleep_ms(50);
    CHECK(battery_get_percent() == 59, "poll initial %d", battery_get_percent());
    put("class/power_supply/battery/capacity", "60");
    g = power_events_generation();
    CHECK(wait_dispatch(g, 2000), "poll dispatch");
    CHECK(battery_get_percent() == 60, "after poll %d", battery_get_percent());
    events_stop();
}

static void test_hysteresis(void) {
    const battery_external_power_state_t ON = BATTERY_EXTERNAL_POWER_CONNECTED;
    const battery_external_power_state_t OFF = BATTERY_EXTERNAL_POWER_DISCONNECTED;
    const battery_external_power_state_t UNK = BATTERY_EXTERNAL_POWER_UNKNOWN;
    struct {
        int capacity;
        battery_external_power_state_t power;
        bool charge;
    } steps[] = {
        { 70, ON, true },   { 84, ON, true },   { 85, ON, false }, /* stop at the limit */
        { 84, ON, false },  { 85, ON, false },  { 83, ON, false }, /* jitter below the limit */
        { 81, ON, false },  { 80, ON, true },                       /* resume 5 % below */
        { 81, ON, true },   { 82, ON, true },   { 86, ON, false },  /* overshoot also stops */
        { -1, ON, false },                                           /* unknown capacity: keep */
        { 79, UNK, true },  { 90, UNK, false },                      /* unknown power: no reset */
        { 90, OFF, true },                                           /* unplugged: reset */
        { 90, ON, false },                                           /* plugged in above the limit */
        { 90, OFF, true },  { 83, ON, true },   { 85, ON, false },   /* plugged in between */
        { 84, OFF, true },  { 84, ON, true },                        /* replug just under */
    };
    charge_hysteresis_t h = { false };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        bool got = charge_hysteresis_step(&h, steps[i].capacity, steps[i].power, 85, 80);
        CHECK(got == steps[i].charge, "step %zu (%d%%, power %d): %d", i, steps[i].capacity, (int) steps[i].power, got);
    }
}

static board_desc_t stop_resume_board(void) {
    board_desc_t b = { .name = "stop_resume" };
    b.caps = CAP_CHARGE_CONTROL | CAP_USB_DAC;
    b.power.battery_node = "battery";
    b.power.usb_node = "usb";
    b.power.charge_method = BOARD_CHARGE_METHOD_STOP_RESUME;
    b.charger = &charger_sysfs_ops;
    return b;
}

static void capacity_event(int capacity) {
    char v[8];
    snprintf(v, sizeof(v), "%d", capacity);
    put("class/power_supply/battery/capacity", "%s", v);
    CHECK(uevent_and_wait("power_supply", "POWER_SUPPLY_NAME=battery"), "uevent %d%%", capacity);
}

static void test_stop_resume(void) {
    fresh_tree();
    battery_node("battery", 84, "Charging");
    put("class/power_supply/battery/charge_enabled", "1");
    supply_node("usb", "USB", 1);
    board_desc_t b = stop_resume_board();
    use(&b);
    events_start(60000);
    battery_init();
    charge_control_init();
    sleep_ms(50);

    const char * en = "class/power_supply/battery/charge_enabled";
    CHECK(charger_supported() & CHARGER_OP_SET_CHARGING, "charge_enabled is a switch");
    CHECK(charge_control_inhibit_supported(), "inhibit supported");
    charge_control_poll(true, true);
    CHECK(get_int(en) == 1, "84%%: charging");
    CHECK(!charge_control_is_holding(), "not holding at 84");

    capacity_event(85);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 0, "85%%: stopped");
    CHECK(charge_control_is_holding() && charge_control_is_confirmed_off(), "holding at 85");

    capacity_event(83);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 0, "83%%: still stopped");

    /* No event, no evaluation: nothing polls sysfs on a timer. */
    put("class/power_supply/battery/capacity", "70");
    battery_invalidate();
    charge_control_poll(true, false);
    CHECK(get_int(en) == 0, "no event: unchanged");

    capacity_event(80);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 1, "80%%: resumed");
    CHECK(!charge_control_is_holding(), "not holding at 80");

    /* Unplug at the limit resets; replug above it stops at once. */
    capacity_event(88);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 0, "88%%: stopped");
    put("class/power_supply/usb/online", "0");
    capacity_event(88);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 1, "unplugged: charger left enabled");
    put("class/power_supply/usb/online", "1");
    capacity_event(88);
    charge_control_poll(true, false);
    CHECK(get_int(en) == 0, "replugged at 88%%: stopped");

    /* Limit off: charging back on. */
    charge_control_poll(false, true);
    CHECK(get_int(en) == 1, "limit off");
    CHECK(!charge_control_is_holding(), "not holding with the limit off");

    /* "Don't charge in DAC mode". */
    charge_control_set_inhibit(true);
    charge_control_poll(false, false);
    CHECK(get_int(en) == 0, "inhibit: off");
    CHECK(!charge_control_is_confirmed_off(), "inhibit is not the limit");
    charge_control_set_inhibit(false);
    charge_control_poll(false, false);
    CHECK(get_int(en) == 1, "inhibit lifted");
    events_stop();

    /* charge_behaviour (Linux 5.17+) is preferred over charge_enabled. */
    fresh_tree();
    battery_node("battery", 90, "Charging");
    put("class/power_supply/battery/charge_behaviour", "[auto] inhibit-charge force-discharge");
    put("class/power_supply/battery/charge_enabled", "1");
    supply_node("usb", "USB", 1);
    use(&b);
    charge_control_poll(true, true);
    char s[64];
    get_str("class/power_supply/battery/charge_behaviour", s, sizeof(s));
    CHECK(strcmp(s, "inhibit-charge") == 0, "charge_behaviour %s", s);
    CHECK(get_int(en) == 1, "charge_enabled untouched");
    charge_control_poll(false, true);
    get_str("class/power_supply/battery/charge_behaviour", s, sizeof(s));
    CHECK(strcmp(s, "auto") == 0, "charge_behaviour %s", s);

    /* Without power events the decision is re-checked every 5 s (here:
     * forced), never left stale. */
    CHECK(charge_control_is_holding() == false, "holding off");
}

static void test_sysfs_ops(void) {
    fresh_tree();
    battery_node("battery", 50, "Charging");
    supply_node("usb", "USB", 1);
    put("class/power_supply/usb/constant_charge_current_max", "1200");
    board_desc_t b = stop_resume_board();
    use(&b);
    const struct board_charger_ops * ops = &charger_sysfs_ops;
    CHECK(ops->supported() == (CHARGER_OP_CHARGE_DONE), "bare tree: 0x%x", ops->supported());
    CHECK(!ops->set_charging(false), "no switch");
    CHECK(!ops->set_current_cap(true), "usb_node is not the charge path");
    CHECK(get_int("class/power_supply/usb/constant_charge_current_max") == 1200, "usb untouched");

    put("class/power_supply/battery/constant_charge_voltage", "4400000");
    put("class/power_supply/battery/constant_charge_current_max", "1500000");
    unsigned sup = ops->supported();
    CHECK((sup & CHARGER_OP_CHARGE_VOLTAGE) && (sup & CHARGER_OP_CURRENT_CAP), "0x%x", sup);
    CHECK(ops->set_charge_voltage(4200), "voltage");
    CHECK(get_int("class/power_supply/battery/constant_charge_voltage") == 4200000, "4.2 V");
    CHECK(ops->set_charge_voltage(4200), "voltage again");
    CHECK(ops->set_charge_voltage(CHARGER_VOLTAGE_STOCK), "stock");
    CHECK(get_int("class/power_supply/battery/constant_charge_voltage") == 4400000, "restored 4.4 V");
    CHECK(ops->set_charge_voltage(CHARGER_VOLTAGE_STOCK), "stock again (nothing to do)");

    CHECK(ops->set_current_cap(true), "cap");
    CHECK(get_int("class/power_supply/battery/constant_charge_current_max") == 500000, "500 mA");
    CHECK(ops->set_current_cap(false), "uncap");
    CHECK(get_int("class/power_supply/battery/constant_charge_current_max") == 1500000, "restored");
    put("class/power_supply/battery/constant_charge_current_max", "300000");
    CHECK(ops->set_current_cap(true), "cap below");
    CHECK(get_int("class/power_supply/battery/constant_charge_current_max") == 300000, "never raised");

    /* A dedicated charger node comes first. */
    supply_node("mp2731-charger", "USB", 1);
    put("class/power_supply/mp2731-charger/constant_charge_voltage", "4350000");
    board_desc_t c = b;
    c.power.charger_node = "mp2731-charger";
    use(&c);
    CHECK(ops->set_charge_voltage(4100), "charger node voltage");
    CHECK(get_int("class/power_supply/mp2731-charger/constant_charge_voltage") == 4100000, "on the charger node");
    CHECK(get_int("class/power_supply/battery/constant_charge_voltage") == 4400000, "battery node untouched");
    CHECK(ops->set_charge_voltage(CHARGER_VOLTAGE_STOCK), "charger node stock");
    CHECK(get_int("class/power_supply/mp2731-charger/constant_charge_voltage") == 4350000, "restored 4.35 V");

    /* The R3 Pro axp_battery chgmilli* candidate: off by default. */
    fresh_tree();
    battery_node("battery", 50, "Charging");
    put("class/power_supply/axp_battery/type", "Battery");
    put("class/power_supply/axp_battery/chgmillicur", "300");
    board_desc_t r3 = b;
    use(&r3);
    CHECK(!(ops->supported() & CHARGER_OP_SET_CHARGING), "chgmilli disabled by default");
    CHECK(!ops->set_charging(false), "chgmilli not written");
    CHECK(get_int("class/power_supply/axp_battery/chgmillicur") == 300, "untouched");
    charger_sysfs_test_reset(true);
    CHECK(ops->supported() & CHARGER_OP_SET_CHARGING, "chgmilli enabled for the test");
    CHECK(ops->set_charging(false), "chgmilli stop");
    CHECK(get_int("class/power_supply/axp_battery/chgmillicur") == 0, "placeholder stop value");
    CHECK(ops->set_charging(true), "chgmilli resume");
    CHECK(get_int("class/power_supply/axp_battery/chgmillicur") == 300, "restored");
    charger_sysfs_test_reset(false);
}

/* The R3 Pro's table, as boards/r3pro/board.c has it (test_board checks
 * the descriptor's own values). */
static const board_input_current_policy_t r3pro_policy = {
    .gadget_state = "class/android_usb/android0/state",
    .gadget_max_power = "class/android_usb/android0/MaxPower",
    .host = { 500, 500, 500 },
    .charger = { 500, 0, 1200 },
    .has_dac_no_charge = true,
    .dac_no_charge = { 10, 100, 0 }, /* the stock dac_charge_disable row */
};

static void policy_tree(const char * state, int online) {
    put("class/android_usb/android0/state", "%s", state);
    put("class/power_supply/usb/online", "%d", online);
}

#define MAXP "class/android_usb/android0/MaxPower"
#define LIMIT "class/power_supply/usb/charge_control_limit_max"
#define CURRENT "class/power_supply/usb/constant_charge_current_max"

static void sentinels(void) {
    put(MAXP, "777");
    put(LIMIT, "777");
    put(CURRENT, "777");
}

static void test_policy(void) {
    fresh_tree();
    battery_node("battery", 50, "Charging");
    supply_node("usb", "USB", 0);
    board_desc_t b = { .name = "r3pro-like" };
    b.caps = CAP_INPUT_CURRENT;
    b.power.battery_node = "battery";
    b.power.usb_node = "usb";
    b.power.input_current = &r3pro_policy;
    b.charger = &charger_sysfs_ops;
    use(&b);
    CHECK(charger_supported() & CHARGER_OP_INPUT_CURRENT, "input current op");

    sentinels();
    policy_tree("CONFIGURED", 1);
    CHECK(charger_detect_input() == CHARGER_INPUT_HOST, "configured = host");
    CHECK(charger_input_current_apply(), "apply host");
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 500 && get_int(CURRENT) == 500, "host %d/%d/%d",
          get_int(MAXP), get_int(LIMIT), get_int(CURRENT));

    sentinels();
    policy_tree("CONNECTED", 0);
    CHECK(charger_detect_input() == CHARGER_INPUT_HOST, "connected = host");

    sentinels();
    policy_tree("DISCONNECTED", 1);
    CHECK(charger_detect_input() == CHARGER_INPUT_CHARGER, "online, no enumeration = charger");
    CHECK(charger_input_current_apply(), "apply charger");
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 0 && get_int(CURRENT) == 1200, "charger %d/%d/%d",
          get_int(MAXP), get_int(LIMIT), get_int(CURRENT));

    sentinels();
    policy_tree("DISCONNECTED", 0);
    CHECK(charger_detect_input() == CHARGER_INPUT_NONE, "unplugged");
    CHECK(charger_input_current_apply(), "apply none");
    CHECK(get_int(MAXP) == 777 && get_int(LIMIT) == 777 && get_int(CURRENT) == 777, "none writes nothing");

    /* Unreadable gadget state and online: nothing written either. */
    char path[512];
    snprintf(path, sizeof(path), "%s/class/android_usb/android0/state", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/class/power_supply/usb/online", root);
    unlink(path);
    CHECK(charger_detect_input() == CHARGER_INPUT_NONE, "unreadable = none");

    /* Event-driven: an android_usb uevent applies the host row. */
    sentinels();
    policy_tree("DISCONNECTED", 0);
    events_start(60000);
    charger_input_current_start();
    sleep_ms(50);
    CHECK(get_int(MAXP) == 777, "nothing while unplugged");
    policy_tree("CONFIGURED", 1);
    CHECK(uevent_and_wait("android_usb", "USB_STATE=CONFIGURED"), "gadget uevent");
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 500 && get_int(CURRENT) == 500, "host on event");
    /* A driver resetting a limit right after the plug is fixed by the fast
     * re-checks (50 ms here, 2 s on the device) without another event. */
    put(LIMIT, "999");
    sleep_ms(300);
    CHECK(get_int(LIMIT) == 500, "re-applied inside the settle window: %d", get_int(LIMIT));
    /* CHARGE:USB_RMVD then a wall charger. */
    policy_tree("DISCONNECTED", 1);
    unsigned g = power_events_generation();
    power_events_sys_message("CHARGE:AC_INSTD");
    CHECK(wait_dispatch(g, 2000), "CHARGE dispatch");
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 0 && get_int(CURRENT) == 1200, "charger on CHARGE:*");
    events_stop();

    /* "Don't charge in USB DAC mode" through the policy (no charge control
     * on this board): the host row becomes the dac_no_charge row at once,
     * and comes back when it is turned off. A wall charger is unaffected. */
    CHECK(!(charger_supported() & CHARGER_OP_SET_CHARGING), "no charging switch here");
    CHECK(charge_control_inhibit_supported(), "inhibit offered through the policy");
    policy_tree("CONFIGURED", 1);
    charge_control_set_inhibit(true);
    CHECK(get_int(MAXP) == 10 && get_int(LIMIT) == 100 && get_int(CURRENT) == 0, "DAC no charge %d/%d/%d",
          get_int(MAXP), get_int(LIMIT), get_int(CURRENT));
    policy_tree("DISCONNECTED", 1);
    CHECK(charger_input_current_apply(), "apply charger while inhibited");
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 0 && get_int(CURRENT) == 1200, "charger row while inhibited");
    policy_tree("CONFIGURED", 1);
    charge_control_set_inhibit(false);
    CHECK(get_int(MAXP) == 500 && get_int(LIMIT) == 500 && get_int(CURRENT) == 500, "host row back");
    charge_control_set_inhibit(false);

    /* A policy without the row: no inhibit offered. */
    board_input_current_policy_t plain = r3pro_policy;
    plain.has_dac_no_charge = false;
    b.power.input_current = &plain;
    CHECK(!charge_control_inhibit_supported(), "no row, no inhibit");
}

/* ---- fake I2C ------------------------------------------------------------------ */

#define AXP 0x34
#define MP 0x4b

static uint8_t regs[2][256];
static bool i2c_fail;
static char i2c_log[4096];

static int chip(uint8_t addr) { return addr == AXP ? 0 : addr == MP ? 1 : -1; }

static bool fake_xfer(const char * bus, uint8_t addr, uint8_t reg, uint8_t * value, bool write) {
    if (strcmp(bus, "/dev/i2c-0") != 0 || chip(addr) < 0 || i2c_fail) return false;
    size_t n = strlen(i2c_log);
    if (write) {
        regs[chip(addr)][reg] = *value;
        snprintf(i2c_log + n, sizeof(i2c_log) - n, "W%02x:%02x=%02x ", addr, reg, *value);
    } else {
        *value = regs[chip(addr)][reg];
        snprintf(i2c_log + n, sizeof(i2c_log) - n, "R%02x:%02x ", addr, reg);
    }
    return true;
}

static char baseline_file[300];

static board_desc_t axp_board(bool mp2731) {
    board_desc_t b = { .name = mp2731 ? "r3proii-like" : "r1-like" };
    b.caps = CAP_CHARGE_CONTROL | CAP_USB_DAC;
    b.power.battery_node = "battery";
    b.power.charge_chips = BOARD_CHARGER_AXP2101 | (mp2731 ? BOARD_CHARGER_MP2731 : 0);
    b.power.axp_voltage_baseline = mp2731 ? 5 : 4;
    b.power.charge_method = BOARD_CHARGE_METHOD_VOLTAGE_CAP;
    if (mp2731) {
        b.power.charger_node = "mp2731-charger";
        b.power.quirks = BOARD_POWER_STATUS_FROM_CHARGER;
    }
    b.charger = &charger_axp2101_i2c_ops;
    return b;
}

static void axp_reset(const board_desc_t * b) {
    memset(regs, 0, sizeof(regs));
    i2c_fail = false;
    i2c_log[0] = '\0';
    snprintf(baseline_file, sizeof(baseline_file), "%s/charge_baseline.txt", root);
    unlink(baseline_file);
    use(b);
    charger_axp2101_test_hooks(fake_xfer, baseline_file);
}

static void test_axp2101(void) {
    fresh_tree();
    battery_node("battery", 90, "Charging");
    board_desc_t r1 = axp_board(false);
    axp_reset(&r1);
    regs[0][0x64] = 4; /* stock 4.35 V */
    regs[0][0x18] = 0x01;

    /* The voltage cap's exact traffic: voltage read, write, read back;
     * charger enable read, write, read back. */
    charge_control_poll(true, true);
    CHECK(strcmp(i2c_log, "R34:64 W34:64=03 R34:64 R34:18 W34:18=03 R34:18 ") == 0, "enable: %s", i2c_log);
    CHECK(charge_control_is_holding(), "holding");

    /* Throttled: nothing within 5 s unless forced. */
    i2c_log[0] = '\0';
    charge_control_poll(true, false);
    CHECK(i2c_log[0] == '\0', "throttled: %s", i2c_log);
    /* Re-asserted when forced, reads only once in place. */
    charge_control_poll(true, true);
    CHECK(strcmp(i2c_log, "R34:64 R34:18 ") == 0, "steady: %s", i2c_log);

    /* Not done yet; REG01 charge-done -> confirmed off. */
    regs[0][0x01] = 0x03;
    CHECK(!charge_control_is_confirmed_off(), "still charging");
    regs[0][0x01] = 0x04;
    CHECK(charge_control_is_confirmed_off(), "done under the cap");

    /* Off: back to the board's stock target. */
    i2c_log[0] = '\0';
    charge_control_poll(false, true);
    CHECK(regs[0][0x64] == 4, "baseline 4.35 V: %u", regs[0][0x64]);
    CHECK(strcmp(i2c_log, "R34:64 W34:64=04 R34:64 R34:18 ") == 0, "disable: %s", i2c_log);
    CHECK(!charge_control_is_holding() && !charge_control_is_confirmed_off(), "released");

    /* A failed transfer is retried after 1 s, not 5 s. */
    i2c_fail = true;
    charge_control_poll(true, true);
    CHECK(!charge_control_is_holding(), "failed: not holding");
    i2c_fail = false;
    i2c_log[0] = '\0';
    charge_control_poll(true, false);
    CHECK(i2c_log[0] == '\0', "retry waits: %s", i2c_log);
    sleep_ms(1100);
    charge_control_poll(true, false);
    CHECK(regs[0][0x64] == 3 && charge_control_is_holding(), "retried after 1 s");

    /* Safe charging: 500 mA = N 11; the baseline is captured and saved first. */
    regs[0][0x62] = 0xE0 | 0x12; /* upper bits kept; N 18 = 1200 mA */
    i2c_log[0] = '\0';
    charge_control_safe_current_poll(true, true);
    CHECK(regs[0][0x62] == (0xE0 | 0x0B), "capped: 0x%02x", regs[0][0x62]);
    FILE * f = fopen(baseline_file, "r");
    char line[128] = "";
    if (f) {
        if (!fgets(line, sizeof(line), f)) line[0] = '\0';
        fclose(f);
    }
    CHECK(strcmp(line, "version=3 board=r1-like axp_current=18\n") == 0, "baseline file '%s'", line);
    charge_control_safe_current_poll(false, true);
    CHECK(regs[0][0x62] == (0xE0 | 0x12), "restored: 0x%02x", regs[0][0x62]);
    /* A later run loads the file instead of reading the (capped) register. */
    regs[0][0x62] = 0x0B;
    use(&r1);
    charger_axp2101_test_hooks(fake_xfer, baseline_file);
    charge_control_safe_current_poll(false, true);
    CHECK(regs[0][0x62] == 0x12, "restored from the file: 0x%02x", regs[0][0x62]);

    /* Another board's file is not this device's baseline, and a register at
     * or below the cap (this player's own cap from before) is never
     * captured: nothing known to restore, so it stays capped. */
    FILE * w = fopen(baseline_file, "w");
    if (w) {
        fputs("version=3 board=other axp_current=20\n", w);
        fclose(w);
    }
    regs[0][0x62] = 0x0B;
    use(&r1);
    charger_axp2101_test_hooks(fake_xfer, baseline_file);
    charge_control_safe_current_poll(true, true);
    charge_control_safe_current_poll(false, true);
    CHECK(regs[0][0x62] == 0x0B, "no baseline: left capped: 0x%02x", regs[0][0x62]);
    f = fopen(baseline_file, "r");
    line[0] = '\0';
    if (f) {
        if (!fgets(line, sizeof(line), f)) line[0] = '\0';
        fclose(f);
    }
    CHECK(strcmp(line, "version=3 board=other axp_current=20\n") == 0, "nothing captured at the cap: '%s'", line);
    /* An old version-2 file (it lived on the card) is ignored too; the live
     * value above the cap is captured and saved for this board. */
    w = fopen(baseline_file, "w");
    if (w) {
        fputs("version=2 axp_current=20\n", w);
        fclose(w);
    }
    regs[0][0x62] = 0x10;
    use(&r1);
    charger_axp2101_test_hooks(fake_xfer, baseline_file);
    charge_control_safe_current_poll(true, true);
    CHECK(regs[0][0x62] == 0x0B, "capped: 0x%02x", regs[0][0x62]);
    charge_control_safe_current_poll(false, true);
    CHECK(regs[0][0x62] == 0x10, "restored the captured value: 0x%02x", regs[0][0x62]);
    regs[0][0x62] = 0x12;

    /* "Don't charge in DAC mode": REG18H bit 1 off, kept off across the
     * voltage re-assert, back on after. */
    CHECK(charge_control_inhibit_supported(), "AXP2101 alone can switch");
    charge_control_poll(true, true);
    charge_control_set_inhibit(true);
    charge_control_poll(true, false); /* an inhibit change forces a pass */
    CHECK(!(regs[0][0x18] & 0x02), "charging off: 0x%02x", regs[0][0x18]);
    charge_control_poll(true, true);
    CHECK(!(regs[0][0x18] & 0x02), "still off after the re-assert");
    charge_control_set_inhibit(false);
    charge_control_poll(true, false);
    CHECK(regs[0][0x18] & 0x02, "charging back on");
    /* Without the inhibit the charger enable is never cleared. */
    i2c_log[0] = '\0';
    charge_control_poll(false, true);
    CHECK(strstr(i2c_log, "W34:18") == NULL, "no REG18 write: %s", i2c_log);

    /* R3 Pro II: the MP2731 follows, its other bit kept; no charging switch. */
    battery_node("battery", 90, "Discharging");
    supply_node("mp2731-charger", "USB", 1);
    put("class/power_supply/mp2731-charger/status", "Charging");
    board_desc_t r3proii = axp_board(true);
    axp_reset(&r3proii);
    regs[0][0x64] = 5;
    regs[0][0x18] = 0x03;
    regs[1][0x07] = 0xC9;
    regs[1][0x05] = 0x10;
    regs[0][0x62] = 0x12;
    charge_control_poll(true, true);
    CHECK(regs[0][0x64] == 3 && regs[1][0x07] == (0xA0 | 0x01), "4.2 V on both: %u 0x%02x", regs[0][0x64], regs[1][0x07]);
    CHECK(!charge_control_is_confirmed_off(), "MP2731 still charging");
    put("class/power_supply/mp2731-charger/status", "Full");
    battery_invalidate();
    CHECK(charge_control_is_confirmed_off(), "MP2731 full");
    charge_control_poll(false, true);
    CHECK(regs[0][0x64] == 5 && regs[1][0x07] == (0xC8 | 0x01), "stock 4.4 V: %u 0x%02x", regs[0][0x64], regs[1][0x07]);
    CHECK(!charge_control_inhibit_supported(), "no switch with an MP2731");
    charge_control_set_inhibit(true);
    charge_control_poll(false, true);
    CHECK(regs[0][0x18] & 0x02, "AXP charger left enabled");
    charge_control_set_inhibit(false);
    charge_control_safe_current_poll(true, true);
    CHECK(regs[0][0x62] == 0x0B && regs[1][0x05] == 0x04, "both capped: 0x%02x 0x%02x", regs[0][0x62], regs[1][0x05]);
    charge_control_safe_current_poll(false, true);
    CHECK(regs[0][0x62] == 0x12 && regs[1][0x05] == 0x10, "both restored");
    f = fopen(baseline_file, "r");
    line[0] = '\0';
    if (f) {
        if (!fgets(line, sizeof(line), f)) line[0] = '\0';
        fclose(f);
    }
    CHECK(strcmp(line, "version=3 board=r3proii-like axp_current=18 mp_current=16\n") == 0, "baseline file '%s'",
          line);

    /* No CAP_CHARGE_CONTROL: no traffic at all (the R3 Pro). */
    board_desc_t none = r1;
    none.caps = 0;
    axp_reset(&none);
    charge_control_poll(true, true);
    charge_control_safe_current_poll(true, true);
    CHECK(i2c_log[0] == '\0', "no capability, no I2C: %s", i2c_log);
    charger_axp2101_test_hooks(NULL, NULL);
}

int main(void) {
    test_parse();
    test_reader();
    test_events();
    test_hysteresis();
    test_stop_resume();
    test_sysfs_ops();
    test_policy();
    test_axp2101();
    power_test_setup(NULL, NULL);
    if (root[0]) {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) { /* best effort */ }
    }
    if (failures) {
        fprintf(stderr, "power: %d failure(s)\n", failures);
        return 1;
    }
    printf("power: ok\n");
    return 0;
}
