#include "board.h"
#include "board_config.h"
#include "power_suspend.h"
#include "battery.h"
#include "power_events.h"
#include "bluetooth_control.h"
#include "db_log.h"
#include "subprocess.h"
#include "wifi_control.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* BOARD_POWER_BT_RESUME_SCRIPT: the stock scripts and the marker the stock
 * bt_resume (like bt_init) writes once the chip and daemons are up. */
#define BT_SUSPEND_SCRIPT "/usr/bin/bt_suspend"
#define BT_RESUME_SCRIPT "/usr/bin/bt_resume"
#define BT_INIT_OK_MARKER "/tmp/bt_init_ok"
/* After Wi-Fi is powered down, before the sleep: the bcmdhd driver's own
 * delayed work ran about 2 s after it (see power_suspend_now()). */
#define WIFI_OFF_SETTLE_US (2500 * 1000)

/* bt_resume sleeps about 6 s on its own (chip power, firmware, daemons). */
#define BT_RESUME_TIMEOUT_MS 30000

static atomic_bool suspend_allowed_override = false;

void power_suspend_set_allowed(bool allowed) {
    atomic_store(&suspend_allowed_override, allowed);
}

bool power_suspend_available(void) {
    return !(BOARD.power.quirks & BOARD_POWER_NO_SUSPEND) || atomic_load(&suspend_allowed_override);
}

/* Monotonic timestamp in milliseconds. Uses CLOCK_MONOTONIC to align with
 * hw_buttons timestamps on the same timeline. */
static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Persist phase markers only when the user has enabled the runtime
 * diagnostic log. Suspend is infrequent, so syncing each marker is a small
 * bounded cost and leaves the last reached stage on storage after a hang. */
#define SUSPEND_LOG(fmt, ...) \
    do { \
        if (db_log_enabled()) { \
            db_log("power_suspend", fmt, ##__VA_ARGS__); \
            (void) db_log_flush(); \
        } \
    } while (0)

static void write_sysfs(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

/* Writes a sysfs value and reports success/failure.
 * Used for /sys/power/state to detect if the write itself failed. */
static bool write_sysfs_checked(const char * path, const char * value, int * error_out) {
    int write_error = 0;
    FILE * f = fopen(path, "w");
    if (!f) {
        write_error = errno;
        if (error_out) *error_out = write_error;
        return false;
    }
    bool ok = fprintf(f, "%s", value) >= 0;
    if (!ok) write_error = errno ? errno : EIO;
    if (fclose(f) != 0 && ok) {
        write_error = errno ? errno : EIO;
        ok = false;
    }
    if (error_out) *error_out = write_error;
    return ok;
}

/* CLOCK_MONOTONIC does not advance across Linux suspend-to-RAM.
 * CLOCK_BOOTTIME is used to measure elapsed time across suspend. */
static clockid_t suspend_duration_clock_id(void) {
    static clockid_t cached = CLOCK_MONOTONIC;
    static bool probed = false;
    if (!probed) {
        struct timespec ts;
        cached = (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) ? CLOCK_BOOTTIME : CLOCK_MONOTONIC;
        probed = true;
    }
    return cached;
}

static uint32_t suspend_duration_ms_now(void) {
    struct timespec ts;
    clock_gettime(suspend_duration_clock_id(), &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Logs elapsed suspend time and the last wake IRQ when runtime logging is
 * enabled. */
static void log_suspend_diagnostics(uint32_t slept_ms, bool suspend_write_ok, int write_errno) {
    static unsigned int suspend_cycle_count;
    ++suspend_cycle_count;
    char wakeup_irq[32] = "";
    FILE * f = fopen("/sys/power/pm_wakeup_irq", "r");
    if (f) {
        if (!fgets(wakeup_irq, sizeof(wakeup_irq), f)) wakeup_irq[0] = '\0';
        fclose(f);
    }
    size_t irq_len = strlen(wakeup_irq);
    if (irq_len > 0 && wakeup_irq[irq_len - 1] == '\n') wakeup_irq[irq_len - 1] = '\0';
    SUSPEND_LOG("cycle=%u returned slept_ms=%u write_ok=%d errno=%d wake_irq=%s",
                suspend_cycle_count, slept_ms, suspend_write_ok, write_errno,
                wakeup_irq[0] ? wakeup_irq : "(unavailable)");
}

static void radio_restore_perform(bool wifi_was_on, bool bt_was_on) {
    if (BOARD.power.quirks & BOARD_POWER_BT_RESUME_SCRIPT) {
        /* Wi-Fi first (about 3 s), then the stock bt_resume (dbus-daemon,
         * chip power, firmware, bluetoothd, bt-agent, bt-monitor, bt-media;
         * about 10 s of fixed sleeps; it leaves the adapter powered off),
         * then Bluetooth on again, which also reconnects the last device.
         * The stock player starts Wi-Fi only after bt_resume, so it came
         * back ~13 s after a wake. The marker goes first so
         * bt_control_init_chip() waits for this bt_resume. */
        unlink(BT_INIT_OK_MARKER);
        if (wifi_was_on) {
            SUSPEND_LOG("wifi_enable begin");
            wifi_control_enable();
            SUSPEND_LOG("wifi_enable done");
        }
        char * bt_resume_argv[] = { (char *) BT_RESUME_SCRIPT, NULL };
        SUSPEND_LOG("bt_resume begin");
        subprocess_run_timeout(bt_resume_argv, NULL, 0, BT_RESUME_TIMEOUT_MS);
        SUSPEND_LOG("bt_resume done marker=%d", access(BT_INIT_OK_MARKER, F_OK) == 0);
        if (bt_was_on && bt_control_init_chip()) bt_control_enable();
        return;
    }
    if (bt_was_on) {
        bt_control_init_chip();
        bt_control_enable();
    }
    if (wifi_was_on) wifi_control_enable();
}

/* Restores Bluetooth and Wi-Fi in a detached thread after resume so that
 * the caller and display unblank are not blocked by radio re-initialization. */
static void * radio_restore_thread_func(void * arg) {
    uintptr_t flags = (uintptr_t) arg;
    bool wifi_was_on = flags & 1;
    bool bt_was_on = flags & 2;
    radio_restore_perform(wifi_was_on, bt_was_on);
    return NULL;
}

bool power_suspend_now(void) {
    if (!power_suspend_available()) return false;
    /* Recheck at the suspend boundary, independently of the UI idle gate.
     * An uncertain sample must never authorize sleeping on external power. */
    if (battery_get_external_power_state() != BATTERY_EXTERNAL_POWER_DISCONNECTED) {
        SUSPEND_LOG("blocked: external power connected or unknown");
        return false;
    }
    const bool diagnostics_enabled = db_log_enabled();
    bool wifi_was_on = wifi_control_is_enabled();
    bool bt_was_on = bt_control_is_powered();
    SUSPEND_LOG("begin wifi_was_on=%d bt_was_on=%d", wifi_was_on, bt_was_on);

    /* Wi-Fi and Bluetooth share one Broadcom chip (SDIO + UART). A
     * Bluetooth command within ~2 s after the Wi-Fi side powered down was
     * followed (70 ms later) by an SDIO command to the dead chip (jzmmc
     * "command response error"); the Wi-Fi bring-up after the wake then hung
     * claiming the SDIO host (ifconfig wlan0 up in sdio_reset_comm), in the
     * stock player's order (Bluetooth off, Wi-Fi off, bt_suspend) as well.
     * So Bluetooth goes down completely first, bt_suspend included, then
     * Wi-Fi, and the Wi-Fi driver gets time to settle before the sleep. */
    if (bt_was_on) {
        /* Cleanly through D-Bus before the raw bt_suspend, so active links
         * are disconnected gracefully. Power-save only: the saved choice
         * for the next boot stays "on". */
        SUSPEND_LOG("bluetooth_disable begin");
        bt_control_set_power_persist(false);
        bt_control_disable();
        bt_control_set_power_persist(true);
    }

    char * bt_suspend_argv[] = { (char *) BT_SUSPEND_SCRIPT, NULL };
    SUSPEND_LOG("bt_suspend begin");
    subprocess_run(bt_suspend_argv, NULL, 0);

    if (wifi_was_on) {
        SUSPEND_LOG("wifi_disable begin");
        /* Power-save only, as for Bluetooth. */
        bool persist = wifi_control_set_power_persist(false);
        wifi_control_disable();
        wifi_control_set_power_persist(persist);
        usleep(WIFI_OFF_SETTLE_US);
    }

    bool slept = false;
    SUSPEND_LOG("framebuffer_blank begin");
    if (!BOARD.screen.no_fb_blank) write_sysfs("/sys/class/graphics/fb0/blank", "4"); /* FB_BLANK_POWERDOWN; never on R3 Pro (see backlight.c) */

    /* USB can be inserted while radio shutdown is in progress. In that
     * case unwind the preparation below without writing the sleep request. */
    if (battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED) {
        SUSPEND_LOG("mem_write begin monotonic_ms=%u", monotonic_ms());
        uint32_t sleep_start_ms = diagnostics_enabled ? suspend_duration_ms_now() : 0;
        int suspend_write_errno = 0;
        bool suspend_write_ok = write_sysfs_checked("/sys/power/state", "mem", &suspend_write_errno);
        slept = suspend_write_ok;
        if (diagnostics_enabled) {
            log_suspend_diagnostics(suspend_duration_ms_now() - sleep_start_ms, suspend_write_ok, suspend_write_errno);
        }
    } else {
        SUSPEND_LOG("cancelled: external power connected or unknown after preparation");
    }

    if (!BOARD.screen.no_fb_blank) write_sysfs("/sys/class/graphics/fb0/blank", "0"); /* FB_BLANK_UNBLANK; never on R3 Pro (see backlight.c) */
    /* The battery may have changed a lot while asleep, and the fallback
     * poll's clock (CLOCK_MONOTONIC) stood still: look again now. */
    power_events_notify(POWER_EVT_RESUME);

    if (bt_was_on || wifi_was_on || (BOARD.power.quirks & BOARD_POWER_BT_RESUME_SCRIPT)) {
        uintptr_t flags = (wifi_was_on ? 1u : 0u) | (bt_was_on ? 2u : 0u);
        pthread_t restore_thread;
        if (pthread_create(&restore_thread, NULL, radio_restore_thread_func, (void *)(uintptr_t) flags) == 0) {
            pthread_detach(restore_thread);
        } else {
            radio_restore_perform(wifi_was_on, bt_was_on);
        }
    }
    return slept;
}
