/* Internet radio: the Radio shortcut on the clock home.
 *
 * The list comes from radio_list.c: the read-only base list in
 * the install root minus the stations you removed, then your own stations
 * from Radio.txt on the SD card, in the NetRadio plugin's format:
 *
 *   Station Name | http://example.com/stream.mp3
 *
 * It is re-read every time the screen is shown. Tap plays; a long press
 * removes a station (a built-in one is hidden, and "Show removed stations"
 * brings it back). Playback goes through the ordinary http stream path
 * (MP3, FLAC, ADTS AAC), so it follows whatever output is active, BT
 * included. */
#include "board_config.h"
#include "gui_radio.h"
#include "gui.h"
#include "i18n.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "screen_builders.h"
#include "gui_navigation.h"
#include "gui_notifications.h"
#include "plugin_manager.h"
#include "radio_list.h"
#include "utf8_util.h"

extern lv_color_t accent_lv_color(void);
#include <stdint.h>

/* Rows shown at once; the list file itself may hold more. */
#define RADIO_MAX_STATIONS 256

static lv_obj_t * radio_screen;
static lv_obj_t * radio_title_label;
static lv_obj_t * radio_list;

static lv_obj_t * removed_screen;
static lv_obj_t * removed_title_label;
static lv_obj_t * removed_list;

/* The shown stations (also used to name a playing stream). */
static radio_entries_t stations;
static char * station_urls[RADIO_MAX_STATIONS];
static int station_count;

/* Hidden base stations, for the "removed" screen. */
static radio_entries_t removed;

static gui_popup_t remove_popup;
static lv_obj_t * remove_popup_title;
static char remove_url[RADIO_URL_MAX];

static void load_stations(void) {
    radio_entries_free(&stations);
    station_count = 0;
    if (!radio_list_load(&stations, false)) return;
    for (int i = 0; i < stations.count && station_count < RADIO_MAX_STATIONS; i++)
        station_urls[station_count++] = stations.items[i].url;
}

bool gui_radio_station_name(const char * url, char * out, size_t out_size) {
    if (!url || !out || out_size == 0 || !radio_url_is_http(url)) return false;
    for (int i = 0; i < station_count; i++) {
        if (strcmp(stations.items[i].url, url) == 0) {
            utf8_truncate_safe(out, stations.items[i].name, out_size);
            return true;
        }
    }
    return false;
}

static void populate_radio_screen(void);
static void populate_removed_screen(void);

static void station_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= station_count) return;
    gui_plugin_play_paths((const char * const *) station_urls, station_count, index);
}

/* ---- remove (long press) ------------------------------------------------- */

static void remove_popup_close_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remove_popup);
}

static void remove_popup_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remove_popup);
    radio_result_t r = radio_list_remove(remove_url);
    if (r == RADIO_OK) show_info_toast(TR("Station removed"));
    else if (r == RADIO_NO_CARD) show_error_toast(TR("Insert a writable SD card to change the Radio list"));
    else show_error_toast(TR("Could not change the Radio list"));
    populate_radio_screen();
}

static void station_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= station_count) return;
    if (!radio_list_editable()) {
        show_error_toast(TR("Insert a writable SD card to change the Radio list"));
        return;
    }
    snprintf(remove_url, sizeof(remove_url), "%s", stations.items[index].url);
    if (!remove_popup.popup) {
        remove_popup.popup = build_confirm_popup(
            "", LV_LABEL_LONG_WRAP, &remove_popup_title, NULL, TR("Remove"), lv_color_make(255, 120, 120),
            remove_popup_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), remove_popup_close_cb, NULL,
            remove_popup_close_cb, &remove_popup.backdrop);
    }
    char title[RADIO_NAME_MAX + 64];
    snprintf(title, sizeof(title), TR("Remove %s?"), stations.items[index].name);
    if (remove_popup_title) lv_label_set_text(remove_popup_title, title);
    gui_popup_show(&remove_popup);
}

/* ---- rows ------------------------------------------------------------------ */

static void plugin_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    plugin_manager_radio_list_item_clicked((int) (intptr_t) lv_event_get_user_data(e));
}

static void show_removed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!removed_screen)
        removed_screen = build_subsonic_list_screen(TR("Removed stations"), &removed_title_label, &removed_list);
    populate_removed_screen();
    nav_push(removed_screen);
}

static lv_obj_t * radio_row(lv_obj_t * list, const char * text, lv_event_cb_t cb, lv_event_code_t code, int index,
                            bool accent) {
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    if (accent) lv_obj_set_style_text_color(label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_set_width(label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, code, (void *) (intptr_t) index);
    return row;
}

static void populate_radio_screen(void) {
    if (!radio_screen) return;
    lv_obj_clean(radio_list);
    lv_label_set_text(radio_title_label, TR("Radio"));

    /* Plugin rows ("radio" list id) first: station finders and directories. */
    int plugin_rows = plugin_manager_get_radio_list_item_count();
    for (int i = 0; i < plugin_rows; i++) {
        char text[192];
        snprintf(text, sizeof(text), LV_SYMBOL_PLUS "  %s", plugin_manager_get_radio_list_item_label(i));
        radio_row(radio_list, text, plugin_row_cb, LV_EVENT_CLICKED, i, true);
    }

    load_stations();
    for (int i = 0; i < station_count; i++) {
        lv_obj_t * row = radio_row(radio_list, stations.items[i].name, station_row_cb, LV_EVENT_SHORT_CLICKED, i, false);
        lv_obj_add_event_cb(row, station_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
    }

    int hidden = radio_list_hidden_count();
    if (hidden > 0) {
        char text[96];
        snprintf(text, sizeof(text), TR("Show removed stations (%d)"), hidden);
        radio_row(radio_list, text, show_removed_cb, LV_EVENT_CLICKED, 0, true);
    }

    if (station_count == 0 && !plugin_rows && hidden == 0) {
        build_list_message(radio_list, TR("No stations"),
                           TR("Put Radio.txt in the SD card root: one \"Name | http://stream-url\" per line."));
    }
}

/* ---- removed stations ------------------------------------------------------- */

static void restore_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= removed.count) return;
    radio_result_t r = radio_list_restore(removed.items[index].url);
    if (r == RADIO_OK) show_info_toast(TR("Station restored"));
    else if (r == RADIO_NO_CARD) show_error_toast(TR("Insert a writable SD card to change the Radio list"));
    else show_error_toast(TR("Could not change the Radio list"));
    populate_removed_screen();
}

static void populate_removed_screen(void) {
    if (!removed_screen) return;
    lv_obj_clean(removed_list);
    lv_label_set_text(removed_title_label, TR("Removed stations"));
    radio_entries_t all = { 0 };
    radio_entries_free(&removed);
    if (radio_list_load(&all, true)) {
        for (int i = 0; i < all.count; i++) {
            if (!all.items[i].hidden) continue;
            radio_entries_t * r = &removed;
            if (r->count == r->cap) {
                int cap = r->cap ? r->cap * 2 : 16;
                radio_station_t * grown = realloc(r->items, (size_t) cap * sizeof(*grown));
                if (!grown) break;
                r->items = grown;
                r->cap = cap;
            }
            /* Moves the strings over; all.items[i] no longer owns them. */
            r->items[r->count++] = all.items[i];
            all.items[i].name = all.items[i].url = NULL;
        }
    }
    radio_entries_free(&all);
    for (int i = 0; i < removed.count; i++) {
        char text[RADIO_NAME_MAX + 32];
        snprintf(text, sizeof(text), LV_SYMBOL_REFRESH "  %s", removed.items[i].name);
        radio_row(removed_list, text, restore_row_cb, LV_EVENT_CLICKED, i, false);
    }
    if (removed.count == 0)
        build_list_message(removed_list, TR("No removed stations"), TR("Every built-in station is shown."));
}

/* Back from a plugin screen or the removed list: show what changed. */
static void radio_loaded_cb(lv_event_t * e) {
    (void) e;
    populate_radio_screen();
}

void gui_radio_open(void) {
    if (!radio_screen) {
        radio_screen = build_subsonic_list_screen(TR("Radio"), &radio_title_label, &radio_list);
        lv_obj_add_event_cb(radio_screen, radio_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    }
    populate_radio_screen();
    nav_push(radio_screen);
}

void gui_radio_teardown(void) {
    gui_popup_teardown(&remove_popup);
    remove_popup_title = NULL;
    if (removed_screen) { lv_obj_delete(removed_screen); removed_screen = NULL; }
    if (radio_screen) { lv_obj_delete(radio_screen); radio_screen = NULL; }
}
