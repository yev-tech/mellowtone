/* Remote Control (optional feature REMOTE): the Remote Control screen, its
 * new-PIN popup and toggle. Part of the network module (gui_network.c). */
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
#include "bluetooth_control.h"
#include "remote_control.h"
#include "bt_remote_control.h"
#include "wifi_control.h"
#include "debug_log.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#ifdef TEST_BOOT_RC
static atomic_bool test_boot_services_started = false;

/* /usr/bin/bt_init creates this only after controller firmware setup. Wait
 * here, off the LVGL thread, so this test path cannot race stock boot's chip
 * initialization. The bounded wait keeps a failed boot from hanging forever. */
static void * test_boot_services_worker(void * arg) {
    (void) arg;
    /* Start Remote Control immediately. Both entry points create their own
     * background workers; the RFCOMM worker waits/retries until BlueZ is
     * available, while the HTTP listener can serve as soon as Wi-Fi is up. */
    remote_control_start();
    bt_remote_control_start();

    /* /usr/bin/bt_init creates this only after controller firmware setup.
     * Wait off the LVGL thread to avoid racing stock boot's chip init. */
    bool init_gate_seen = false;
    for (int i = 0; i < 240; ++i) {
        if (access("/tmp/bt_init_ok", F_OK) == 0) {
            init_gate_seen = true;
            break;
        }
        usleep(500000);
    }
    if (!init_gate_seen) {
        DBG_LOG("test boot RC: Bluetooth initialization gate timed out\n");
        return NULL;
    }

    if (bt_control_init_chip() && !bt_control_is_powered()) bt_control_enable();
    return NULL;
}

#endif

static lv_obj_t * remote_control_screen;
#ifndef HOST_BUILD
static bool remote_control_last_bt_powered;
static bt_remote_control_status_t remote_control_last_bt_status = BT_REMOTE_CONTROL_DISABLED;
#endif

/* ---- Remote Control screen (Wireless -> "Open Link") -- exposes the
 * HTTP address over Wi-Fi and the RFCOMM service name over Bluetooth.
 * Built on the same flex list as the DLNA/Wi-Fi screens: standard toggle
 * row, a "Connection PIN" row with a refresh icon to replace the PIN, then the
 * QR/URL lines styled like build_import_wifi_screen()'s -- the same
 * "here's an address, scan or type it" moment for a different feature. ---- */

static lv_obj_t * remote_control_toggle_img;
static lv_obj_t * remote_control_pin_label;
static char remote_control_displayed_pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static bool remote_control_displayed_pin_valid;
static lv_obj_t * remote_control_status_label;
static lv_obj_t * remote_control_url_label;
static lv_obj_t * remote_control_bluetooth_label;
#if LV_USE_QRCODE
static lv_obj_t * remote_control_qrcode;
#endif

/* The screen is a flex column (build_subsonic_list_screen()), so positions
 * follow the content automatically. An empty label still occupies a text
 * line there, though, so hide the address lines whose text is empty instead
 * of leaving blank gaps. Call after every text change below. */
static void remote_control_hide_empty_lines(void) {
    lv_obj_t * lines[] = { remote_control_url_label, remote_control_bluetooth_label };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char * text = lv_label_get_text(lines[i]);
        if (text && text[0]) lv_obj_remove_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void remote_control_refresh_pin_label(void) {
    if (!remote_control_pin_label) return;
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1] = {0};
    remote_control_get_pin(pin, sizeof(pin));
    if (remote_control_displayed_pin_valid && strcmp(remote_control_displayed_pin, pin) == 0) return;
    /* Empty only if the system random source failed; the server refuses to
     * start in that state too, so say so rather than show a fake PIN. */
    lv_label_set_text(remote_control_pin_label, pin[0] ? pin : TR("Unavailable"));
    snprintf(remote_control_displayed_pin, sizeof(remote_control_displayed_pin), "%s", pin);
    remote_control_displayed_pin_valid = true;
}

void remote_control_refresh_address(void) {
    remote_control_refresh_pin_label();
    if (!current_settings.remote_control_enabled) {
        lv_label_set_text(remote_control_status_label,
                           TR("Turn this on to see the address here."));
        lv_label_set_text(remote_control_url_label, "");
        lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    wifi_settings_snapshot_start();
    bool wifi_available = gui_shell_wifi_effective_enabled() && wifi_cached_info_is_current() &&
                          gui_wifi_cached_info_connected() && gui_wifi_cached_info()->ip[0] != '\0';
    bool bluetooth_available = false;
    bool bluetooth_powered = false;
    bt_remote_control_status_t bluetooth_status = BT_REMOTE_CONTROL_DISABLED;
    /* The RFCOMM transport needs BlueZ 5 (ProfileManager1); Wi-Fi only on
     * BlueZ 4 (the R3 Pro). */
    bool bluetooth_transport = (bt_control_backend_features() & BT_BACKEND_RFCOMM_REMOTE) != 0;
#if !defined(HOST_BUILD)
    if (bluetooth_transport) {
        bluetooth_powered = bt_is_powered_cached;
        bluetooth_status = bt_remote_control_status();
        bluetooth_available = bluetooth_powered && bt_remote_control_is_advertising();
    }
#endif
    if (!wifi_available && !bluetooth_available) {
        if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
            lv_label_set_text(remote_control_status_label, TR("Bluetooth remote service is unavailable; retrying."));
        else if (bluetooth_powered)
            lv_label_set_text(remote_control_status_label, TR("Bluetooth remote service is starting…"));
        else if (!bluetooth_transport)
            lv_label_set_text(remote_control_status_label, TR("Connect to Wi-Fi to use the remote."));
        else
            lv_label_set_text(remote_control_status_label, TR("Enable Wi-Fi or Bluetooth to connect."));
        lv_label_set_text(remote_control_url_label, "");
        if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
            lv_label_set_text(remote_control_bluetooth_label, TR("Bluetooth: registration failed; retrying"));
        else if (bluetooth_powered)
            lv_label_set_text(remote_control_bluetooth_label, TR("Bluetooth: waiting for service registration"));
        else
            lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    lv_label_set_text(remote_control_status_label, TR("Connect using either available route:"));
    if (wifi_available) {
        char url[64];
        snprintf(url, sizeof(url), "Wi-Fi: http://%s:8899", gui_wifi_cached_info()->ip);
        lv_label_set_text(remote_control_url_label, url);
    } else {
        lv_label_set_text(remote_control_url_label, "");
    }
    if (bluetooth_available) lv_label_set_text(remote_control_bluetooth_label, TR("Bluetooth: Mellowtone Remote"));
    else if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
        lv_label_set_text(remote_control_bluetooth_label, TR("Bluetooth: registration failed; retrying"));
    else if (bluetooth_powered)
        lv_label_set_text(remote_control_bluetooth_label, TR("Bluetooth: waiting for service registration"));
    else lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
    if (wifi_available) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:8899", gui_wifi_cached_info()->ip);
        lv_obj_remove_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(remote_control_qrcode, url, strlen(url));
    } else {
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    remote_control_hide_empty_lines();
}

static void refresh_remote_control_screen_if_built(void) {
    if (remote_control_toggle_img) remote_control_refresh_address();
}

#ifdef TEST_BOOT_RC
/* gui_init invokes this on the LVGL thread after all screens are built. Sync
 * the cached screen's switch and address before starting asynchronous service
 * bring-up so the test-only boot state is visible immediately. */
void gui_network_start_test_boot_services(void) {
    bool expected = false;
    if (!MT_HAS_FEATURE(REMOTE)) return;
    if (!atomic_compare_exchange_strong(&test_boot_services_started, &expected, true)) return;
    if (remote_control_toggle_img)
        lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    refresh_remote_control_screen_if_built();

    pthread_t thread;
    if (pthread_create(&thread, NULL, test_boot_services_worker, NULL) != 0) {
        atomic_store(&test_boot_services_started, false);
        DBG_LOG("test boot RC: could not create startup worker\n");
        return;
    }
    pthread_detach(thread);
}
#endif

/* gui_network_toggle_airplay()'s twin for the phone remote-control server --
 * shown as "RC" on the quick drawer's own tile, where there is no room for
 * the full name. */
bool gui_network_toggle_remote_control(void) {
    if (!MT_HAS_FEATURE(REMOTE)) return false;
    bool turning_on = !current_settings.remote_control_enabled;
    if (network_service_is_busy(NETWORK_SERVICE_REMOTE) ||
        !network_service_enqueue(NETWORK_SERVICE_REMOTE, turning_on, false)) {
        show_info_toast(TR("Service is busy"));
        refresh_remote_control_screen_if_built();
        return current_settings.remote_control_enabled;
    }
    current_settings.remote_control_enabled = turning_on;
    settings_save_async(&current_settings);

    /* Real lv_switch now (see build_remote_control_screen()'s own comment)
     * -- CHECKED state alone drives its visual, no sprite swap needed. */
    if (remote_control_toggle_img) {
        if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
        else lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
    }
    refresh_remote_control_screen_if_built();
    return current_settings.remote_control_enabled;
}

static void remote_control_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_remote_control();
}

/* ---- New-PIN confirmation for the PIN row's refresh icon. Regenerating
 * immediately locks out every app and browser still using the old PIN, so it
 * asks first, like the other disruptive actions in this module
 * (gui_bluetooth.c's bt_dac_leave_popup). Built once in
 * gui_network_init() on lv_layer_top(), torn down in gui_network_teardown(). */
static gui_popup_t remote_control_new_pin_popup;

static void remote_control_new_pin_popup_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
}

static void remote_control_new_pin_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
    if (!remote_control_generate_new_pin()) {
        show_info_toast(TR("Could not generate a new PIN"));
        return;
    }
    remote_control_refresh_pin_label();
    show_info_toast(TR("New PIN generated"));
}

static void remote_control_regenerate_pin_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&remote_control_new_pin_popup);
}

void build_remote_control_new_pin_popup(void) {
    remote_control_new_pin_popup.popup = build_confirm_popup(
        TR("Generate a new PIN?"), LV_LABEL_LONG_WRAP, NULL,
        TR("Apps and browsers using the current PIN will need the new one to reconnect."), TR("Generate"),
        lv_color_make(255, 120, 120), remote_control_new_pin_confirm_cb, NULL, TR("Cancel"), accent_lv_color(),
        remote_control_new_pin_popup_dismiss_cb, NULL, remote_control_new_pin_popup_dismiss_cb,
        &remote_control_new_pin_popup.backdrop);
}

/* Standard settings-list layout (build_subsonic_list_screen(), the same
 * flex column as the Wi-Fi and Bluetooth screens): the on/off toggle row,
 * then the PIN row with a refresh icon to replace the PIN. The connection
 * details follow as wrapped text lines. */
static lv_obj_t * build_remote_control_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Remote Control"), &title_label, &list);

    lv_obj_t * toggle_row = add_pill_toggle_row(list, TR("Remote Control"), current_settings.remote_control_enabled,
                                                remote_control_toggle_cb);
    /* add_pill_toggle_row() creates the label first and the switch last;
     * gui_network_toggle_remote_control() drives this switch's state. */
    remote_control_toggle_img = lv_obj_get_child(toggle_row, -1);

    lv_obj_t * pin_row = add_pill_row_base(list, TR("Connection PIN"));

    /* Refresh icon at the row's trailing edge (same -20 inset as the toggle
     * and chevron rows) regenerates the PIN; LV_SYMBOL_REFRESH comes from the
     * Montserrat fallback chain behind the app font. The glyph is small, so
     * widen its touch area. */
    lv_obj_t * regenerate_icon = lv_label_create(pin_row);
    lv_label_set_text(regenerate_icon, LV_SYMBOL_REFRESH);
    lv_obj_add_style(regenerate_icon, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(regenerate_icon, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(regenerate_icon, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(regenerate_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(regenerate_icon, BOARD_SCALE_PX(20));
    lv_obj_add_event_cb(regenerate_icon, remote_control_regenerate_pin_cb, LV_EVENT_CLICKED, NULL);

    /* The PIN sits just left of the icon. lv_obj_align() (unlike
     * lv_obj_align_to()) keeps the right-edge anchor as the text changes
     * between a PIN and "Unavailable". */
    lv_obj_update_layout(regenerate_icon);
    int32_t icon_width = lv_obj_get_width(regenerate_icon);
    remote_control_pin_label = lv_label_create(pin_row);
    lv_obj_add_style(remote_control_pin_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remote_control_pin_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(remote_control_pin_label, LV_ALIGN_RIGHT_MID, -20 - icon_width - BOARD_SCALE_PX(16), 0);
    remote_control_displayed_pin_valid = false;
    remote_control_refresh_pin_label();

    lv_obj_t * explanation = lv_label_create(list);
    lv_label_set_text(explanation,
                      TR("Connect over Wi-Fi or Bluetooth to see what's playing, control playback, and browse "
                      "your library. Enter this PIN when the app or browser asks for it; Bluetooth also "
                      "requires pairing."));
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(explanation, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);

    remote_control_status_label = lv_label_create(list);
    lv_obj_set_width(remote_control_status_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

#if LV_USE_QRCODE
    remote_control_qrcode = lv_qrcode_create(list);
    lv_qrcode_set_size(remote_control_qrcode, BOARD_SCALE_PX(220));
    lv_qrcode_set_dark_color(remote_control_qrcode, lv_color_black());
    lv_qrcode_set_light_color(remote_control_qrcode, lv_color_white());
    lv_obj_set_style_border_width(remote_control_qrcode, 4, 0);
    lv_obj_set_style_border_color(remote_control_qrcode, lv_color_white(), 0);
    lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif

    remote_control_url_label = lv_label_create(list);
    lv_obj_set_width(remote_control_url_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_url_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(remote_control_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    remote_control_bluetooth_label = lv_label_create(list);
    lv_obj_set_width(remote_control_bluetooth_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_bluetooth_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_bluetooth_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_bluetooth_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_bluetooth_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_pad_bottom(remote_control_bluetooth_label, BOARD_SCALE_PX(24), 0);
    /* open_remote_control_screen() fills these in every time the screen
     * opens; hide the still-empty lines until then. */
    remote_control_hide_empty_lines();

    return scr;
}

static void open_remote_control_screen(void) {
    wifi_settings_snapshot_invalidate();
    gui_wifi_set_snapshot_enabled(true);
    remote_control_refresh_address();
    nav_push(remote_control_screen);
}

void remote_control_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_remote_control_screen();
}

void gui_remote_open_screen(void) {
    open_remote_control_screen();
}

/* poll_wifi_scan()'s every-poll refresh of this screen (gui_wifi.c). */
void remote_control_poll_screen(void) {
    if (FEATURE_SCREEN(REMOTE, remote_control_screen) && gui_navigation_is_top(remote_control_screen))
        remote_control_refresh_pin_label();
#ifndef HOST_BUILD
    /* These snapshots are updated by the service thread / regular shell poll;
     * never query bluetoothd from the UI thread. */
    bt_remote_control_status_t bt_status = bt_remote_control_status();
    if (FEATURE_SCREEN(REMOTE, remote_control_screen) && gui_navigation_is_top(remote_control_screen) &&
        (remote_control_last_bt_powered != bt_is_powered_cached ||
         remote_control_last_bt_status != bt_status)) {
        remote_control_last_bt_powered = bt_is_powered_cached;
        remote_control_last_bt_status = bt_status;
        remote_control_refresh_address();
    }
#endif
}

void gui_remote_init(void) {
    remote_control_screen = build_remote_control_screen();
}

void gui_remote_teardown_widgets(void) {
    gui_popup_teardown(&remote_control_new_pin_popup);

    /* The Remote Control screen's widgets die with remote_control_screen
     * (gui_remote_teardown_screen()); clear the pointers so the quick-drawer
     * toggle and the periodic poll (which check them) can't touch freed
     * objects before a rebuild. */
    remote_control_toggle_img = NULL;
    remote_control_pin_label = NULL;
    remote_control_displayed_pin_valid = false;
    remote_control_status_label = NULL;
    remote_control_url_label = NULL;
    remote_control_bluetooth_label = NULL;
#if LV_USE_QRCODE
    remote_control_qrcode = NULL;
#endif
}

void gui_remote_teardown_screen(void) {
    if (remote_control_screen) { lv_obj_delete(remote_control_screen); remote_control_screen = NULL; }
}

lv_obj_t * gui_remote_get_screen(void) { return remote_control_screen; }
