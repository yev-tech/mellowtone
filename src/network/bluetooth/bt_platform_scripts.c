/* X1600 platform (R1, R3 Pro II, R3 II 2025): the stock board scripts power
 * the chip and the radio; codec policy belongs to the audio transport
 * (bluealsa). Moved from the old bluetooth_control.c, with the hciconfig
 * calls replaced: adapter presence is read from /sys/class/bluetooth and
 * the controller reset is the HCIDEVDOWN/HCIDEVUP pair `hciconfig hci0
 * reset` itself issues. */
#include "bt_backend.h"
#include "bt_dbus.h"
#include "debug_log.h"
#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BT_INIT_TIMEOUT_MS 30000

/* <bluetooth/hci.h>, which the toolchain does not ship. */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define MT_BTPROTO_HCI 1
#define MT_HCIDEVUP _IOW('H', 201, int)
#define MT_HCIDEVDOWN _IOW('H', 202, int)

bool bt_hci_adapter_present(void) {
    DIR * d = opendir("/sys/class/bluetooth");
    if (!d) return false;
    bool found = false;
    struct dirent * e;
    while (!found && (e = readdir(d)))
        found = strncmp(e->d_name, "hci", 3) == 0 && strchr(e->d_name, ':') == NULL;
    closedir(d);
    return found;
}

/* `hciconfig hciN reset`: down, then up (EALREADY counts as up). */
static bool hci_reset(int dev) {
    int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, MT_BTPROTO_HCI);
    if (s < 0) {
        DBG_LOG("bt_scripts: HCI socket: %s\n", strerror(errno));
        return false;
    }
    bool ok = ioctl(s, MT_HCIDEVDOWN, dev) == 0;
    if (!ok) DBG_LOG("bt_scripts: hci%d down: %s\n", dev, strerror(errno));
    if (ioctl(s, MT_HCIDEVUP, dev) != 0 && errno != EALREADY) {
        DBG_LOG("bt_scripts: hci%d up: %s\n", dev, strerror(errno));
        ok = false;
    }
    close(s);
    return ok;
}

/* Uses /usr/bin/bt_resume, not /usr/bin/bt_init -- the stock hiby_player
 * binary references bt_resume, bt_enable, bt_suspend, and `bt-device -l`
 * by exact path, never bt_init/bt_done/bluealsa_profile. bt_init reliably
 * creates a duplicate dbus-daemon; bt_resume avoids that since its own
 * dbus-daemon-startup lines are commented out in the script, so it just
 * uses whatever system dbus-daemon is already running. Otherwise
 * near-identical to bt_init: same chip detection/firmware flash, plus
 * pgrep guards around starting bluetoothd/bt-agent/bluealsa so it won't
 * double-start any of those if called again while they're still up.
 * The bluealsa source daemon is ensured separately, every time (the audio
 * transport's ensure_running(), right after this). */
static bool scripts_init_chip(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    if (!bt_hci_adapter_present()) {
        char out[512];
        char * bt_resume_argv[] = { (char *) "/usr/bin/bt_resume", NULL };
        subprocess_run_timeout(bt_resume_argv, out, sizeof(out), BT_INIT_TIMEOUT_MS);
    }
    bool present = bt_hci_adapter_present();
    pthread_mutex_unlock(&bt_chip_mutex);
    return present;
}

/* /usr/bin/bt_enable -- matches hiby_player's own confirmed strings. It's
 * `bt-adapter --set Powered On` + `--set Discoverable On` together. On a
 * device where the chip is already flashed but the radio is
 * administratively down (the state the stock boot sequence leaves it in),
 * bt_enable brings hci0 fully up in about a second, no re-flash needed.
 *
 * /usr/bin/bt_disable, NOT /usr/bin/bt_suspend, despite bt_suspend being
 * the one referenced in hiby_player's strings: bt_suspend fully tears down
 * the chip's UART firmware link (kills brcm_patchram_plus/hciattach, rfkill
 * block), and re-flashing it with bt_resume afterwards reliably failed
 * ("Can't get device info: No such device") until a reboot. bt_suspend
 * belongs to the device's own sleep/wake cycle, not a user-facing switch.
 * bt_disable (`bt-adapter --set Discoverable Off` + `--set Powered Off`)
 * only touches the adapter state, the same layer bt_enable works at, so a
 * later bt_enable brings it back in ~1 s with no re-flash. */
static void scripts_power(bool on) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) (on ? "/usr/bin/bt_enable" : "/usr/bin/bt_disable"), NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* /etc/init.d/S80_bt_init always starts a second, independent dbus-daemon
 * (`--config-file=/usr/share/dbus-1/system.conf`) alongside the one
 * S30dbus already started at boot (`--system`). Depending on which of the
 * two buses a client (bluetoothd, bluealsa, bt-agent, this player) lands
 * on, they can lose sight of each other -- e.g. an incoming AVDTP connect
 * is rejected ("Authentication attempt without agent") because bt-agent
 * registered on the bus bluetoothd isn't on. Either daemon still answers
 * on its own, so counting dbus-daemon processes is what detects it. */
static int count_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;
    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        if (strstr(line, needle)) count++;
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static bool system_bus_reachable(void) {
    DBusConnection * c = bt_dbus_open();
    if (!c) return false;
    dbus_connection_close(c);
    dbus_connection_unref(c);
    return true;
}

static void ensure_single_dbus_daemon(void) {
    if (count_matching("dbus-daemon") <= 1 && system_bus_reachable()) return;

    subprocess_kill_all_matching("dbus-daemon");
    remove("/var/run/messagebus.pid"); /* stale pidfile blocks a fresh start otherwise */
    usleep(300000);

    char * argv[] = { (char *) "dbus-daemon", (char *) "--system", (char *) "--fork", NULL };
    subprocess_spawn_daemon(argv);
    usleep(300000);
}

/* bluetoothd can end up answering nothing useful over D-Bus while hci0 is
 * still up (seen once real A2DP audio flowed in DAC mode): same PID, still
 * sleeping. The only recovery that works is restarting it, mirroring
 * /usr/bin/bt_resume's own remediation: stop bluetoothd, reset hci0, start
 * bluetoothd again. Rate-limited so a wedge that recovery does not fix
 * cannot restart it on every status poll. bluetooth_control.c calls this
 * and then lets the audio transport restore its daemons. */
#define WEDGED_RECOVERY_COOLDOWN_SECONDS 20

static bool scripts_recover_stack(void) {
    static time_t last_recovery_attempt = 0;
    time_t now = time(NULL);
    if (now - last_recovery_attempt < WEDGED_RECOVERY_COOLDOWN_SECONDS) {
        DBG_LOG("bt_scripts: wedged daemon detected, but recovery on cooldown (%lds left)\n",
                (long) (WEDGED_RECOVERY_COOLDOWN_SECONDS - (now - last_recovery_attempt)));
        return false;
    }
    last_recovery_attempt = now;
    DBG_LOG("bt_scripts: wedged daemon detected, attempting recovery\n");

    ensure_single_dbus_daemon();

    subprocess_kill_all_matching("bluetoothd");
    usleep(500000);

    bool reset_ok = hci_reset(0);
    usleep(500000);

    char * bluetoothd_argv[] = { (char *) "/usr/libexec/bluetooth/bluetoothd", (char *) "-E", (char *) "-C", NULL };
    bool spawn_ok = subprocess_spawn_daemon(bluetoothd_argv);
    usleep(500000); /* give it a moment to register the adapter before the next step touches it */

    /* A restarted bluetoothd comes up with the adapter off; this only runs
     * because Bluetooth was expected to be on. */
    bt_control_enable();

    DBG_LOG("bt_scripts: recovery attempt done (hci0 reset=%d, bluetoothd spawn=%d)\n", reset_ok, spawn_ok);
    (void) reset_ok;
    (void) spawn_ok;
    return true;
}

const struct board_bt_platform_ops bt_platform_scripts_ops = {
    .name = "board-scripts",
    .init_chip = scripts_init_chip,
    .power = scripts_power,
    .codec_policy = NULL, /* bluealsa's */
    .recover_stack = scripts_recover_stack,
};

/* ---- no platform (host simulator): the stack's own Powered property ---------- */

static bool none_init_chip(void) {
    return true;
}

const struct board_bt_platform_ops bt_platform_none_ops = {
    .name = "none",
    .init_chip = none_init_chip,
};
