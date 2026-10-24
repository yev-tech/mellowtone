/* DLNA renderer (optional feature DLNA_RENDERER): the DLNA screen and its
 * toggle. Part of the network module (gui_network.c). */
#include "board.h"
#include "mt_features.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "screen_builders.h"
#include "settings.h"

static lv_obj_t * dlna_screen;

/* ---- DLNA screen -- toggles the DLNA DMR service (dmrd) to allow UPnP/DLNA
 * controllers on the local Wi-Fi network to stream to this device. ---- */

static lv_obj_t * dlna_list;

static void dlna_toggle_cb(lv_event_t * e);

void populate_dlna_screen(void) {
    lv_obj_clean(dlna_list);
    add_pill_toggle_row(dlna_list, TR("DLNA Renderer"), current_settings.dlna_renderer_enabled, dlna_toggle_cb);

    lv_obj_t * explanation = lv_label_create(dlna_list);
    lv_label_set_text(explanation,
                      TR("When on, this device is visible to DLNA/UPnP controller apps on your Wi-Fi network -- cast "
                      "a track from one to play it here. Pause, mute, volume, and seek from the controller app "
                      "aren't supported; use this device's own controls instead once a track starts."));
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
}

static void refresh_dlna_screen_if_built(void) {
    if (dlna_list) populate_dlna_screen();
}

/* gui_network_toggle_airplay()'s twin for DLNA -- same reason the drawer
 * routes through it rather than setting the flag itself. */
bool gui_network_toggle_dlna(void) {
    if (!MT_HAS_FEATURE(DLNA_RENDERER)) return false;
    bool turning_on = !current_settings.dlna_renderer_enabled;
    /* Same defensive enable guard as airplay_toggle_cb() -- this screen can
     * already be open when Wi-Fi is disabled elsewhere, so the tile guard
     * alone (dlna_tile_cb() below) isn't sufficient. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    if (network_service_is_busy(NETWORK_SERVICE_DLNA) ||
        !network_service_enqueue(NETWORK_SERVICE_DLNA, turning_on, false)) {
        show_info_toast(TR("Service is busy"));
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    current_settings.dlna_renderer_enabled = turning_on;
    settings_save_async(&current_settings);

    refresh_dlna_screen_if_built();
    return current_settings.dlna_renderer_enabled;
}

static void dlna_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_dlna();
}

static lv_obj_t * build_dlna_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("DLNA"), &title_label, &dlna_list);
}

static void open_dlna_screen(void) {
    populate_dlna_screen();
    nav_push(dlna_screen);
}

void dlna_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_dlna_screen();
}

void gui_dlna_init(void) {
    dlna_screen = build_dlna_screen();
}

void gui_dlna_teardown(void) {
    if (dlna_screen) { lv_obj_delete(dlna_screen); dlna_screen = NULL; }
}
