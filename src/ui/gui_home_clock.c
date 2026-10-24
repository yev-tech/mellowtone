/* Clock-face home screen (every board): large time and date, a
 * status line (battery, Bluetooth headphones and codec), the current track
 * with play/pause, and four round shortcuts. Laid out on the 360x480 grid of
 * the R3 Pro's 360x480 panel, scaled to the panel with BOARD_UI_PX()/_PY().
 *
 * Kept current by the same hooks that feed the quick drawer (track text,
 * cover, play state; see gui_shell.c) plus a 1 s timer for the clock and the
 * status line, which only touches labels whose text changed. */
#include "board_config.h"

#include "gui_home_clock.h"
#include "gui_shell.h"
#include "gui.h"
#include "gui_player.h"
#include "gui_navigation.h"
#include "i18n.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include "assets.h"
#include "app_clock.h"
#include "battery.h"
#include "bluetooth_control.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

extern player_settings_t current_settings;
extern char bt_connected_mac_cached[18];
extern char bt_connected_codec_cached[32];
extern unsigned int bt_connected_rate_cached;
extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
void gui_settings_open_home_destination(const char * key);

#define CARD_COVER_PX BOARD_UI_PX(52)
#define CARD_BUTTON_PX BOARD_UI_PX(44)
#define CARD_W BOARD_UI_PX(312)
#define CARD_H BOARD_UI_PX(76)
#define CARD_INSET BOARD_UI_PX(12)

static lv_obj_t * home_screen;
static lv_obj_t * time_label;
static lv_obj_t * date_label;
static lv_obj_t * status_label;
static lv_obj_t * card;
static lv_obj_t * cover_img;
static lv_obj_t * cover_placeholder;
static lv_obj_t * title_label;
static lv_obj_t * artist_label;
static lv_obj_t * play_btn;
static lv_obj_t * play_icon;
static lv_timer_t * tick_timer;
static bool has_track;
static bool is_playing;

static void set_text_if_changed(lv_obj_t * label, const char * text) {
    if (!label) return;
    const char * cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

/* "WH-1000XM4 · LDAC 44.1 kHz" from the cached BT poll; name from the last
 * device list, falling back to "Bluetooth". */
static void format_bt(char * out, size_t size) {
    out[0] = '\0';
    if (!bt_connected_mac_cached[0]) return;
    char known[64];
    const char * name = gui_shell_connected_bt_name(known, sizeof(known)) ? known : TR("Bluetooth");
    for (int i = 0; i < bt_scan_result_count && name != known; i++)
        if (strcasecmp(bt_scan_results[i].mac, bt_connected_mac_cached) == 0 && bt_scan_results[i].name[0]) {
            name = bt_scan_results[i].name;
            break;
        }
    char rate[16] = "";
    if (bt_connected_rate_cached) {
        unsigned int r = bt_connected_rate_cached;
        if (r % 1000) snprintf(rate, sizeof(rate), " %u.%u kHz", r / 1000, (r % 1000) / 100);
        else snprintf(rate, sizeof(rate), " %u kHz", r / 1000);
    }
    if (bt_connected_codec_cached[0])
        snprintf(out, size, "%s · %s%s", name, bt_connected_codec_cached, rate);
    else
        snprintf(out, size, "%s", name);
}

static void refresh_clock_and_status(void) {
    struct tm tm;
    app_clock_localtime(&tm);
    char buf[64];
    if (current_settings.clock_24h) strftime(buf, sizeof(buf), "%H:%M", &tm);
    else {
        int h = tm.tm_hour % 12;
        snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, tm.tm_min);
    }
    set_text_if_changed(time_label, buf);
    strftime(buf, sizeof(buf), "%A, %e %B", &tm);
    char * p = buf;
    char compact[64];
    /* %e pads single-digit days with a space: "Saturday,  3 October". */
    size_t n = 0;
    for (; *p && n + 1 < sizeof(compact); p++)
        if (!(*p == ' ' && n > 0 && compact[n - 1] == ' ')) compact[n++] = *p;
    compact[n] = '\0';
    set_text_if_changed(date_label, compact);

    char bt[128]; /* name 63 + " · " + codec 31 + rate 15 */
    format_bt(bt, sizeof(bt));
    int pct = battery_get_display_percent();
    char status[160];
    if (pct >= 0 && bt[0]) snprintf(status, sizeof(status), "%d %% · %s", pct, bt);
    else if (pct >= 0) snprintf(status, sizeof(status), "%d %%", pct);
    else snprintf(status, sizeof(status), "%s", bt);
    set_text_if_changed(status_label, status);
}

static void tick_cb(lv_timer_t * t) {
    (void) t;
    if (lv_screen_active() != home_screen) return; /* nothing visible to update */
    refresh_clock_and_status();
}

static void apply_card_state(void) {
    if (!card) return;
    if (has_track) {
        lv_obj_remove_flag(play_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_text_if_changed(title_label, TR("Nothing playing"));
        set_text_if_changed(artist_label, TR("Tap to browse music"));
        lv_obj_add_flag(play_btn, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(cover_img, NULL);
        lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(play_icon, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void gui_home_clock_set_track(const char * title, const char * artist) {
    has_track = title && title[0];
    if (has_track) {
        set_text_if_changed(title_label, title);
        set_text_if_changed(artist_label, artist ? artist : "");
    }
    apply_card_state();
}

/* The cover descriptor belongs to gui_player.c and is replaced or released
 * right after this hook runs for the drawer, so the source is always
 * re-pointed (or dropped) here at the same moment. */
void gui_home_clock_refresh_cover(void) {
    if (!cover_img) return;
    const lv_image_dsc_t * cover = has_track ? gui_player_get_current_cover_dsc() : NULL;
    if (cover && cover->data && cover->header.w > 0) {
        lv_image_set_src(cover_img, cover);
        lv_image_set_scale(cover_img, (uint32_t) (CARD_COVER_PX * LV_SCALE_NONE / cover->header.w));
        lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(cover_img, NULL);
        lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_home_clock_set_playing(bool playing) {
    is_playing = playing;
    if (play_icon) lv_label_set_text(play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void card_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (has_track) nav_push(gui_player_get_screen());
    else gui_settings_open_home_destination("music");
}

static void play_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
}

static void shortcut_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_home_destination((const char *) lv_event_get_user_data(e));
}

static lv_obj_t * make_label(lv_obj_t * parent, const lv_font_t * font, lv_color_t color) {
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static void add_shortcut(lv_obj_t * row, const char * key, const char * icon_asset, const char * label,
                         uint32_t tint) {
    lv_obj_t * col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, BOARD_UI_PX(72), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, BOARD_UI_PX(6), 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(col, shortcut_click_cb, LV_EVENT_CLICKED, (void *) key);

    lv_obj_t * ring = lv_obj_create(col);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, BOARD_UI_PX(56), BOARD_UI_PX(56));
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x2F3437), 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * icon = lv_image_create(ring);
    lv_image_set_src(icon, asset_path(icon_asset));
    /* Launcher icons are 140 px; draw at 28 px, tinted like the mockup. */
    lv_image_set_scale(icon, BOARD_UI_PX(28) * LV_SCALE_NONE / 140);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(tint), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);

    lv_obj_t * name = make_label(col, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), lv_color_hex(GUI_COLOR_TEXT_MUTED));
    lv_label_set_text(name, label);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
}

/* A UI reload deletes Home before the other screens are rebuilt; forget the
 * widgets so no report made meanwhile can reach them (gui_shell.c re-applies
 * the current track to the new Home). A theme refresh builds the new Home
 * first, so only the current one clears. */
static void home_deleted_cb(lv_event_t * e) {
    if (lv_event_get_target(e) != home_screen) return;
    home_screen = NULL;
    time_label = date_label = status_label = NULL;
    card = cover_img = cover_placeholder = NULL;
    title_label = artist_label = play_btn = play_icon = NULL;
}

lv_obj_t * gui_home_clock_build(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    home_screen = scr;
    lv_obj_add_event_cb(scr, home_deleted_cb, LV_EVENT_DELETE, NULL);

    time_label = make_label(scr, fallback_font_status_face(BOARD_UI_PX(84)), lv_color_hex(GUI_COLOR_TEXT));
    lv_obj_set_width(time_label, BOARD_SCREEN_WIDTH);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(40));

    date_label = make_label(scr, gui_theme_font(GUI_FONT_ROLE_ROW), lv_color_hex(GUI_COLOR_TEXT_SOFT));
    lv_obj_set_width(date_label, BOARD_SCREEN_WIDTH);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(146));

    status_label = make_label(scr, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), lv_color_hex(GUI_COLOR_TEXT_DIM));
    lv_obj_set_width(status_label, BOARD_UI_PX(320));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(174));

    /* Now-playing card */
    card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(208));
    lv_obj_set_style_radius(card, BOARD_UI_PX(16), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(GUI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);

    cover_placeholder = lv_obj_create(card);
    lv_obj_remove_style_all(cover_placeholder);
    lv_obj_set_size(cover_placeholder, CARD_COVER_PX, CARD_COVER_PX);
    lv_obj_align(cover_placeholder, LV_ALIGN_LEFT_MID, CARD_INSET, 0);
    lv_obj_set_style_radius(cover_placeholder, BOARD_UI_PX(6), 0);
    lv_obj_set_style_bg_color(cover_placeholder, lv_color_hex(GUI_COLOR_CONTROL), 0);
    lv_obj_set_style_bg_opa(cover_placeholder, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cover_placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * note = lv_label_create(cover_placeholder);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(note, lv_color_hex(GUI_COLOR_TEXT_FAINT), 0);
    lv_obj_center(note);

    cover_img = lv_image_create(card);
    lv_obj_set_size(cover_img, CARD_COVER_PX, CARD_COVER_PX);
    lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(cover_img, LV_ALIGN_LEFT_MID, CARD_INSET, 0);
    lv_obj_set_style_radius(cover_img, BOARD_UI_PX(6), 0);
    lv_obj_set_style_clip_corner(cover_img, true, 0);
    lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);

    int32_t text_x = CARD_INSET + CARD_COVER_PX + CARD_INSET;
    int32_t text_w = CARD_W - text_x - CARD_BUTTON_PX - BOARD_UI_PX(20);
    const lv_font_t * title_font = gui_theme_font(GUI_FONT_ROLE_ROW);
    const lv_font_t * artist_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t title_h = lv_font_get_line_height(title_font);
    int32_t artist_h = lv_font_get_line_height(artist_font);
    int32_t text_top = (CARD_H - title_h - artist_h) / 2;
    title_label = make_label(card, title_font, lv_color_hex(GUI_COLOR_TEXT));
    lv_obj_set_size(title_label, text_w, title_h);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, text_x, text_top);
    artist_label = make_label(card, artist_font, lv_color_hex(GUI_COLOR_TEXT_MUTED));
    lv_obj_set_size(artist_label, text_w, artist_h);
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
    lv_obj_align(artist_label, LV_ALIGN_TOP_LEFT, text_x, text_top + title_h);

    play_btn = lv_obj_create(card);
    lv_obj_remove_style_all(play_btn);
    lv_obj_set_size(play_btn, CARD_BUTTON_PX, CARD_BUTTON_PX);
    lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, -CARD_INSET, 0);
    lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_style(play_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(play_btn, BOARD_UI_PX(8));
    lv_obj_add_event_cb(play_btn, play_click_cb, LV_EVENT_CLICKED, NULL);
    play_icon = lv_label_create(play_btn);
    lv_obj_set_style_text_color(play_icon, lv_color_hex(GUI_COLOR_ON_ACCENT), 0);
    lv_obj_center(play_icon);

    /* Shortcuts */
    lv_obj_t * row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, CARD_W, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(324));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    add_shortcut(row, "music", "launcher/music.png", TR("Music"), 0xF5B457);
    add_shortcut(row, "radio", "launcher/radio.png", TR("Radio"), 0xF08A5D);
    add_shortcut(row, "stream_media", "launcher/stream_media.png", TR("Stream"), 0x48ADE5);
    add_shortcut(row, "settings", "launcher/sys_set.png", TR("Settings"), 0x909EB5);

    lv_obj_t * hint = make_label(scr, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), lv_color_hex(GUI_COLOR_TEXT_HINT));
    lv_label_set_text(hint, TR("Swipe down for Wi-Fi and Bluetooth"));
    lv_obj_set_width(hint, BOARD_SCREEN_WIDTH);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, BOARD_UI_PY(438));

    apply_card_state();
    refresh_clock_and_status();
    if (!tick_timer) tick_timer = lv_timer_create(tick_cb, 1000, NULL);
    return scr;
}

void gui_home_clock_set_screen_on(bool on) {
    if (!tick_timer) return;
    if (on) {
        lv_timer_resume(tick_timer);
        lv_timer_ready(tick_timer);
    } else {
        lv_timer_pause(tick_timer);
    }
}

/* The screen is rebuilt on a UI reload; the timer is reused (it only acts
 * while home_screen is the active screen). */
