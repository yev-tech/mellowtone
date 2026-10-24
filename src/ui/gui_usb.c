/* USB: the USB Mode screen, the per-connection "Connected to a computer"
 * prompt, card lending (the shared-card path on X1600 boards and the lend
 * path, usb_storage_lend.h) with its storage screen, the USB DAC overlay and
 * leave popup, and the USB polls (mode switch, bridge restart, hotplug).
 * Part of the network module (gui_network.c). */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_settings.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "audio.h"
#include "plugin_manager.h"
#include "usb_mode_control.h"
#include "usb_connection.h"
#include "battery.h"
#include "backlight.h"
#include "usb_storage_session.h"
#include "usb_dac_bridge.h"
#include "usb_gadget.h"
#include "usb_storage_lend.h"
#include "storage_paths.h"
#include "db_log.h"
#include "gui_library.h"
#include "airplay_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* Screens owned by this module */
static lv_obj_t * usb_mode_screen;
static lv_obj_t * usb_dac_overlay_screen;
/* See the mode-switch success path for why this exists rather than reading
 * usb_mode_active(). */
static bool usb_dac_mode_active = false;
static lv_obj_t * usb_dac_hint_label;
static lv_obj_t * usb_dac_input_label;
static lv_obj_t * usb_dac_path_label;
/* "Connected as USB storage" (boards that lend the card, usb_storage_lend.h). */
static lv_obj_t * usb_storage_screen;
static lv_obj_t * usb_storage_status_label;
static lv_obj_t * usb_storage_button;
static int usb_storage_nav_slot = -1;

/* ---- USB device mode (Settings > USB Mode, and the "Connected to a
 * computer" prompt) -- single-select list with an accent-colored border on
 * the active option. Charge only until the user picks Storage or USB DAC
 * for the connection (usb_connection.h). Applying a mode modifies
 * configfs/sysfs in a worker thread polled from update_timer_cb to prevent
 * blocking the UI thread. ---- */

typedef struct {
    usb_mode_t mode;
    const char * label;
} usb_mode_option_t;

static const usb_mode_option_t usb_mode_options[] = {
    { USB_MODE_CHARGE, N_("Charge only") },
    { USB_MODE_STORAGE, N_("Storage") },
    { USB_MODE_DAC, N_("USB DAC") },
    { USB_MODE_ADB, N_("ADB") },
};
#define USB_MODE_OPTION_COUNT (sizeof(usb_mode_options) / sizeof(usb_mode_options[0]))

static lv_obj_t * usb_mode_list;

static pthread_t usb_mode_switch_thread;
static bool usb_mode_switch_active = false;
static bool usb_mode_switch_queued = false;
/* UI-owned guard; the detached worker only publishes completion. It may
 * wait indefinitely for an old bridge writer, so never join it on the UI. */
static bool usb_bridge_restart_active = false;
static atomic_bool usb_bridge_restart_done = false;
/* Restarts of a bridge that keeps stopping are spaced out: 1 s, doubling up
 * to 8 s, back to 1 s once a restarted bridge streams or keeps running. */
#define USB_BRIDGE_RESTART_MIN_MS 1000
#define USB_BRIDGE_RESTART_MAX_MS 8000
#define USB_BRIDGE_STABLE_MS 3000
static uint32_t usb_bridge_restart_delay_ms = USB_BRIDGE_RESTART_MIN_MS;
static uint32_t usb_bridge_restart_tick;
static bool usb_bridge_restarted;
static atomic_bool usb_mode_switch_done_flag = false;
static volatile bool usb_mode_switch_succeeded = false;
static usb_mode_t usb_mode_switch_target;
static bool usb_cable_state_initialized;
static bool usb_cable_was_connected;
/* The connection (usb_connection.h) and what it needs from the port: the
 * charge-only gadget bound on each plug-in where the board has to bind one
 * (configfs), and whether that worked -- without it a computer cannot be
 * told from a charger, so the modes stay choosable while the cable is in. */
static usb_conn_t usb_conn;
static bool usb_charge_bind_pending;
static bool usb_charge_unverified;
static bool usb_conn_host_last;
static uint32_t usb_conn_host_check_tick;
static bool usb_mode_choosable_shown;
/* The "Connected to a computer" prompt: due (shown once the screen is on
 * and nothing else is in the way) and the popup itself. */
static bool usb_prompt_pending;
static gui_popup_t usb_prompt_popup;
static void usb_dac_overlay_dismiss(void);
static usb_storage_session_t usb_storage_session;
static void usb_storage_session_ended(void);
static void poll_usb_shared_card(void);
static bool usb_shared_card_ending(void);
static uint32_t usb_storage_host_check_tick;

static void usb_mode_option_row_cb(lv_event_t * e);
/* ADB is a separate toggle, not a third equal option alongside Storage/DAC
 * -- matches the stock firmware's own USB device mode screen (confirmed by
 * the user against a real stock device: ADB overrides Storage/DAC while
 * on, turning it off falls back to the other two). This also matches the
 * actual gadget architecture: ADB lives under its own "adb_demo" configfs
 * directory, entirely separate from the "android0" one Storage and DAC
 * share (see usb_mode_control.c), so it really is orthogonal rather than a
 * third position in the same rotation. */
static bool adb_owns_usb_port(void);

static bool usb_storage_offered(void) { return board_has(CAP_USB_STORAGE) && !board_impl_missing(CAP_USB_STORAGE); }
static bool usb_dac_offered(void) { return board_has(CAP_USB_DAC) && !board_impl_missing(CAP_USB_DAC); }

static bool usb_mode_offered(usb_mode_t mode) {
    switch (mode) {
        case USB_MODE_CHARGE: return true;
        case USB_MODE_STORAGE: return usb_storage_offered();
        case USB_MODE_DAC: return usb_dac_offered();
        case USB_MODE_ADB: return false; /* Developer Options */
    }
    return false;
}

/* Storage and USB DAC are chosen per connection, so only with a computer on
 * the cable -- or with any cable when the board could not tell (see
 * usb_charge_unverified). */
static bool usb_mode_choosable(void) {
    return usb_conn_computer(&usb_conn) || (usb_charge_unverified && usb_conn_cable(&usb_conn));
}

static void populate_usb_mode_screen(void) {
    lv_obj_clean(usb_mode_list);

    /* ADB itself is toggled from Developer Options, not here, but it still
     * owns the port while on, so this screen keeps showing that the other
     * modes are unavailable. */
    bool adb_active = adb_owns_usb_port();
    bool choosable = !adb_active && usb_mode_choosable();
    usb_mode_choosable_shown = usb_mode_choosable();

    for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
        if (!usb_mode_offered(usb_mode_options[i].mode)) continue; /* ADB: handled in Developer Options */

        bool selected = !adb_active && usb_mode_active() == usb_mode_options[i].mode;
        lv_obj_t * row = add_pill_option_row(usb_mode_list, TR(usb_mode_options[i].label),
                                            selected, usb_mode_option_row_cb, (void *) (intptr_t) i);

        if (!choosable) {
            /* Not meaningful choices while ADB owns the USB port or no
             * computer is on the cable -- dimmed and inert rather than
             * removed, so the rows stay put (no layout jump) and it's
             * visually clear these exist but can't be picked now. */
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        }
    }
    if (!choosable) {
        lv_obj_t * hint = lv_label_create(usb_mode_list);
        lv_label_set_text(hint, adb_active ? TR("Turn off ADB in Developer Options to use these modes")
                                           : TR("Connect to a computer to choose a USB mode"));
        lv_obj_add_style(hint, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_top(hint, 12, 0);
    }
}

static void * usb_mode_switch_thread_func(void * arg) {
    (void) arg;
    usb_mode_switch_succeeded = usb_mode_control_apply(usb_mode_switch_target);
    atomic_store_explicit(&usb_mode_switch_done_flag, true, memory_order_release); /* written last -- poll_usb_mode_switch only checks this flag */
    return NULL;
}

static void * usb_bridge_restart_thread_func(void * arg) {
    (void) arg;
    usb_dac_bridge_start();
    atomic_store_explicit(&usb_bridge_restart_done, true, memory_order_release);
    return NULL;
}

/* Returns whether the worker actually started. A launch that fails runs no
 * poll, so nothing else will ever correct a control left flipped in
 * anticipation of it -- the caller has to know. */
static bool launch_usb_mode_switch(void) {
    usb_mode_switch_queued = false;
    atomic_store_explicit(&usb_mode_switch_done_flag, false, memory_order_relaxed);
    int rc = pthread_create(&usb_mode_switch_thread, NULL, usb_mode_switch_thread_func, NULL);
    if (rc != 0) {
        /* Without this rollback, a transient low-memory/thread-creation
         * failure leaves the guard latched forever: every later tap is
         * ignored until the player or device is restarted. */
        usb_mode_switch_active = false;
        show_error_toast(TR("Could not start USB mode switch"));
        return false;
    }
    return true;
}

/* Returns whether the request was accepted. A caller that flips its own UI
 * ahead of the hardware needs to know when a tap was dropped, or the control
 * ends up showing a state nothing is working toward. */
bool start_usb_mode_switch(usb_mode_t target) {
    /* Not while the card is lent: the port belongs to the storage session
     * until the card is back (usb_storage_lend.h); nor while a shared card
     * is being taken back, which Storage would export again mid-way. */
    if (usb_mode_switch_active || usb_storage_lend_busy() || usb_shared_card_ending()) return false;
    gui_library_suspend_boot_prompt();
    usb_mode_switch_target = target;
    usb_mode_switch_active = true;
    /* Preserve the explicit request without letting gadget teardown race a
     * watchdog start already in flight. Polling launches it once safe. */
    usb_mode_switch_queued = usb_bridge_restart_active;
    if (!usb_mode_switch_queued) return launch_usb_mode_switch();
    return true;
}

/* The state the ADB control should show: the target while a switch is in
 * flight, otherwise the recorded mode. */
bool gui_network_adb_active(void) {
    if (usb_mode_switch_active) return usb_mode_switch_target == USB_MODE_ADB;
    return usb_mode_active() == USB_MODE_ADB;
}

/* Whether ADB still has a claim on the port, which is deliberately more
 * cautious than gui_network_adb_active(). Leaving ADB does not free the port
 * until the switch actually lands, so the modes that cannot be chosen yet stay
 * dimmed for the whole transition in both directions rather than offering rows
 * whose taps the in-flight guard would only reject. */
static bool adb_owns_usb_port(void) {
    bool recorded_adb = usb_mode_active() == USB_MODE_ADB;
    if (usb_mode_switch_active) return recorded_adb || usb_mode_switch_target == USB_MODE_ADB;
    return recorded_adb;
}

void poll_usb_mode_switch(void) {
    if (usb_bridge_restart_active &&
        atomic_load_explicit(&usb_bridge_restart_done, memory_order_acquire)) {
        usb_bridge_restart_active = false;
    }
    if (usb_mode_switch_queued && !usb_bridge_restart_active) {
        /* Deferred launch: same failure consequence as the immediate one, but
         * the original caller has long since returned, so the control it
         * flipped has to be corrected from here. */
        if (!launch_usb_mode_switch()) gui_settings_sync_adb_toggle();
    }
    /* If set to USB DAC mode but the audio bridge is not running, restart it.
     * Skipped while a mode switch is in flight to avoid racing
     * usb_mode_control_apply(). */
    if (!usb_mode_switch_active && !usb_bridge_restart_active &&
        usb_mode_active() == USB_MODE_DAC) {
        usb_dac_stream_info_t info;
        usb_dac_bridge_get_stream_info(&info);
        if (info.bridge_running && usb_bridge_restarted &&
            (info.streaming || lv_tick_elaps(usb_bridge_restart_tick) >= USB_BRIDGE_STABLE_MS)) {
            usb_bridge_restarted = false;
            usb_bridge_restart_delay_ms = USB_BRIDGE_RESTART_MIN_MS;
        }
        if (!info.bridge_running &&
            (!usb_bridge_restarted || lv_tick_elaps(usb_bridge_restart_tick) >= usb_bridge_restart_delay_ms)) {
            if (usb_bridge_restarted) {
                usb_bridge_restart_delay_ms *= 2;
                if (usb_bridge_restart_delay_ms > USB_BRIDGE_RESTART_MAX_MS)
                    usb_bridge_restart_delay_ms = USB_BRIDGE_RESTART_MAX_MS;
            }
            usb_bridge_restarted = true;
            usb_bridge_restart_tick = lv_tick_get();
            pthread_attr_t attr;
            if (pthread_attr_init(&attr) == 0) {
                if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0) {
                    pthread_t thread;
                    atomic_store_explicit(&usb_bridge_restart_done, false, memory_order_relaxed);
                    usb_bridge_restart_active = true;
                    if (pthread_create(&thread, &attr, usb_bridge_restart_thread_func, NULL) != 0)
                        usb_bridge_restart_active = false;
                }
                pthread_attr_destroy(&attr);
            }
        }
    }

    if (!usb_mode_switch_active || usb_mode_switch_queued || !atomic_load_explicit(&usb_mode_switch_done_flag, memory_order_acquire)) return;
    usb_mode_switch_active = false;
    pthread_join(usb_mode_switch_thread, NULL);

    /* usb_mode_control_apply() only reports success once the target
     * gadget is actually verified bound, not just "the script ran" -- on
     * failure the active mode is deliberately left untouched (the switch
     * genuinely didn't happen) rather than optimistically recording what
     * was merely requested. */
    if (usb_mode_switch_target == USB_MODE_CHARGE) usb_charge_unverified = !usb_mode_switch_succeeded;
    if (!usb_mode_switch_succeeded) {
        const char * label = TR("USB mode");
        for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
            if (usb_mode_options[i].mode == usb_mode_switch_target) { label = TR(usb_mode_options[i].label); break; }
        }
        char msg[128];
        snprintf(msg, sizeof(msg), TR("Failed to switch to %s"), label);
        show_error_toast(msg);
        /* apply() tears the old gadget down before bringing the target up, so
         * a failure can leave neither bound -- the recorded mode is then not
         * merely unchanged, it is wrong. Re-read the hardware so the rollback
         * below reflects what is actually there. */
        usb_mode_t actual_usb_mode;
        if (usb_mode_control_detect_current(&actual_usb_mode)) usb_mode_set_active(actual_usb_mode);
        if (usb_dac_mode_active && usb_mode_active() != USB_MODE_DAC) {
            usb_dac_mode_active = false;
            usb_dac_overlay_dismiss();
        }
        populate_usb_mode_screen(); /* in case the actual mode's row needs re-highlighting */
        gui_settings_sync_adb_toggle(); /* roll the optimistic flip back to what the hardware actually did */
        return;
    }

    /* Leaving Storage while the PC is still attached ends its access too;
     * the cable staying in no longer means a session. */
    usb_mode_t previous = usb_mode_active();
    if (previous == USB_MODE_STORAGE && usb_mode_switch_target != USB_MODE_STORAGE &&
        usb_storage_session.host_seen) {
        usb_storage_session.host_seen = false;
        usb_storage_session_ended();
    }
    fprintf(stderr, "usb_mode: %s -> %s\n", usb_mode_name(previous), usb_mode_name(usb_mode_switch_target));
    usb_mode_set_active(usb_mode_switch_target);
    /* ADB alone is remembered (Developer Options); every other mode lasts
     * one connection. */
    if ((usb_mode_switch_target == USB_MODE_ADB) != current_settings.adb_enabled) {
        current_settings.adb_enabled = usb_mode_switch_target == USB_MODE_ADB;
        settings_save(&current_settings);
    }
    /* Process-lifetime truth for "the DAC overlay owns the UI", as opposed to
     * usb_mode_active(), which can read DAC after the player restarted
     * mid-connection without the overlay ever being shown. Set here rather
     * than next to the nav_push below so leaving DAC mode -- from the
     * overlay, Settings or an unplug -- clears it just as reliably. */
    bool leaving_dac = usb_dac_mode_active && usb_mode_switch_target != USB_MODE_DAC;
    usb_dac_mode_active = (usb_mode_switch_target == USB_MODE_DAC);
    if (leaving_dac) usb_dac_overlay_dismiss();
    populate_usb_mode_screen(); /* refresh which row shows the accent border */
    gui_settings_sync_adb_toggle();

    /* DAC mode takes over the whole screen with its own overlay (matching
     * the stock firmware's hiby_usb.view) -- the device can't sensibly be
     * used as a normal music player while it's presenting itself to a PC
     * as a USB sound card. Storage/ADB have no such conflict (the app
     * keeps running normally), so they don't get an overlay. */
    if (usb_mode_switch_target == USB_MODE_DAC) {
        /* Same three-way mutual exclusion as bt_dac_toggle_cb()/
         * airplay_toggle_cb() -- all three (local playback, Bluetooth DAC,
         * AirPlay) fight over the same hw:0,0 output. usb_dac_bridge_start()
         * (called from usb_mode_control_apply(), already run on the
         * background thread by this point) already called audio_stop()
         * itself, but that only stops the backend -- the play button icon
         * needs its own explicit refresh, same as every other call site
         * that stops playback out from under the UI. */
        if (audio_is_playing() || audio_is_paused()) {
            audio_stop();
            set_play_button_state(false);
            plugin_manager_notify_stopped();
        }
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
        if (current_settings.wifi_dac_mode_enabled) {
            current_settings.wifi_dac_mode_enabled = false;
            settings_save(&current_settings);
            airplay_control_stop();
        }
        nav_push(usb_dac_overlay_screen);
    }
}

/* ---- USB storage on boards that share the mounted card (X1600) ----------
 * The card stays mounted while a computer has it, so from the moment a
 * computer configures the port nothing here may write it: the writers keep
 * their saves in memory (mt_state_set_card_lent(), which also moves the logs
 * to RAM), a running scan stops, the debug logs close, and the back end
 * remounts the card read-only (storage_export_shared_ops) so nothing else
 * can write it either. Music keeps playing from it. When the session ends
 * the player lets go of the card as for a removal (playback stops, keeping
 * its resume point), the card is unmounted -- never lazily -- and mounted
 * again so nothing cached from before is written back over what the
 * computer changed; then the saves kept in memory are written and the
 * library reloads. */
typedef enum {
    USB_SHARED_IDLE = 0,
    USB_SHARED_ACTIVE, /* a computer has the card */
    USB_SHARED_ENDING, /* closing everything, then unmount and mount */
} usb_shared_state_t;
static usb_shared_state_t usb_shared_state;
static bool usb_shared_read_only;
static uint32_t usb_shared_try_tick;
static uint32_t usb_shared_end_started;
#define USB_SHARED_RETRY_MS 1000
#define USB_SHARED_END_TIMEOUT_MS 15000

#ifndef HOST_BUILD
extern void boot_checkpoint_release(void);
extern void mount_sd_card_if_needed(void);
#endif

static bool usb_shared_card_ending(void) { return usb_shared_state == USB_SHARED_ENDING; }

static void usb_shared_card_begin(void) {
    fprintf(stderr, "usb_storage: a computer has the card; the player stops writing it\n");
    usb_shared_state = USB_SHARED_ACTIVE;
    usb_shared_read_only = false;
    usb_shared_try_tick = lv_tick_get() - USB_SHARED_RETRY_MS;
    gui_library_cancel_scan();
    /* Writers pause first, so nothing reopens a log once it is closed. */
    mt_state_set_card_lent(true);
    db_log_release_file();
    usb_dac_bridge_set_debug_log_enabled(false); /* closes its log; reopens when logging */
#ifndef HOST_BUILD
    boot_checkpoint_release();
#endif
}

/* A computer's storage session on a board that shares the mounted card
 * ended (cable out or mode changed): take the card back. */
static void usb_storage_session_ended(void) {
    if (usb_shared_state != USB_SHARED_ACTIVE) return;
    usb_shared_state = USB_SHARED_ENDING;
    usb_shared_end_started = lv_tick_get();
    usb_shared_try_tick = lv_tick_get() - USB_SHARED_RETRY_MS;
}

static void usb_shared_card_finish(bool released) {
    usb_shared_state = USB_SHARED_IDLE;
    if (released) {
#ifndef HOST_BUILD
        mount_sd_card_if_needed();
#endif
    } else if (!usb_shared_read_only) {
        /* Still mounted, writable, with what it cached before the computer
         * wrote: writing now could corrupt the card. Writers stay paused. */
        fprintf(stderr, "usb_storage: the card could not be unmounted; writes stay paused\n");
        return;
    } else {
        /* Still mounted read-only: the card state says so, nothing writes. */
        fprintf(stderr, "usb_storage: the card could not be unmounted; it stays read-only\n");
    }
    mt_state_set_card_lent(false);
    gui_library_sd_lend_finish(!usb_dac_mode_active);
}

static void poll_usb_shared_card(void) {
    const struct board_storage_export_ops * ops = usb_storage_export_ops();
    switch (usb_shared_state) {
        case USB_SHARED_IDLE:
            if (gui_network_usb_storage_session_active()) usb_shared_card_begin();
            return;
        case USB_SHARED_ACTIVE:
            if (usb_shared_read_only || lv_tick_elaps(usb_shared_try_tick) < USB_SHARED_RETRY_MS) return;
            usb_shared_try_tick = lv_tick_get();
            usb_shared_read_only = !ops || !ops->session_started || ops->session_started();
            return;
        case USB_SHARED_ENDING:
            break;
    }
    /* Plugged straight back in: the computer has the card again. */
    if (gui_network_usb_storage_session_active()) {
        usb_shared_state = USB_SHARED_ACTIVE;
        return;
    }
    /* The library first: a running scan has to stop before the database
     * can close. */
    if (!gui_library_sd_lend_prepare() && lv_tick_elaps(usb_shared_end_started) < USB_SHARED_END_TIMEOUT_MS) return;
    if (audio_is_playing() || audio_is_paused()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    if (lv_tick_elaps(usb_shared_try_tick) < USB_SHARED_RETRY_MS) return;
    usb_shared_try_tick = lv_tick_get();
    bool released = !ops || !ops->session_ended || ops->session_ended();
    if (released || lv_tick_elaps(usb_shared_end_started) >= USB_SHARED_END_TIMEOUT_MS)
        usb_shared_card_finish(released);
}

/* ---- USB storage on boards that lend the card (usb_storage_lend.h) ------
 * The card is unmounted while the computer has it: everything on it is
 * closed first, a full-screen "Connected as USB storage" screen stays up
 * meanwhile, and the library and queue come back afterwards. */
static bool usb_storage_closing;
static uint32_t usb_storage_close_started;
/* The other process the busy check found holding the card, and the message
 * waiting to be shown after a lend that did not happen. */
static char usb_storage_busy_name[32];
static char usb_storage_notice[160];
#define USB_STORAGE_CLOSE_TIMEOUT_MS 15000

static void usb_storage_disconnect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    usb_storage_lend_user_disconnect();
}

static void usb_storage_screen_update(void) {
    if (!usb_storage_screen) return;
    const char * text;
    bool button = false;
    switch (usb_storage_lend_state()) {
        case USB_LEND_LENT:
            text = TR("Eject the card on the computer before unplugging, or tap Disconnect.");
            button = true;
            break;
        case USB_LEND_RECLAIMING:
        case USB_LEND_REOPENING:
            text = TR("Getting the card back…");
            break;
        default:
            text = TR("Preparing USB storage…");
            break;
    }
    set_label_if_changed(usb_storage_status_label, text);
    if (button) lv_obj_remove_state(usb_storage_button, LV_STATE_DISABLED);
    else lv_obj_add_state(usb_storage_button, LV_STATE_DISABLED);
}

static void usb_storage_show(bool on) {
    if (!usb_storage_screen) return;
    if (on) {
        /* Closing the library resets the navigation to Home, taking this
         * screen with it: it is pushed again then. */
        if (!gui_navigation_contains(usb_storage_screen)) {
            usb_storage_nav_slot = gui_navigation_get_depth(); /* this screen's own slot, about to be occupied */
            nav_push(usb_storage_screen);
        }
        usb_storage_screen_update();
        return;
    }
    if (lv_screen_active() == usb_storage_screen) {
        nav_pop();
    } else if (usb_storage_nav_slot >= 0 && usb_storage_nav_slot < gui_navigation_get_depth() &&
               gui_navigation_get_screen_at(usb_storage_nav_slot) == usb_storage_screen) {
        nav_remove_stack_slot(usb_storage_nav_slot);
    }
    usb_storage_nav_slot = -1;
}

/* The USB Mode screen and setting follow what the port really does after a
 * lend that did not happen. */
static void usb_mode_show_actual(void) {
    usb_mode_t actual;
    if (usb_mode_control_detect_current(&actual)) usb_mode_set_active(actual);
    if (usb_mode_list) populate_usb_mode_screen();
    gui_settings_sync_adb_toggle();
}

/* Order: playback and every writer stop, everything this player has on the
 * card closes (stdout/stderr move to RAM), and only then the busy check,
 * which can then only find other processes. */
static usb_lend_result_t usb_storage_close_all(void) {
    if (!usb_storage_closing) {
        usb_storage_closing = true;
        usb_storage_close_started = lv_tick_get();
        usb_storage_busy_name[0] = '\0';
        usb_storage_notice[0] = '\0';
        usb_storage_show(true);
        /* Written now, back after the lend: the queue with its position and
         * the settings (while lent they stay in memory). */
        gui_player_queue_flush();
        settings_save(&current_settings);
    }
    bool timed_out = lv_tick_elaps(usb_storage_close_started) > USB_STORAGE_CLOSE_TIMEOUT_MS;
    /* The track playing is a file on the card. */
    if (audio_is_playing() || audio_is_paused()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    /* The library: a running scan and the other workers on the card stop
     * before the database, the queue and the file browser let go of it;
     * then the playback thread closes its files. */
    bool closed = gui_library_sd_lend_prepare() && audio_files_closed();
    usb_storage_show(true); /* again if closing the library went back to Home */
    if (!closed) return timed_out ? USB_LEND_FAILED : USB_LEND_PENDING;
    usb_storage_closing = false;
    /* Every writer sees no card from here, and stdout/stderr (the launcher's
     * log on the card) go to RAM; then the logs close, none can reopen. */
    mt_state_set_card_lent(true);
    db_log_release_file();
    usb_dac_bridge_set_debug_log_enabled(false); /* closes its log; reopens when logging */
#ifndef HOST_BUILD
    boot_checkpoint_release();
#endif
    /* sys_server would unmount a busy card lazily and report success: the
     * computer must never get a card something still writes. */
    if (!usb_storage_lend_card_free(MT_SD_ROOT, usb_storage_busy_name, sizeof(usb_storage_busy_name)))
        return USB_LEND_FAILED;
    return USB_LEND_OK;
}

static void usb_storage_reopen_all(usb_lend_end_t end) {
    usb_storage_closing = false;
    mt_state_set_card_lent(false);
    bool computer_had_card = end == USB_LEND_END_UNPLUG || end == USB_LEND_END_EJECT || end == USB_LEND_END_USER;
    gui_library_sd_lend_finish(computer_had_card);
    if (end != USB_LEND_END_FAILED) return;
    /* Shown once the library is back (poll_usb_storage_lend()): reloading
     * it can hold the screen longer than a toast lasts. */
    if (usb_storage_busy_name[0])
        snprintf(usb_storage_notice, sizeof(usb_storage_notice), TR("Card in use by %s; can't lend it now"),
                 usb_storage_busy_name);
    else
        snprintf(usb_storage_notice, sizeof(usb_storage_notice), "%s", TR("Could not connect as USB storage"));
    usb_mode_show_actual();
}

static const usb_lend_hooks_t usb_storage_hooks = {
    .close_all = usb_storage_close_all,
    .reopen_all = usb_storage_reopen_all,
    .show = usb_storage_show,
};

static lv_obj_t * build_usb_storage_screen(void) {
    /* One column: icon, title and status flow down from below the status
     * bar, the button stays at the bottom, so wrapped text (a long
     * translation, the 360x480 panel) pushes nothing under the button. */
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(scr, BOARD_SCALE_PX(64), 0);
    lv_obj_set_style_pad_bottom(scr, HOME_INDICATOR_CONTENT_INSET + BOARD_SCALE_PX(12), 0); /* clear of the home pill */
    lv_obj_set_style_pad_row(scr, BOARD_SCALE_PX(12), 0);

    /* The stock icon is 218 px square: shrunk to fit the narrow panels. */
    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("usb/usb.png"));
    lv_obj_set_size(icon, BOARD_SCALE_PX(170), BOARD_SCALE_PX(170));
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, TR("Connected as USB storage"));
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(title, LV_PCT(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    usb_storage_status_label = lv_label_create(scr);
    lv_label_set_text(usb_storage_status_label, TR("Preparing USB storage…"));
    lv_obj_add_style(usb_storage_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_storage_status_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_storage_status_label, LV_PCT(90));
    lv_label_set_long_mode(usb_storage_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(usb_storage_status_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t * spacer = lv_obj_create(scr);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    usb_storage_button = lv_button_create(scr);
    lv_obj_set_style_bg_color(usb_storage_button, accent_lv_color(), 0);
    lv_obj_set_style_shadow_width(usb_storage_button, 0, 0); /* the theme's offset shadow reads as a second button */
    lv_obj_add_event_cb(usb_storage_button, usb_storage_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * button_label = lv_label_create(usb_storage_button);
    lv_label_set_text(button_label, TR("Disconnect"));
    lv_obj_set_style_text_font(button_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    /* Dark text on a light accent, white on a dark one, as the confirm popups do. */
    lv_obj_set_style_text_color(button_label, lv_color_luminance(accent_lv_color()) > 140
                                ? lv_color_hex(GUI_COLOR_ON_ACCENT) : lv_color_white(), 0);
    lv_obj_center(button_label);
    lv_obj_add_state(usb_storage_button, LV_STATE_DISABLED);
    return scr;
}

/* Storage is wanted when the user chose it for this connection and nothing
 * else holds or is changing the port. */
static void poll_usb_storage_lend(void) {
    bool want = usb_mode_active() == USB_MODE_STORAGE && !usb_mode_switch_active &&
                !usb_mode_switch_queued && !usb_bridge_restart_active && !usb_dac_mode_active;
    usb_storage_lend_poll(want, lv_tick_get());
    if (usb_storage_lend_busy()) {
        usb_storage_screen_update();
    } else if (usb_storage_notice[0] && gui_library_sd_lend_settled()) {
        show_info_toast_for(usb_storage_notice, 6000);
        usb_storage_notice[0] = '\0';
    }
}

/* ---- the connection: charge only, the prompt, back on unplug ---------- */

static void usb_prompt_hide(void) { gui_popup_hide(&usb_prompt_popup); }

static void usb_prompt_choose_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    usb_mode_t mode = (usb_mode_t) (intptr_t) lv_event_get_user_data(e);
    usb_prompt_hide();
    /* Charge only (or a tap outside): nothing to change. */
    if (mode == USB_MODE_CHARGE || mode == usb_mode_active()) return;
    start_usb_mode_switch(mode);
}

/* Built on first use (gui_network_teardown() drops it with the rest, so a
 * UI reload builds it again in the new language). Charge only is the
 * neutral answer, last, as tapping outside also is. */
static void usb_prompt_build(void) {
    if (usb_prompt_popup.popup) return;
    usb_prompt_popup.popup = build_popup_surface(usb_prompt_choose_cb, &usb_prompt_popup.backdrop);
    lv_obj_set_style_pad_all(usb_prompt_popup.popup, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_row(usb_prompt_popup.popup, BOARD_SCALE_PX(12), 0);
    lv_obj_t * title = lv_label_create(usb_prompt_popup.popup);
    lv_label_set_text(title, TR("Connected to a computer"));
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    static const usb_mode_t order[] = { USB_MODE_STORAGE, USB_MODE_DAC, USB_MODE_CHARGE };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (!usb_mode_offered(order[i])) continue;
        const char * label = "";
        for (size_t j = 0; j < USB_MODE_OPTION_COUNT; j++)
            if (usb_mode_options[j].mode == order[i]) label = TR(usb_mode_options[j].label);
        bool neutral = order[i] == USB_MODE_CHARGE;
        lv_color_t bg = neutral ? lv_color_hex(GUI_COLOR_CONTROL) : accent_lv_color();
        lv_obj_t * b = lv_obj_create(usb_prompt_popup.popup);
        lv_obj_remove_style_all(b);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(b, BOARD_UI_PX(46), 0);
        lv_obj_set_style_pad_all(b, BOARD_SCALE_PX(10), 0);
        lv_obj_set_style_radius(b, BOARD_UI_PX(12), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_add_style(b, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, usb_prompt_choose_cb, LV_EVENT_CLICKED, (void *) (intptr_t) order[i]);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, label);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        /* Dark text on a light accent, white on a dark one, as the confirm popups do. */
        lv_obj_set_style_text_color(l, neutral ? lv_color_hex(GUI_COLOR_TEXT)
                                       : lv_color_luminance(bg) > 140 ? lv_color_hex(GUI_COLOR_ON_ACCENT)
                                                                      : lv_color_white(), 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* Every UI poll on boards with Storage or USB DAC: runs the connection,
 * binds the charge-only set-up on each plug-in, offers the modes once a
 * computer configures the port, and goes back to charge only on unplug. */
static void poll_usb_connection(void) {
    if (!usb_storage_offered() && !usb_dac_offered()) return;
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    if (!ops || !ops->apply) return;
    battery_external_power_state_t power = battery_get_external_power_state();
    bool known = power != BATTERY_EXTERNAL_POWER_UNKNOWN;
    bool cable = power == BATTERY_EXTERNAL_POWER_CONNECTED;
    bool adb = adb_owns_usb_port();
    usb_mode_t mode = usb_mode_active();

    /* Sysfs reads, a few times a second; never mid-switch (the port is
     * being torn down and set up again). */
    if (!cable) {
        usb_conn_host_last = false;
    } else if (!usb_mode_switch_active && lv_tick_elaps(usb_conn_host_check_tick) >= 250) {
        usb_conn_host_check_tick = lv_tick_get();
        usb_conn_host_last = ops->host_connected && ops->host_connected();
    }
    usb_conn_inputs_t in = {
        .cable_known = known, .cable = cable, .host = usb_conn_host_last, .adb = adb,
        .modes_offered = true, .mode = mode,
    };
    switch (usb_conn_step(&usb_conn, &in, lv_tick_get())) {
        case USB_CONN_PROMPT:
            usb_prompt_pending = true;
            break;
        case USB_CONN_UNPLUGGED:
            /* Storage: the lend ends on the unplug by itself, and the switch
             * waits until the card is back; USB DAC: the bridge stops. */
            if (start_usb_mode_switch(USB_MODE_CHARGE)) usb_conn_revert_started(&usb_conn);
            break;
        case USB_CONN_NOTHING:
            break;
    }

    /* The charge-only set-up on each plug-in (and a cable in at start-up):
     * the R3 Pro's idle set-up is already there; configfs binds its gadget,
     * replacing whatever was left bound. ADB and a running switch are left
     * alone. */
    if (known) {
        if (!usb_cable_state_initialized) usb_charge_bind_pending = cable;
        else if (cable && !usb_cable_was_connected) usb_charge_bind_pending = true;
        usb_cable_state_initialized = true;
        usb_cable_was_connected = cable;
    }
    if (mode == USB_MODE_ADB) usb_charge_bind_pending = false;
    if (usb_charge_bind_pending && cable && mode == USB_MODE_CHARGE && !usb_mode_switch_active &&
        !usb_storage_lend_busy() && !usb_shared_card_ending()) {
        usb_charge_bind_pending = false;
        usb_mode_t live;
        bool have_live = usb_mode_control_detect_current(&live);
        if (!have_live || (live != USB_MODE_CHARGE && live != USB_MODE_ADB)) start_usb_mode_switch(USB_MODE_CHARGE);
        else usb_charge_unverified = false;
    }

    /* The prompt: once the screen is on (it does not wake it), after the
     * setup wizard and with no other popup up; dropped when the computer
     * goes or a mode was chosen meanwhile. */
    bool still_wanted = usb_conn_computer(&usb_conn) && usb_mode_active() == USB_MODE_CHARGE && !adb;
    if (usb_prompt_pending && !still_wanted) usb_prompt_pending = false;
    if (gui_popup_is_visible(&usb_prompt_popup) && !still_wanted) usb_prompt_hide();
    if (usb_prompt_pending && !usb_mode_switch_active && current_settings.setup_complete &&
        backlight_screen_is_on() && gui_popup_visible_count() == 0) {
        usb_prompt_pending = false;
        usb_prompt_build();
        gui_popup_show(&usb_prompt_popup);
    }

    if (usb_mode_list && usb_mode_choosable() != usb_mode_choosable_shown) populate_usb_mode_screen();
}

void poll_usb_storage_hotplug(void) {
    poll_usb_connection();
    /* No USB storage takeover where it is not implemented yet
     * (board_impl_missing()). */
    if (!usb_storage_offered()) return;
    /* Boards that lend the card (R3 Pro): usb_storage_lend.h. */
    if (usb_storage_lend_supported()) {
        poll_usb_storage_lend();
        return;
    }
    battery_external_power_state_t power = battery_get_external_power_state();
    /* A failed power-supply read is not an unplug. Preserve the confirmed
     * host session until power presence is known again; a session already
     * ending still finishes. */
    if (power == BATTERY_EXTERNAL_POWER_UNKNOWN) {
        if (usb_shared_state == USB_SHARED_ENDING) poll_usb_shared_card();
        return;
    }
    bool connected = power == BATTERY_EXTERNAL_POWER_CONNECTED;
    bool storage_configured = false;
    if (connected && !usb_storage_session.host_seen && !usb_mode_switch_active &&
        lv_tick_elaps(usb_storage_host_check_tick) >= 250) {
        usb_storage_host_check_tick = lv_tick_get();
        storage_configured = usb_mode_control_storage_host_configured();
    }
    bool storage_was_active = gui_network_usb_storage_session_active();
    bool storage_session_ended = usb_storage_session_poll(&usb_storage_session,
                                                          connected, storage_configured);
    if (!storage_was_active && gui_network_usb_storage_session_active())
        gui_library_suspend_boot_prompt();
    if (storage_session_ended) usb_storage_session_ended();
    /* Pauses writers while a computer has the card, takes it back after.
     * The rescan offer comes with the reload, and not while the device is a
     * USB sound card (the modal "Updating music database..." would take over
     * the DAC overlay); anything the PC wrote is then picked up by the next
     * ordinary rescan trigger or a manual one from Settings. */
    poll_usb_shared_card();
}

bool gui_network_usb_storage_session_active(void) {
    if (usb_storage_lend_supported()) return usb_storage_lend_busy();
    return usb_storage_session.host_seen && usb_cable_was_connected;
}

static void usb_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    usb_mode_t target = usb_mode_options[index].mode;
    if (usb_mode_active() == target || adb_owns_usb_port() || !usb_mode_choosable()) return;
    usb_prompt_pending = false; /* chosen here instead */
    start_usb_mode_switch(target);
}

static lv_obj_t * build_usb_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("USB Mode"), &title_label, &usb_mode_list);
}

static void open_usb_mode_screen(void) {
    /* Corrected against live gadget state whenever that state is
     * unambiguous, rather than showing a stale/wrong row highlighted (a
     * player restarted mid-connection, a change made outside this app).
     * Just sysfs reads, fast enough for the UI thread. */
    usb_mode_t detected;
    if (!usb_mode_switch_active && usb_mode_control_detect_current(&detected)) usb_mode_set_active(detected);
    populate_usb_mode_screen();
    nav_push(usb_mode_screen);
}

void usb_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_usb_mode_screen();
}

/* ---- USB DAC mode overlay + leave-confirmation popup --------------------
 * Full-screen takeover matching the stock firmware's hiby_usb.view (a
 * centered USB icon + a top-left back button, nothing else) -- deliberately
 * skips finalize_screen_navigation()'s swipe-to-back/swipe-to-player-screen
 * wiring, same reasoning as build_import_wifi_screen()'s own comment: the
 * only way out is the back button, which asks for confirmation rather than
 * leaving immediately, and an accidental swipe bypassing that would defeat
 * the point. The confirmation popup itself reuses bt_action_popup's own
 * hand-built top-layer overlay shape (this codebase doesn't use LVGL's
 * lv_msgbox anywhere). ---- */
static gui_popup_t usb_dac_leave_popup;

static void hide_usb_dac_leave_popup(void) {
    gui_popup_hide(&usb_dac_leave_popup);
}

static void usb_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup(); /* tap-outside = cancel, matches bt_action_popup precedent */
}

static void usb_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
}

static void usb_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */
    start_usb_mode_switch(USB_MODE_CHARGE); /* back to the default: charge only */
}

/* Leaving DAC mode some other way than the overlay's own Leave (Settings, an
 * unplug, a failed switch): the overlay goes, wherever it is in the stack. */
static void usb_dac_overlay_dismiss(void) {
    hide_usb_dac_leave_popup();
    if (!usb_dac_overlay_screen) return;
    if (lv_screen_active() == usb_dac_overlay_screen) {
        nav_pop();
        return;
    }
    for (int i = gui_navigation_get_depth() - 1; i >= 0; i--) {
        if (gui_navigation_get_screen_at(i) == usb_dac_overlay_screen) {
            nav_remove_stack_slot(i);
            return;
        }
    }
}

static void usb_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&usb_dac_leave_popup);
}

void build_usb_dac_leave_popup(void) {
    usb_dac_leave_popup.popup = build_confirm_popup(TR("Leave USB DAC mode?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Leave"),
                                                     lv_color_make(255, 120, 120), usb_dac_leave_confirm_cb, NULL, TR("Cancel"),
                                                     accent_lv_color(), usb_dac_leave_cancel_cb, NULL,
                                                     usb_dac_leave_popup_backdrop_cb, &usb_dac_leave_popup.backdrop);
}

static lv_obj_t * build_usb_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, usb_dac_overlay_back_cb, NULL, NULL);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("usb/usb.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-40));

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, TR("USB DAC mode"));
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    /* Use fixed width with centered text alignment so the labels stay centered
     * when their text content updates dynamically. */
    usb_dac_hint_label = lv_label_create(scr);
    lv_label_set_text(usb_dac_hint_label, TR("Waiting for USB audio…"));
    lv_obj_add_style(usb_dac_hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_hint_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    usb_dac_input_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_input_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_input_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_input_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_input_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_input_label, usb_dac_hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    usb_dac_path_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_path_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_path_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_path_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_path_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_path_label, usb_dac_input_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(4));

    return scr;
}

/* The USB DAC overlay's stream lines, refreshed with the Bluetooth DAC
 * overlay's by gui_network.c's dac_stream_labels_timer. */
void gui_usb_update_dac_stream_labels(void) {
    char text[160];
    usb_dac_stream_info_t usb;
    usb_dac_bridge_get_stream_info(&usb);
    set_label_if_changed(usb_dac_hint_label, usb.streaming ? TR("This device is now a USB sound card") : TR("Waiting for USB audio…"));
    char input_rate[32], output_rate[32];
    format_rate(input_rate, sizeof(input_rate), usb.input_sample_rate);
    format_rate(output_rate, sizeof(output_rate), usb.output_sample_rate);
    if (usb.input_dsd)
        snprintf(text, sizeof(text), TR("USB input: native %s"), uac_sa_dsd_name(usb.input_sample_rate));
    else
        snprintf(text, sizeof(text), TR("USB input: %s · %u-bit"), input_rate, usb.input_bit_depth);
    set_label_if_changed(usb_dac_input_label, text);
    if (usb.output_unsupported)
        snprintf(text, sizeof(text), "%s", TR("DAC path: this DSD rate is not supported"));
    else if (usb.output_kind != USB_DAC_STREAM_PCM)
        snprintf(text, sizeof(text), TR("DAC path: DSD over PCM (DoP) · %s"), output_rate);
    else
        snprintf(text, sizeof(text), TR("DAC path: %s · %u-bit"), output_rate, usb.output_bit_depth);
    set_label_if_changed(usb_dac_path_label, text);
}

void gui_usb_init(void) {
    usb_mode_screen = build_usb_mode_screen();
    usb_dac_overlay_screen = build_usb_dac_overlay_screen();
    usb_storage_screen = build_usb_storage_screen();
}

/* Lending the card: hooks every init (a UI reload rebuilds the screen),
 * the start-up check once. */
void gui_usb_init_lend(void) {
    if (board_has(CAP_USB_STORAGE) && !board_impl_missing(CAP_USB_STORAGE) && usb_storage_lend_supported()) {
        static bool lend_started = false;
        usb_storage_lend_set_hooks(&usb_storage_hooks);
        if (!lend_started) {
            lend_started = true;
            usb_storage_lend_start();
        } else if (usb_storage_lend_busy()) {
            usb_storage_show(true);
        }
    }
}

void gui_usb_teardown_popups(void) {
    gui_popup_teardown(&usb_dac_leave_popup);
    gui_popup_teardown(&usb_prompt_popup);
}

void gui_usb_teardown_screens(void) {
    if (usb_mode_screen) { lv_obj_delete(usb_mode_screen); usb_mode_screen = NULL; }
    if (usb_dac_overlay_screen) { lv_obj_delete(usb_dac_overlay_screen); usb_dac_overlay_screen = NULL; }
    if (usb_storage_screen) { lv_obj_delete(usb_storage_screen); usb_storage_screen = NULL; usb_storage_nav_slot = -1; }
}

bool gui_usb_has_background_work(void) {
    return usb_mode_switch_active || usb_bridge_restart_active;
}

void gui_usb_cancel_background_work(void) {
    /* The detached bridge restart owns no UI objects and may be stuck in
     * start() indefinitely. Leave its guard intact; never join it here. */
    if (usb_mode_switch_active) {
        if (!usb_mode_switch_queued) pthread_join(usb_mode_switch_thread, NULL);
        usb_mode_switch_queued = false;
        usb_mode_switch_active = false;
    }
}

lv_obj_t * gui_network_get_usb_dac_overlay(void) { return usb_dac_overlay_screen; }

bool gui_network_usb_dac_mode_active(void) { return usb_dac_mode_active; }

bool gui_network_usb_prompt_invalidated(void) {
    /* Check the live UDC state so a confirm click between polls cannot race
     * a newly-configured USB storage host. */
    return usb_mode_switch_active || usb_mode_switch_queued || usb_bridge_restart_active ||
           usb_dac_mode_active || gui_network_usb_storage_session_active() ||
           usb_mode_control_storage_host_configured();
}

bool gui_network_boot_prompt_blocked(void) {
    lv_obj_t * active = lv_screen_active();
    return gui_network_usb_prompt_invalidated() || gui_popup_is_visible(&usb_prompt_popup) || usb_prompt_pending ||
           active == gui_bluetooth_get_dac_screen() || active == gui_network_get_bt_dac_overlay() ||
           active == usb_dac_overlay_screen || active == gui_airplay_get_overlay_screen() ||
           (usb_storage_screen && active == usb_storage_screen);
}
