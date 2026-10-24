/* Bluetooth settings: the Bluetooth screen and its device list, the device
 * action popup, Bluetooth DAC (screen, overlay and leave popup), codec
 * selection, Bluetooth > Advanced and Sample Rate, and the scan/connect/
 * forget/codec/rate workers and their polls. Part of the network module
 * (gui_network.c). */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "audio.h"
#include "plugin_manager.h"
#include "bluetooth_control.h"
#include "bt_codec_choice.h"
#include "airplay_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <pthread.h>

/* Screens owned by this module */
static lv_obj_t * bt_screen;
static lv_obj_t * bt_dac_screen;
static lv_obj_t * bt_dac_overlay_screen;
static lv_obj_t * bt_dac_stream_label;
static lv_obj_t * bt_codec_screen;
static lv_obj_t * bt_advanced_screen;
static lv_obj_t * bt_rate_screen;

extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;

/* ---- Bluetooth settings screen ----
 *
 * Same shape as the Wi-Fi screen (gui_wifi.c): full scan + pair/connect, reachable
 * from the Wireless submenu's Bluetooth tile and from a long-press on the
 * drawer's bt icon. bt_control_scan() blocks for its whole scan window
 * itself (the inquiry window), so the scan thread has no separate
 * sleep to add, unlike gui_wifi.c's wifi_scan_thread_func. */

static lv_obj_t * bt_list;

static void start_bt_scan(void);

static pthread_t bt_connect_thread;
static bool bt_connect_active = false;
static atomic_bool bt_connect_done_flag = false;
static volatile bool bt_connect_succeeded = false;

/* Which device's row (see add_bt_device_row()) should show inline
 * "Connecting..."/"Failed to connect" status -- an earlier version of this
 * blocked the whole screen with a "Pairing & connecting..." overlay
 * instead, same class of issue as the scan/toggle overlays removed
 * elsewhere (see gui_wifi.c's start_wifi_scan() comment): running fully
 * in the background with inline per-row feedback, matching the stock
 * player, means there's no screen to get stuck on at all. */
static char bt_connecting_mac[18] = "";
static char bt_connect_failed_mac[18] = "";

typedef struct {
    char mac[18];
} bt_connect_request_t;

static void * bt_connect_thread_func(void * arg) {
    bt_connect_request_t * req = (bt_connect_request_t *) arg;
    bt_connect_succeeded = bt_control_connect(req->mac);
    free(req);
    atomic_store_explicit(&bt_connect_done_flag, true, memory_order_release); /* written last -- poll_bt_connect only checks this flag */
    return NULL;
}

static void start_bt_connect(const char * mac) {
    if (bt_connect_active) return; /* one at a time -- bt_connect_thread/succeeded are single shared slots */

    bt_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);

    atomic_store_explicit(&bt_connect_done_flag, false, memory_order_relaxed);
    bt_connect_active = true;
    snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", mac);
    bt_connect_failed_mac[0] = '\0';
    populate_bt_screen(); /* immediate inline "Connecting..." on that row */

        if (pthread_create(&bt_connect_thread, NULL, bt_connect_thread_func, req) != 0) {
        bt_connect_active = false;
        free(req);
        if (bt_list) lv_obj_invalidate(bt_list);
    }
}

void poll_bt_connect(void) {
    if (!bt_connect_active || !atomic_load_explicit(&bt_connect_done_flag, memory_order_acquire)) return;

    bt_connect_active = false;
    char connected_mac[18];
    snprintf(connected_mac, sizeof(connected_mac), "%s", bt_connecting_mac);
    pthread_join(bt_connect_thread, NULL);
    if (!bt_connect_succeeded) {
        snprintf(bt_connect_failed_mac, sizeof(bt_connect_failed_mac), "%s", connected_mac);
    }
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (strcmp(bt_scan_results[i].mac, connected_mac) != 0) continue;
        bt_scan_results[i].paired = bt_connect_succeeded || bt_scan_results[i].paired;
        bt_scan_results[i].connected = bt_connect_succeeded;
        break;
    }
    bt_connecting_mac[0] = '\0';
    populate_bt_screen();
}

static pthread_t bt_forget_thread;
static bool bt_forget_active = false;
static atomic_bool bt_forget_done_flag = false;

typedef struct {
    char mac[18];
    bool succeeded;
} bt_forget_request_t;

/* Kept alive past the thread completing (freed in poll_bt_forget() instead
 * of the thread func itself) so the completion handler still knows which
 * device to update in bt_scan_results -- see poll_bt_forget()'s own
 * comment for why. */
static bt_forget_request_t * bt_forget_pending_req = NULL;

static void * bt_forget_thread_func(void * arg) {
    bt_forget_request_t * req = (bt_forget_request_t *) arg;
    req->succeeded = bt_control_forget(req->mac);
    atomic_store_explicit(&bt_forget_done_flag, true, memory_order_release); /* written last -- poll_bt_forget only checks this flag */
    return NULL;
}

static void start_bt_forget(const char * mac) {
    if (bt_forget_active) return;
    gui_shell_cancel_bt_reconnect();
    bt_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    bt_forget_pending_req = req;

    atomic_store_explicit(&bt_forget_done_flag, false, memory_order_relaxed);
    bt_forget_active = true;
        if (pthread_create(&bt_forget_thread, NULL, bt_forget_thread_func, req) != 0) {
        bt_forget_active = false;
        bt_forget_pending_req = NULL;
        free(req);
    }
}

/* Updates bt_scan_results locally and redraws the Bluetooth screen upon
 * forgetting a device without running a full background scan. */
void poll_bt_forget(void) {
    if (!bt_forget_active || !atomic_load_explicit(&bt_forget_done_flag, memory_order_acquire)) return;
    bt_forget_active = false;
    pthread_join(bt_forget_thread, NULL);

    if (bt_forget_pending_req->succeeded) {
        /* A refresh launched after the initial manual cancellation may still
         * have observed the device before Forget completed. Invalidate that
         * positive snapshot before changing the remembered preference. */
        gui_shell_cancel_bt_reconnect();
        for (int i = 0; i < bt_scan_result_count; i++) {
            if (strcmp(bt_scan_results[i].mac, bt_forget_pending_req->mac) == 0) {
                bt_scan_results[i].paired = false;
                bt_scan_results[i].connected = false;
                break;
            }
        }
        if (strcmp(current_settings.bt_last_output_mac, bt_forget_pending_req->mac) == 0) {
            current_settings.bt_last_output_mac[0] = '\0';
            settings_save_async(&current_settings);
        }
    }
    free(bt_forget_pending_req);
    bt_forget_pending_req = NULL;
    populate_bt_screen();
}

/* ---- Bluetooth device action popup --------------------------------------
 * Displays an action popup offering Connect or Forget based on the device's
 * current paired/connected state. */
static gui_popup_t bt_action_popup;
static lv_obj_t * bt_action_popup_title;
static lv_obj_t * bt_action_connect_row;
static lv_obj_t * bt_action_forget_row;
static char bt_action_popup_device_mac[18] = "";

static void hide_bt_action_popup(void) {
    gui_popup_hide(&bt_action_popup);
    bt_action_popup_device_mac[0] = '\0';
}

static void bt_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_action_popup();
}

static void bt_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strlen(bt_action_popup_device_mac) != 17) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_action_popup_device_mac);
    hide_bt_action_popup();
    gui_shell_cancel_bt_reconnect();
    start_bt_connect(mac);
}

static void bt_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strlen(bt_action_popup_device_mac) != 17) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_action_popup_device_mac);
    hide_bt_action_popup();
    start_bt_forget(mac);
}

void build_bt_action_popup(void) {
    bt_action_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &bt_action_popup_title, NULL, TR("Connect"),
                                                 accent_lv_color(), bt_action_connect_cb, &bt_action_connect_row, TR("Forget"),
                                                 lv_color_make(255, 120, 120), bt_action_forget_cb, &bt_action_forget_row,
                                                 bt_action_popup_backdrop_cb, &bt_action_popup.backdrop);
}

static void show_bt_action_popup(int index) {
    if (index < 0 || index >= bt_scan_result_count) return;
    bt_device_t * dev = &bt_scan_results[index];
    snprintf(bt_action_popup_device_mac, sizeof(bt_action_popup_device_mac), "%s", dev->mac);

    lv_label_set_text(bt_action_popup_title, dev->name[0] != '\0' ? dev->name : dev->mac);

    /* Connect makes sense unless already connected. Forget makes sense for
     * anything paired OR connected (the "OR connected" is just a safety net
     * -- a connected-but-somehow-not-paired device shouldn't be able to land
     * on a popup with neither action available). */
    bool show_connect = !dev->connected;
    bool show_forget = dev->paired || dev->connected;
    /* No manual reposition needed -- bt_action_popup's rows are flex-column
     * children now (build_confirm_popup()), so hiding one lets the other
     * reflow up to take its place automatically. */
    if (show_connect) lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    if (show_forget) lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);

    gui_popup_show(&bt_action_popup);
}

static void bt_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_bt_action_popup(index);
}

static void bt_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* bt_scan_results contains both paired devices and transient discovery
     * results. Preserve the former (pairing is persistent state), but drop
     * every old Available Devices entry before beginning a new discovery
     * window. The completed scan replaces this compacted set as usual. */
    int paired_count = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        if (paired_count != i) bt_scan_results[paired_count] = bt_scan_results[i];
        paired_count++;
    }
    bt_scan_result_count = paired_count;
    populate_bt_screen();
    start_bt_scan();
}

static lv_obj_t * bt_rescan_btn;

static void set_bt_rescan_active(bool active) {
    if (!bt_rescan_btn) return;
    lv_label_set_text(bt_rescan_btn, active ? TR("Scanning...") : TR("Rescan"));
    lv_obj_set_style_opa(bt_rescan_btn, active ? LV_OPA_60 : LV_OPA_COVER, 0);
    align_screen_header_action(bt_rescan_btn, 20);
    if (active) {
        lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(bt_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(bt_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* Checks if the device name is a MAC address placeholder (e.g. "5D-C4-96-7E-56-88")
 * BlueZ uses as the Alias when no name was advertised. */
static bool bt_name_is_mac_placeholder(const bt_device_t * dev) {
    if (dev->name[0] == '\0') return true;
    size_t mac_len = strlen(dev->mac);
    if (strlen(dev->name) != mac_len) return false;
    for (size_t i = 0; i < mac_len; i++) {
        char m = dev->mac[i];
        char n = dev->name[i];
        if (m == ':') { if (n != '-' && n != ':') return false; }
        else if (m != n) return false;
    }
    return true;
}

/* Extra row height (beyond LIST_ROW_WIDTH's normal LIST_ROW_HEIGHT) when a
 * row shows the codec subtitle line -- one more small-font line plus a
 * little breathing room, not a full second LIST_ROW_HEIGHT line. */
#define BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT BOARD_SCALE_PX(40)

static void add_bt_device_row(lv_obj_t * parent, int index) {
    bt_device_t * dev = &bt_scan_results[index];

    /* Codec is only meaningful for the ONE device actually carrying A2DP
     * audio right now -- dev->connected alone isn't enough (a paired,
     * link-level-connected non-audio BLE peripheral would also read
     * connected=true here), so this also checks the row's own MAC against
     * bt_connected_mac_cached, the accessory bt_control_get_connected_
     * device_mac()/_codec() actually queried. Both come from the same
     * background poll as the rest of this screen's live state (see
     * refresh_bt_icon_thread_func()) -- never a synchronous bluealsactl
     * call here on the UI thread. */
    bool show_codec = dev->connected && bt_connected_codec_cached[0] != '\0' &&
                       strcasecmp(dev->mac, bt_connected_mac_cached) == 0;

    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT + (show_codec ? BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT : 0));
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, dev->name[0] != '\0' ? dev->name : dev->mac);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    configure_network_row_line(label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                               gui_theme_font(GUI_FONT_ROLE_BODY));

    lv_obj_t * status_label = lv_label_create(row);
    const char * status = strcmp(dev->mac, bt_connecting_mac) == 0 ? TR("Connecting") :
                          strcmp(dev->mac, bt_connect_failed_mac) == 0 ? TR("Connection failed") :
                          dev->connected ? TR("Connected") : dev->paired ? TR("Paired") : TR("Available");
    lv_label_set_text(status_label, status);
    lv_obj_add_style(status_label, dev->connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    configure_network_row_line(status_label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                               gui_theme_font(GUI_FONT_ROLE_SUBTEXT));

    lv_obj_t * codec_label = NULL;
    if (show_codec) {
        codec_label = lv_label_create(row);
        /* Codec and rate together: the negotiated link is not necessarily the
         * codec preference in Settings, and the frequency is the other half of
         * what tells you which link you actually got. Rate is omitted rather
         * than guessed when bluealsa does not report one. */
        char codec_text[96]; /* codec (32) + separator + formatted rate */
        if (bt_connected_rate_cached) {
            char rate[32];
            format_rate(rate, sizeof(rate), bt_connected_rate_cached);
            snprintf(codec_text, sizeof(codec_text), "%s · %s", bt_connected_codec_cached, rate);
        } else {
            snprintf(codec_text, sizeof(codec_text), "%s", bt_connected_codec_cached);
        }
        lv_label_set_text(codec_label, codec_text);
        lv_obj_add_style(codec_label, &style_theme_text_muted, 0);
        configure_network_row_line(codec_label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                                   gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    }
    layout_network_row(row, LIST_ROW_LABEL_INSET, label, status_label, codec_label);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static pthread_t bt_scan_thread;
static bool bt_scan_active = false;
static atomic_bool bt_scan_done_flag = false;
static bt_device_t bt_scan_pending_results[BT_MAX_RESULTS];
static int bt_scan_pending_result_count = 0;

/* Set before the thread starts, so the write happens-before the read. */
static bool bt_scan_want_inquiry = true;

static void * bt_scan_thread_func(void * arg) {
    (void) arg;
    bt_scan_pending_result_count = bt_scan_want_inquiry
        ? bt_control_scan(6, bt_scan_pending_results, BT_MAX_RESULTS)
        : bt_control_list_devices(bt_scan_pending_results, BT_MAX_RESULTS);
    atomic_store_explicit(&bt_scan_done_flag, true, memory_order_release); /* written last -- poll_bt_scan only checks this flag */
    return NULL;
}

/* Runs Bluetooth scan in a background thread without navigation or busy screen.
 * With inquiry false it only re-reads the devices BlueZ already knows, which
 * leaves an active A2DP stream alone. */
static void start_bt_scan_ex(bool inquiry) {
    if (bt_scan_active) return;

    bt_scan_want_inquiry = inquiry;

    atomic_store_explicit(&bt_scan_done_flag, false, memory_order_relaxed);
    bt_scan_active = true;
    set_bt_rescan_active(true);

    if (pthread_create(&bt_scan_thread, NULL, bt_scan_thread_func, NULL) != 0) {
        bt_scan_active = false;
        set_bt_rescan_active(false);
    }
}

static void start_bt_scan(void) { start_bt_scan_ex(true); }

void poll_bt_scan(void) {
    if (!bt_scan_active || !atomic_load_explicit(&bt_scan_done_flag, memory_order_acquire)) return;

    bt_scan_active = false;
    pthread_join(bt_scan_thread, NULL);
    set_bt_rescan_active(false);
    if (bt_scan_pending_result_count >= 0 && bt_scan_pending_result_count <= BT_MAX_RESULTS) {
        memcpy(bt_scan_results, bt_scan_pending_results,
               (size_t) bt_scan_pending_result_count * sizeof(bt_scan_results[0]));
        bt_scan_result_count = bt_scan_pending_result_count;
    }
    populate_bt_screen(); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

/* ---- Bluetooth DAC screen -- lets this device accept incoming A2DP audio
 * (another device streaming TO it, using it as an external DAC) rather than
 * only ever sending audio out. See bt_control_apply_output_settings()'s own
 * doc comment for exactly what toggling this does at the bluealsa/
 * discoverability level -- it's a real running-service restart, not just a
 * flag flip. ---- */

static lv_obj_t * bt_dac_list;

static void bt_dac_enable_row_cb(lv_event_t * e);

/* Explanation + a single "Enable" action, not a toggle -- turning Bluetooth
 * DAC on takes over the whole screen with its own overlay (see
 * build_bt_dac_overlay_screen() below), matching USB DAC mode's own design
 * (build_usb_dac_overlay_screen()): the device can't sensibly be used as a
 * normal music player while it's set up to receive and play audio FROM
 * another device, so that isn't something a background toggle should hide.
 * open_bt_dac_screen() only ever reaches this screen when Bluetooth DAC is
 * currently off -- while on, it goes straight to the overlay instead. */
void populate_bt_dac_screen(void) {
    lv_obj_clean(bt_dac_list);

    lv_obj_t * explanation = lv_label_create(bt_dac_list);
    lv_label_set_text(explanation,
                      TR("When on, this device stays visible and pairable to other Bluetooth devices, "
                      "so a phone or computer can stream audio TO it and play through this device's "
                      "own output -- using it as an external DAC."));
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(explanation, BOARD_SCALE_PX(12), 0);

    lv_obj_t * row = add_pill_row_base(bt_dac_list, TR("Enable Bluetooth DAC"));
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_dac_enable_row_cb, LV_EVENT_CLICKED, NULL);
}

/* Shared by bt_dac_enable_row_cb() and open_bt_dac_screen()'s "already
 * enabled" shortcut -- reapplies the bluealsa/bt-agent sink configuration
 * rather than relying solely on current_settings.bt_dac_mode_enabled, ensuring
 * the underlying pipeline matches the active UI state. */
static void enable_bt_dac_and_show_overlay(void) {
    gui_shell_cancel_bt_reconnect();
    current_settings.bt_dac_mode_enabled = true;
    settings_save(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);

    /* Bluetooth DAC, AirPlay, and local playback share the same physical ALSA
     * audio device and are mutually exclusive. Enabling Bluetooth DAC stops local
     * playback and turns off AirPlay if active. */
    if (audio_is_playing()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    if (current_settings.wifi_dac_mode_enabled) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        airplay_control_stop();
    }

    nav_push(bt_dac_overlay_screen);
}

static void bt_dac_enable_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    enable_bt_dac_and_show_overlay();
}

static lv_obj_t * build_bt_dac_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Bluetooth DAC"), &title_label, &bt_dac_list);
}

static void open_bt_dac_screen(void) {
    /* Bluetooth itself has to be on before any of this makes sense. The
     * Bluetooth screen only offers this row while powered, but Bluetooth can
     * go off in between, and enable_bt_dac_and_show_overlay() would spawn
     * bluealsa/bt-agent against a radio that isn't even on. */
    if (!bt_is_powered_cached) {
        show_error_toast(TR("Enable Bluetooth in settings to use BT DAC mode"));
        return;
    }
    /* Already on -- re-apply and go straight to the overlay rather than
     * showing the "explanation + Enable" screen for a mode that's already
     * supposed to be active (see enable_bt_dac_and_show_overlay()'s own
     * comment on why re-applying here, not just trusting the flag,
     * matters). */
    if (current_settings.bt_dac_mode_enabled) {
        enable_bt_dac_and_show_overlay();
        return;
    }
    populate_bt_dac_screen();
    nav_push(bt_dac_screen);
}

void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_dac_screen();
}

/* ---- Bluetooth DAC overlay + leave-confirmation popup -- same shape as
 * USB DAC mode's own (build_usb_dac_overlay_screen()/
 * build_usb_dac_leave_popup()): full-screen takeover, no swipe-to-back, the
 * only way out is the back button which asks for confirmation first. ---- */
static gui_popup_t bt_dac_leave_popup;

static void hide_bt_dac_leave_popup(void) {
    gui_popup_hide(&bt_dac_leave_popup);
}

static void bt_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */

    current_settings.bt_dac_mode_enabled = false;
    settings_save(&current_settings);
    start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
}

static void bt_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&bt_dac_leave_popup);
}

void build_bt_dac_leave_popup(void) {
    bt_dac_leave_popup.popup = build_confirm_popup(TR("Leave Bluetooth DAC mode?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Leave"),
                                                    lv_color_make(255, 120, 120), bt_dac_leave_confirm_cb, NULL, TR("Cancel"),
                                                    accent_lv_color(), bt_dac_leave_cancel_cb, NULL,
                                                    bt_dac_leave_popup_backdrop_cb, &bt_dac_leave_popup.backdrop);
}

static lv_obj_t * build_bt_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, bt_dac_overlay_back_cb, NULL, NULL);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("bt/bt.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-40));

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, TR("Bluetooth DAC mode"));
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    lv_obj_t * hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, TR("This device is now receiving Bluetooth audio"));
    lv_obj_add_style(hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    /* Same fixed-width + center treatment as the USB DAC screen's own
     * matching labels (see build_usb_dac_overlay_screen()'s own comment)
     * -- kept consistent between the two DAC mode screens on request, and
     * this specific string is long enough to need an explicit width to
     * wrap within regardless (no width means no wrap boundary, so it would
     * otherwise render on one line and can overflow past the screen edge). */
    lv_obj_set_width(hint_label, LV_PCT(90));
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    bt_dac_stream_label = lv_label_create(scr);
    lv_label_set_text(bt_dac_stream_label, TR("Waiting for Bluetooth stream…"));
    lv_obj_add_style(bt_dac_stream_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(bt_dac_stream_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(bt_dac_stream_label, LV_PCT(90));
    lv_obj_set_style_text_align(bt_dac_stream_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(bt_dac_stream_label, hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    return scr;
}

/* ---- Codec selection screen -- single-select list, current choice shown
 * via an accent-colored border rather than a checkmark glyph (same
 * indicator build_accent_color_screen's swatches use; safer than a unicode
 * checkmark that this project's subset LVGL fonts may not actually have a
 * glyph for). The preference is applied through the BlueALSA 5 source daemon
 * reconciliation path. ---- */

typedef struct {
    const char * value; /* matches player_settings_t.bt_codec / bt_control_set_codec()'s argument */
    const char * label;
} bt_codec_option_t;

/* The one global preference (bluealsa; the R3 Pro chooses per device, see
 * below), in menu order; the list shows the ones
 * bt_control_codec_supported() accepts. */
static const bt_codec_option_t bt_codec_options[] = {
    { "auto", N_("Auto") },
    { "ldac_hq", N_("LDAC Quality") }, { "ldac_sq", N_("LDAC Standard") }, { "ldac_mq", N_("LDAC Mobile") },
    { "ldac_abr", N_("LDAC Adaptive") }, { "aptx", "aptX" }, { "aac", "AAC" }, { "sbc", "SBC" },
    { "sbc_xq", "SBC-XQ" },
};
#define BT_CODEC_OPTION_COUNT (sizeof(bt_codec_options) / sizeof(bt_codec_options[0]))

static lv_obj_t * bt_codec_list;

static void bt_codec_option_row_cb(lv_event_t * e);

/* Where the codec is chosen per device (the R3 Pro, bt_codec_choice.h) the
 * screen edits the connected device's choice -- Auto or one of the codecs
 * that device has -- and the global LDAC quality. A row's user data holds
 * its kind (high byte) and its option index (low byte). */
enum { BT_CODEC_ROW_GLOBAL = 0, BT_CODEC_ROW_DEVICE = 1, BT_CODEC_ROW_LDAC = 2 };
#define BT_CODEC_ROW(kind, index) ((void *) (intptr_t) (((kind) << 8) | (index)))

typedef struct {
    const char * value; /* bt_control_set_device_codec()'s choice */
    const char * label;
    unsigned int bit;   /* BT_CODEC_*; 0 for Auto */
} bt_device_codec_option_t;

/* Best first, as Auto ranks them. */
static const bt_device_codec_option_t bt_device_codec_options[] = {
    { "auto", N_("Auto"), 0 }, { "ldac", "LDAC", BT_CODEC_LDAC }, { "aptx", "aptX", BT_CODEC_APTX },
    { "aac", "AAC", BT_CODEC_AAC }, { "sbc", "SBC", BT_CODEC_SBC },
};
#define BT_DEVICE_CODEC_OPTION_COUNT (sizeof(bt_device_codec_options) / sizeof(bt_device_codec_options[0]))

static const bt_codec_option_t bt_ldac_quality_options[] = {
    { "ldac_hq", N_("Quality") }, { "ldac_sq", N_("Standard") }, { "ldac_mq", N_("Mobile") },
    { "ldac_abr", N_("Adaptive") },
};
#define BT_LDAC_QUALITY_OPTION_COUNT (sizeof(bt_ldac_quality_options) / sizeof(bt_ldac_quality_options[0]))

/* "ldac_hq", "aptx", ... -> the codec's name in the list; NULL if unknown. */
static const char * bt_codec_family_label(const char * setting) {
    unsigned int bit = bt_codec_bit(setting);
    for (size_t i = 1; bit && i < BT_DEVICE_CODEC_OPTION_COUNT; i++)
        if (bt_device_codec_options[i].bit == bit) return bt_device_codec_options[i].label;
    return NULL;
}

static void populate_bt_device_codec_rows(void) {
    bt_codec_device_view_t view;
    if (!bt_connected_mac_cached[0] || !bt_control_codec_device_view(bt_connected_mac_cached, &view)) {
        add_section_header(bt_codec_list, TR("The codec is chosen for each device: connect one to change its codec"));
        return;
    }
    add_section_header(bt_codec_list, TR("Connected device"));
    const char * resolved = bt_codec_family_label(view.resolved);
    for (size_t i = 0; i < BT_DEVICE_CODEC_OPTION_COUNT; i++) {
        const bt_device_codec_option_t * o = &bt_device_codec_options[i];
        if (o->bit && !(view.codecs & o->bit)) continue; /* only what the device has */
        char label[64];
        if (!o->bit && resolved) snprintf(label, sizeof(label), TR("Auto (%s)"), resolved);
        else snprintf(label, sizeof(label), "%s", TR(o->label));
        add_pill_option_row(bt_codec_list, label, strcmp(view.choice, o->value) == 0, bt_codec_option_row_cb,
                            BT_CODEC_ROW(BT_CODEC_ROW_DEVICE, (int) i));
    }
    if (!view.codecs) add_section_header(bt_codec_list, TR("Its codecs are listed once it has connected"));
}

static void populate_bt_codec_screen(void) {
    lv_obj_clean(bt_codec_list);
    if (bt_control_codec_per_device()) {
        populate_bt_device_codec_rows();
        add_section_header(bt_codec_list, TR("LDAC quality"));
        for (size_t i = 0; i < BT_LDAC_QUALITY_OPTION_COUNT; i++)
            add_pill_option_row(bt_codec_list, TR(bt_ldac_quality_options[i].label),
                                strcmp(current_settings.bt_ldac_quality, bt_ldac_quality_options[i].value) == 0,
                                bt_codec_option_row_cb, BT_CODEC_ROW(BT_CODEC_ROW_LDAC, (int) i));
        return;
    }
    const char * current = current_settings.bt_codec;
    /* Show what the system will negotiate, shared with the stock player
     * (BlueZ 4 back end; BlueZ 5 has no system-wide codec). */
    char system_codec[16];
    if (bt_control_get_system_codec(system_codec, sizeof(system_codec))) current = system_codec;
    for (size_t i = 0; i < BT_CODEC_OPTION_COUNT; i++) {
        if (!bt_control_codec_supported(bt_codec_options[i].value)) continue;
        bool selected = strcmp(current, bt_codec_options[i].value) == 0;
        add_pill_option_row(bt_codec_list, TR(bt_codec_options[i].label),
                            selected, bt_codec_option_row_cb, BT_CODEC_ROW(BT_CODEC_ROW_GLOBAL, (int) i));
    }
}

/* Setting the codec waits for any scan, pairing or connection in progress
 * on the R3 Pro (one controller command at a time), which can take minutes,
 * so it runs on a worker while the screen stays live. A per-device choice
 * writes the card and may run bt_codec: the same worker. */
static pthread_t bt_codec_apply_thread;
static bool bt_codec_apply_active = false;
static atomic_bool bt_codec_apply_done_flag = false;
static bool bt_codec_apply_ok = false;
/* Captured at launch: what is being applied, to which device, and the codec
 * that device resolved to before. */
static int bt_codec_apply_kind = BT_CODEC_ROW_GLOBAL;
static int bt_codec_apply_index = 0;
static char bt_codec_apply_mac[18];
static char bt_codec_apply_before[16];

static void * bt_codec_apply_thread_func(void * arg) {
    (void) arg;
    if (bt_codec_apply_kind == BT_CODEC_ROW_DEVICE)
        bt_codec_apply_ok = bt_control_set_device_codec(bt_codec_apply_mac,
                                                        bt_device_codec_options[bt_codec_apply_index].value);
    else if (bt_codec_apply_kind == BT_CODEC_ROW_LDAC)
        bt_codec_apply_ok = bt_control_apply_codec();
    else
        bt_codec_apply_ok = bt_control_set_codec(bt_codec_options[bt_codec_apply_index].value);
    atomic_store_explicit(&bt_codec_apply_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

void poll_bt_codec_apply(void) {
    if (!bt_codec_apply_active || !atomic_load_explicit(&bt_codec_apply_done_flag, memory_order_acquire)) return;
    bt_codec_apply_active = false;
    pthread_join(bt_codec_apply_thread, NULL);
    if (!bt_codec_apply_ok) {
        show_error_toast(TR("Could not save Bluetooth codec"));
        return;
    }
    bool codec_changed;
    if (bt_codec_apply_kind == BT_CODEC_ROW_GLOBAL) {
        const char * codec = bt_codec_options[bt_codec_apply_index].value;
        codec_changed = strcmp(current_settings.bt_codec, codec) != 0;
        snprintf(current_settings.bt_codec, sizeof(current_settings.bt_codec), "%s", codec);
        settings_save_async(&current_settings);
    } else {
        bt_codec_device_view_t view;
        codec_changed = bt_control_codec_device_view(bt_codec_apply_mac, &view) &&
                        strcmp(view.resolved, bt_codec_apply_before) != 0;
    }
    if (bt_codec_screen && gui_navigation_is_top(bt_codec_screen)) populate_bt_codec_screen();
    if (codec_changed) show_info_toast(TR("Turn Bluetooth off and on to apply"));
}

static void bt_codec_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int data = (int) (intptr_t) lv_event_get_user_data(e);
    int kind = data >> 8, index = data & 0xff;
    size_t count = kind == BT_CODEC_ROW_DEVICE ? BT_DEVICE_CODEC_OPTION_COUNT
                   : kind == BT_CODEC_ROW_LDAC ? BT_LDAC_QUALITY_OPTION_COUNT
                                               : BT_CODEC_OPTION_COUNT;
    if ((size_t) index >= count) return;
    if (bt_codec_apply_active) {
        show_info_toast(TR("Still applying the previous choice"));
        return;
    }
    if (kind != BT_CODEC_ROW_GLOBAL) {
        bt_codec_device_view_t before;
        if (!bt_control_codec_device_view(bt_connected_mac_cached, &before)) before.resolved[0] = '\0';
        snprintf(bt_codec_apply_before, sizeof(bt_codec_apply_before), "%s", before.resolved);
        snprintf(bt_codec_apply_mac, sizeof(bt_codec_apply_mac), "%s", bt_connected_mac_cached);
    }
    if (kind == BT_CODEC_ROW_DEVICE && !bt_codec_apply_mac[0]) return;
    if (kind == BT_CODEC_ROW_LDAC) {
        /* Global and no I/O: kept here, and the worker only configures it
         * for the connected device. */
        const char * quality = bt_ldac_quality_options[index].value;
        if (strcmp(current_settings.bt_ldac_quality, quality) == 0) return;
        snprintf(current_settings.bt_ldac_quality, sizeof(current_settings.bt_ldac_quality), "%s", quality);
        settings_save_async(&current_settings);
        bt_control_set_ldac_quality(quality);
        populate_bt_codec_screen();
        if (!bt_codec_apply_mac[0]) return;
    }
    atomic_store_explicit(&bt_codec_apply_done_flag, false, memory_order_relaxed);
    bt_codec_apply_kind = kind;
    bt_codec_apply_index = index;
    bt_codec_apply_active = true;
    if (pthread_create(&bt_codec_apply_thread, NULL, bt_codec_apply_thread_func, NULL) != 0) {
        bt_codec_apply_active = false;
        show_error_toast(TR("Thread launch failed"));
        return;
    }
    show_info_toast(TR("Changing Bluetooth codec…"));
}

/* ---- Bluetooth > Advanced ---------------------------------------------
 * Settings that are set once and rarely revisited, kept off the Bluetooth
 * screen so the device list stays the focus there. ---- */
static lv_obj_t * bt_advanced_list;
static lv_obj_t * bt_rate_list;

static void bt_rate_option_row_cb(lv_event_t * e);

/* Rates offered by the connected accessory for the codec in use. Queried
 * when the screen opens, because the set depends on both the accessory and
 * the negotiated codec. */
#define BT_RATE_MAX_OPTIONS 12
static unsigned int bt_rate_options[BT_RATE_MAX_OPTIONS];
static int bt_rate_option_count;
/* True while the background query is in flight, so the list can say so. */
static bool bt_rate_query_active = false;

static void bt_rate_format(char * out, size_t out_size, unsigned int rate) {
    if (rate % 1000 == 0) snprintf(out, out_size, "%u kHz", rate / 1000);
    else snprintf(out, out_size, "%u.%u kHz", rate / 1000, (rate % 1000) / 100);
}

/* Rate shown and edited is the connected accessory's own, falling back to the
 * global default when nothing is connected. */
static unsigned int bt_rate_selected(void) {
    return settings_bt_rate_for(&current_settings, bt_connected_mac_cached);
}

static void populate_bt_rate_screen(void) {
    lv_obj_clean(bt_rate_list);
    add_pill_option_row(bt_rate_list, TR("Automatic (44.1 kHz)"), bt_rate_selected() == 0,
                        bt_rate_option_row_cb, (void *) (intptr_t) 0);

    /* Nothing connected means nothing to query, and only 44.1 can be applied
     * without an accessory present to negotiate with. */
    if (bt_rate_option_count == 0) {
        add_pill_option_row(bt_rate_list, "44.1 kHz", bt_rate_selected() == 44100,
                            bt_rate_option_row_cb, (void *) (intptr_t) 44100);
        add_section_header(bt_rate_list, bt_rate_query_active ? TR("Reading supported rates...")
                                                              : TR("Connect a device to see its supported rates"));
        return;
    }
    for (int i = 0; i < bt_rate_option_count; i++) {
        char label[32];
        bt_rate_format(label, sizeof(label), bt_rate_options[i]);
        add_pill_option_row(bt_rate_list, label, bt_rate_selected() == bt_rate_options[i],
                            bt_rate_option_row_cb, (void *) (intptr_t) bt_rate_options[i]);
    }
}

static void populate_bt_advanced_screen(void);
/* Defined with the Bluetooth screen further down; the Advanced screen owns
 * the rows now but the callbacks stay next to the state they touch. */
static void bt_volume_sync_toggle_cb(lv_event_t * e);
static void bt_hide_unnamed_toggle_cb(lv_event_t * e);
static void bt_codec_settings_row_cb(lv_event_t * e);

/* Cycling the accessory's link blocks for seconds, so it runs on a worker
 * while the screen stays live. */
static pthread_t bt_rate_apply_thread;
static bool bt_rate_apply_active = false;
static atomic_bool bt_rate_apply_done_flag = false;
/* Captured at launch so a status refresh cannot change what is being applied
 * half way through. */
static unsigned int bt_rate_apply_target = 0;

static void * bt_rate_apply_thread_func(void * arg) {
    (void) arg;
    (void) bt_control_reconnect_for_rate_change(bt_rate_apply_target);
    atomic_store_explicit(&bt_rate_apply_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

void poll_bt_rate_apply(void) {
    if (!bt_rate_apply_active || !atomic_load_explicit(&bt_rate_apply_done_flag, memory_order_acquire)) return;
    bt_rate_apply_active = false;
    pthread_join(bt_rate_apply_thread, NULL);
    /* No outcome toast: the accessory may still need reconnecting by hand,
     * so "applied" or "failed" here would contradict what the user is about
     * to do. The redrawn list shows the rate that actually took. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
}

static void bt_rate_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    unsigned int rate = (unsigned int) (intptr_t) lv_event_get_user_data(e);
    bool changed = bt_rate_selected() != rate;
    /* Remembered against the accessory when one is connected, so each keeps
     * its own; with nothing connected this sets the default for accessories
     * that have no entry yet. */
    if (bt_connected_mac_cached[0]) settings_bt_set_rate_for(&current_settings, bt_connected_mac_cached, rate);
    else current_settings.bt_sample_rate = rate;
    settings_save_async(&current_settings);
    bt_control_choose_sample_rate(rate);
    populate_bt_rate_screen();
    if (!changed) return;

    /* The rate is fixed when the A2DP configuration is negotiated, and these
     * accessories refuse to renegotiate a live transport, so the link has to
     * be re-established for the choice to take effect. Only the link cycles;
     * the radio stays on. With nothing connected there is nothing to cycle
     * and the choice simply applies on the next connection. */
    if (!gui_shell_is_bt_audio_connected()) return;
    /* HiBy's BlueZ 4 asks for the rate when a stream is configured: it
     * applies on the next connection. */
    if (!(bt_control_backend_features() & BT_BACKEND_LIVE_RATE_CHANGE)) {
        show_info_toast(TR("Turn Bluetooth off and on to apply"));
        return;
    }
    if (bt_rate_apply_active) {
        /* Silently dropping this would leave the setting saying one thing and
         * the transport doing another. */
        show_info_toast(TR("Still applying the previous choice"));
        return;
    }
    atomic_store_explicit(&bt_rate_apply_done_flag, false, memory_order_relaxed);
    bt_rate_apply_target = rate;
    bt_rate_apply_active = true;
    if (pthread_create(&bt_rate_apply_thread, NULL, bt_rate_apply_thread_func, NULL) != 0) {
        bt_rate_apply_active = false;
        show_info_toast(TR("Turn Bluetooth off and on to apply"));
        return;
    }
    show_info_toast(TR("Headset may disconnect, manual reconnection might be required"));
}

static lv_obj_t * build_bt_rate_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen(TR("Sample Rate"), &title_label, &bt_rate_list);
}

/* The query forks bluealsactl, which must never happen on the UI thread:
 * this screen would otherwise freeze for as long as the call takes, the same
 * stall populate_bt_screen() documents for bt_control_is_powered(). The
 * screen opens with what is already known and fills in when the worker
 * finishes. */
static pthread_t bt_rate_query_thread;
static atomic_bool bt_rate_query_done_flag = false;
static unsigned int bt_rate_query_results[BT_RATE_MAX_OPTIONS];
static int bt_rate_query_result_count;

static void * bt_rate_query_thread_func(void * arg) {
    (void) arg;
    bt_rate_query_result_count = bt_control_get_available_rates(bt_rate_query_results, BT_RATE_MAX_OPTIONS);
    atomic_store_explicit(&bt_rate_query_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

/* The screen shows one accessory's rates and edits that accessory's entry,
 * so a device changing underneath it has to redraw or it would edit the
 * wrong one. */
void gui_network_notify_bt_device_changed(void) {
    if (bt_codec_screen && gui_navigation_is_top(bt_codec_screen)) populate_bt_codec_screen();
    if (!bt_rate_screen || !gui_navigation_is_top(bt_rate_screen)) return;
    bt_rate_option_count = 0;
    populate_bt_rate_screen();
}

void poll_bt_rate_query(void) {
    if (!bt_rate_query_active || !atomic_load_explicit(&bt_rate_query_done_flag, memory_order_acquire)) return;
    bt_rate_query_active = false;
    pthread_join(bt_rate_query_thread, NULL);
    if (bt_rate_query_result_count > 0) {
        memcpy(bt_rate_options, bt_rate_query_results, sizeof(bt_rate_options));
        bt_rate_option_count = bt_rate_query_result_count;
    }
    /* Repainted even when the query found nothing, so the "reading" note is
     * replaced rather than left up until the screen is reopened. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
}

static void bt_rate_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bt_rate_option_count = 0;
    populate_bt_rate_screen();
    nav_push(bt_rate_screen);
    if (gui_shell_is_bt_audio_connected() && !bt_rate_query_active) {
        atomic_store_explicit(&bt_rate_query_done_flag, false, memory_order_relaxed);
        bt_rate_query_active = true;
        if (pthread_create(&bt_rate_query_thread, NULL, bt_rate_query_thread_func, NULL) != 0)
            bt_rate_query_active = false;
    }
}

static void bt_speexrate_toggle_cb(lv_event_t * e) {
    /* add_pill_toggle_row() makes the switch itself non-clickable and fires
     * CLICKED on the row, so the state is flipped here rather than read back
     * off the widget. */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_speexrate_enabled = !current_settings.bt_speexrate_enabled;
    settings_save_async(&current_settings);
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    populate_bt_advanced_screen();
}

static void populate_bt_advanced_screen(void) {
    lv_obj_clean(bt_advanced_list);
    /* No AVRCP absolute volume on BlueZ 4 (the R3 Pro). */
    if (bt_control_backend_features() & BT_BACKEND_VOLUME_SYNC)
        add_pill_toggle_row(bt_advanced_list, TR("Bluetooth Volume Sync"),
                            current_settings.bt_volume_sync_enabled, bt_volume_sync_toggle_cb);
    add_pill_chevron_row(bt_advanced_list, TR("Codec"), bt_codec_settings_row_cb);
    add_pill_chevron_row(bt_advanced_list, TR("Sample Rate"), bt_rate_settings_row_cb);
    /* Only does anything for a track whose rate differs from the negotiated
     * transport rate; with Sample Rate at 44.1 kHz most tracks never reach
     * the resampler at all. */
    /* No speex rate plugin on the R3 Pro firmware. */
    if (bt_control_backend_features() & BT_BACKEND_SPEEX_RATE)
        add_pill_toggle_row(bt_advanced_list, TR("Speex Resampling"),
                            current_settings.bt_speexrate_enabled, bt_speexrate_toggle_cb);
    add_pill_toggle_row(bt_advanced_list, TR("Hide Unnamed Devices"),
                        current_settings.bt_hide_unnamed_devices, bt_hide_unnamed_toggle_cb);
}

static lv_obj_t * build_bt_advanced_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen(TR("Advanced"), &title_label, &bt_advanced_list);
}

static void bt_advanced_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_bt_advanced_screen();
    nav_push(bt_advanced_screen);
}

static lv_obj_t * build_bt_codec_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Codec"), &title_label, &bt_codec_list);
}

static void open_bt_codec_screen(void) {
    populate_bt_codec_screen();
    nav_push(bt_codec_screen);
}

static void bt_codec_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_codec_screen();
}

static void bt_volume_sync_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_volume_sync_enabled = !current_settings.bt_volume_sync_enabled;
    settings_save(&current_settings);
    start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    populate_bt_advanced_screen(); /* the row lives on Advanced now */
}

/* Display filter toggle to hide unnamed Bluetooth devices (e.g. BLE beacons)
 * from the Available Devices list. Paired devices are never hidden. */
static void bt_hide_unnamed_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_hide_unnamed_devices = !current_settings.bt_hide_unnamed_devices;
    settings_save(&current_settings);
    populate_bt_advanced_screen(); /* moves the switch the user just tapped */
    populate_bt_screen();          /* and the device list it filters, for the way back */
}

/* ---- Master rebuild for the Bluetooth screen's one dynamic list: toggle
 * row, then (only while powered) volume sync/DAC/Codec rows, Paired
 * Devices, and Available Devices -- same full-rebuild-on-any-change
 * convention as populate_wifi_screen() (gui_wifi.c). bt_scan_results already holds
 * both paired and freshly-discovered devices from one bt_control_scan()
 * call; this just splits that single list into the two sections by
 * ->paired rather than needing two separate scans. ---- */
void populate_bt_screen(void) {
    /* Preserve scroll position across periodic screen rebuilds so the view
     * does not snap back to the top while browsing devices. */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(bt_list);

    lv_obj_clean(bt_list);

    /* bt_is_powered_cached, not bt_control_is_powered() -- same reasoning
     * as external_dac_block_reason()'s own use of the cache (see its
     * comment): calling bt_control_is_powered() directly here forked
     * bluetoothctl show synchronously on the UI thread every time this
     * screen opened, which on a real device produced a multi-second freeze
     * entering the Bluetooth submenu (a milder version of the same
     * pre-existing hang class start_refresh_bt_icon()'s own comment
     * documents). The cache is kept fresh in the background regardless. */
    bool powered = bt_is_powered_cached;
    if (bt_rescan_btn) {
        if (powered) lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
    /* quick_drawer_bt_event_cb is the SAME real toggle-thread trigger the
     * drawer's own bt icon uses -- see populate_wifi_screen()'s identical
     * reasoning for wifi. */
    add_pill_toggle_row(bt_list, TR("Bluetooth"), powered, quick_drawer_bt_event_cb);

    if (!powered) {
        build_list_message(bt_list, TR("Bluetooth is off"), TR("Turn on Bluetooth to see paired and nearby devices."));
        lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
        return;
    }

    /* Bluetooth DAC = the receiver (A2DP sink). */
    if (board_has(CAP_BT_RECEIVER)) add_pill_chevron_row(bt_list, TR("Bluetooth DAC"), bt_dac_settings_row_cb);
    add_pill_chevron_row(bt_list, TR("Advanced"), bt_advanced_settings_row_cb);

    add_section_header(bt_list, TR("Paired Devices"));
    int paired_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        add_bt_device_row(bt_list, i);
        paired_shown++;
    }
    if (paired_shown == 0) {
        build_list_message(bt_list, TR("No paired devices"), TR("Previously connected accessories will appear here."));
    }

    add_section_header(bt_list, TR("Available Devices"));
    int available_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_scan_results[i].paired) continue;
        if (current_settings.bt_hide_unnamed_devices && bt_name_is_mac_placeholder(&bt_scan_results[i])) continue;
        add_bt_device_row(bt_list, i);
        available_shown++;
    }
    if (available_shown == 0) {
        build_list_message(bt_list, TR("No nearby devices"), TR("Make an accessory discoverable, then tap Rescan."));
    }

    lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
}

static lv_obj_t * build_bluetooth_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen(TR("Bluetooth"), &title_label, &bt_list);

    bt_rescan_btn = lv_label_create(scr);
    lv_label_set_text(bt_rescan_btn, TR("Rescan"));
    lv_obj_add_style(bt_rescan_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(bt_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(bt_rescan_btn, 20);
    lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_rescan_btn, bt_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void open_bluetooth_screen(void) {
    populate_bt_screen();
    nav_push(bt_screen);
    /* Auto-refresh on open -- cached power state, not a fresh
     * bt_control_is_powered() call, same reasoning as populate_bt_screen().
     * An inquiry is skipped while an accessory is connected: discovery makes
     * the controller interleave with the A2DP link and the music audibly
     * stutters for the whole scan window. The known-device list still
     * refreshes, and Rescan is there when discovery is actually wanted. */
    if (bt_is_powered_cached) start_bt_scan_ex(!gui_shell_is_bt_audio_connected());
}

/* The Bluetooth DAC overlay's stream line, refreshed with the USB DAC
 * overlay's by gui_network.c's dac_stream_labels_timer. */
void gui_bluetooth_update_dac_stream_label(void) {
    bt_dac_stream_info_t bt;
    bt_control_get_dac_stream_info(&bt);
    char text[160];
    if (!bt.available || !bt.running) {
        snprintf(text, sizeof(text), "%s", TR("Waiting for Bluetooth stream…"));
    } else {
        char rate[32];
        format_rate(rate, sizeof(rate), bt.sample_rate);
        const char * format = bt.bit_depth ? NULL : (bt.pcm_format[0] ? bt.pcm_format : TR("Unknown format"));
        if (bt.bit_depth)
            snprintf(text, sizeof(text), "%s · %s · %u-bit", bt.codec[0] ? bt.codec : TR("Unknown codec"), rate, bt.bit_depth);
        else
            snprintf(text, sizeof(text), "%s · %s · %s", bt.codec[0] ? bt.codec : TR("Unknown codec"), rate, format);
    }
    set_label_if_changed(bt_dac_stream_label, text);
}

void gui_bluetooth_init(void) {
    bt_screen = build_bluetooth_screen();
    bt_dac_screen = build_bt_dac_screen();
    bt_dac_overlay_screen = build_bt_dac_overlay_screen();
    bt_codec_screen = build_bt_codec_screen();
    bt_advanced_screen = build_bt_advanced_screen();
    bt_rate_screen = build_bt_rate_screen();
}

void gui_bluetooth_teardown_action_popup(void) {
    gui_popup_teardown(&bt_action_popup);
}

void gui_bluetooth_teardown_dac_leave_popup(void) {
    gui_popup_teardown(&bt_dac_leave_popup);
}

void gui_bluetooth_teardown_screens(void) {
    if (bt_screen) { lv_obj_delete(bt_screen); bt_screen = NULL; }
    if (bt_dac_screen) { lv_obj_delete(bt_dac_screen); bt_dac_screen = NULL; }
    if (bt_dac_overlay_screen) { lv_obj_delete(bt_dac_overlay_screen); bt_dac_overlay_screen = NULL; }
    if (bt_codec_screen) { lv_obj_delete(bt_codec_screen); bt_codec_screen = NULL; }
    if (bt_advanced_screen) { lv_obj_delete(bt_advanced_screen); bt_advanced_screen = NULL; }
    if (bt_rate_screen) { lv_obj_delete(bt_rate_screen); bt_rate_screen = NULL; }
}

bool gui_bluetooth_has_background_work(void) {
    return bt_connect_active || bt_forget_active || bt_scan_active;
}

void gui_bluetooth_cancel_background_work(void) {
    if (bt_connect_active) {
        pthread_join(bt_connect_thread, NULL);
        bt_connect_active = false;
    }
    if (bt_forget_active) {
        pthread_join(bt_forget_thread, NULL);
        bt_forget_active = false;
    }
    if (bt_rate_query_active) {
        pthread_join(bt_rate_query_thread, NULL);
        bt_rate_query_active = false;
    }
    if (bt_rate_apply_active) {
        pthread_join(bt_rate_apply_thread, NULL);
        bt_rate_apply_active = false;
    }
    if (bt_scan_active) {
        pthread_join(bt_scan_thread, NULL);
        bt_scan_active = false;
    }
}

lv_obj_t * gui_network_get_bt_screen(void) { return bt_screen; }
lv_obj_t * gui_network_get_bt_dac_overlay(void) { return bt_dac_overlay_screen; }
lv_obj_t * gui_bluetooth_get_dac_screen(void) { return bt_dac_screen; }
