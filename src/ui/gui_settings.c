#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "storage_paths.h"
#include "r3pro_stock_ini.h"
#include "idle_shutdown.h"
#include "power_suspend.h"
#include "gui_setup.h"
#include "gui.h"
#include "gui_settings.h"
#include "gui_stream_home.h"
#include "eq_presets.h"
#include "i18n.h"
#include "hw_buttons.h"
#include "gui_library.h"
#include "bluetooth_control.h"
#include "gui_shell.h"
#include "gui_text_input.h"
#include "app_clock.h"
#include "app_version.h"
#include <math.h>
#include <sys/stat.h>
#include "gui_subsonic.h"
#include "gui_radio.h"
#include "gui_network.h"
#include "subprocess.h"
#include "timezone_apply.h"
#include "gui_lyrics.h"
#include "screen_builders.h"
#include "settings.h"
#include "tagcache.h"
#include "metadata_db.h"
#include "assets.h"
#include "metadata.h"
#include "audio.h"
#include "peq.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "charge_control.h"
#include "timezone_data.h"
#include "launcher_client.h"
#include "plugin_manager.h"
#include "gui_plugin_manage.h"
#include "fallback_font.h"
#include "gui_navigation.h"
#include "gui_player.h"
#include "gui_reload.h"
#include "db_log.h"
#include "usb_dac_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
/* Developer Options > Enable debug logging, where the boot launcher reads a
 * flag (BOARD.paths.launcher_debug_flag, R3 Pro): the player keeps its flag
 * in config/ on the card. The boot launcher and bt_init still read the old
 * flag in flash (bluetoothd -d, power.log) until the new launcher reads the
 * card's; switching off also removes that one. */
#define LAUNCHER_DEBUG_FLAG MT_DEBUG_FLAG_FILE
#include "backlight.h"
#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR BOARD_SD_ROOT ""
#endif
#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR "/PEQ_Profiles"
#include <stdatomic.h>
#include <pthread.h>

/* Extern references to screen pointers owned by this module (defined here) */
static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_gapless_toggle_img = NULL;
static lv_obj_t * settings_screen;
static lv_obj_t * settings_library_screen;
static lv_obj_t * settings_sorting_screen;
static lv_obj_t * settings_sorting_options[5];
static lv_obj_t * settings_sound_effects_screen;
static lv_obj_t * settings_eq_profiles_menu_screen;
static lv_obj_t * settings_appearance_screen;
static lv_obj_t * settings_player_layout_screen;
static lv_obj_t * settings_gestures_screen;
static lv_obj_t * settings_charging_screen;
static lv_obj_t * settings_maintenance_screen;
static lv_obj_t * settings_system_maintenance_screen;
static lv_obj_t * settings_tools_screen;
static lv_obj_t * music_playback_screen;
static lv_obj_t * music_audio_screen;
static lv_obj_t * music_controls_screen;
static lv_obj_t * settings_display_screen;
static lv_obj_t * animation_speed_screen;
static lv_obj_t * animation_speed_list;
static lv_obj_t * language_choice_screen;
static lv_obj_t * language_choice_list;
static lv_obj_t * settings_power_screen;
static lv_obj_t * settings_system_screen;
static lv_obj_t * about_screen;
static lv_obj_t * dev_options_screen;
static lv_obj_t * accent_color_screen;
static lv_obj_t * custom_font_screen;
static lv_obj_t * screen_timeout_screen;
static lv_obj_t * screen_dimming_screen;
static lv_obj_t * screen_dimming_switch;
static lv_obj_t * screen_dimming_slider_card;
static lv_obj_t * screen_dimming_slider;
static lv_obj_t * screen_dimming_value_label;
static lv_obj_t * startup_volume_screen;
static lv_obj_t * sleep_timer_screen;
static lv_obj_t * idle_shutdown_screen;
static lv_obj_t * clock_screen;
static lv_obj_t * clock_set_time_screen;
static lv_obj_t * clock_hour_roller;
static lv_obj_t * clock_minute_roller;
static lv_obj_t * clock_ampm_roller;
static lv_obj_t * clock_set_time_row;
static lv_obj_t * clock_timezone_row;
static lv_obj_t * clock_timezone_value_label;
static lv_obj_t * eq_screen;
static lv_obj_t * eq_profiles_screen;
static char eq_current_profile_name[256];
static void eq_profile_display_name(const char * name, char * out, size_t size) {
    if (!name || !name[0]) name = TR("Custom");
    if (strncmp(name, "AutoEQ - ", 9) == 0) {
        snprintf(out, size, "%s", name + 9);
        char * suffix = strrchr(out, ' ');
        if (suffix && suffix >= out + 2 && suffix[-1] == '-' && suffix[-2] == ' ' &&
            strlen(suffix + 1) == 10 && strspn(suffix + 1, "0123456789abcdefABCDEF") == 10)
            suffix[-2] = '\0';
    } else snprintf(out, size, "%s", name);
}

static void style_settings_dropdown(lv_obj_t * dropdown) {
    lv_obj_add_style(dropdown, &style_theme_card_bg, 0);
    lv_obj_add_style(dropdown, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_all(dropdown, BOARD_SCALE_PX(16), 0);
    lv_obj_t * list = lv_dropdown_get_list(dropdown);
    lv_obj_add_style(list, &style_theme_card_bg, 0);
    lv_obj_add_style(list, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(list, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(list, gui_theme_accent_style(), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, lv_color_black(), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_pad_ver(list, BOARD_SCALE_PX(12), 0);
}

static lv_obj_t * settings_eq_summary;
static void eq_graphic_current_name(char * out, size_t size);
static lv_obj_t * settings_sleep_summary;

static void settings_summary_loaded_cb(lv_event_t * e) {
    (void) e;
    if (settings_eq_summary) {
        char name[256];
        eq_graphic_current_name(name, sizeof(name)); /* names a matching preset */
        lv_label_set_text_fmt(settings_eq_summary, "%s · %s", peq_get_bypass() ? TR("Off") : TR("On"), name);
    }
    if (settings_sleep_summary) {
        int seconds = quick_drawer_sleep_timer_remaining_seconds();
        if (quick_drawer_sleep_timer_is_active())
            lv_label_set_text_fmt(settings_sleep_summary, TR("%d min remaining"), (seconds + 59) / 60);
        else lv_label_set_text(settings_sleep_summary, TR("Off"));
    }
}

static lv_obj_t * settings_add_summary(lv_obj_t * row) {
    lv_obj_update_layout(row);
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), -BOARD_SCALE_PX(16));
    lv_obj_t * label = lv_label_create(row);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(label, lv_obj_get_width(row) - BOARD_SCALE_PX(84));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), BOARD_SCALE_PX(20));
    int32_t needed = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) +
                     lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + BOARD_SCALE_PX(48);
    if (lv_obj_get_height(row) < needed) lv_obj_set_height(row, needed);
    return label;
}
static lv_obj_t * build_library_category_screen(void);
static void gain_settings_row_cb(lv_event_t * e);
static lv_obj_t * build_music_controls_screen(void);

/* Externs to gui.c functions and state this module needs */
extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * stream_media_screen;
extern lv_obj_t * gui_network_get_wireless_screen();
extern lv_obj_t * gui_network_get_usb_dac_overlay();
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);
extern void generic_back_cb(lv_event_t * e);
extern void start_library_rescan(void);
extern void show_library_refresh_all_metadata_prompt(void);
extern void show_library_refresh_all_covers_prompt(void);

static lv_obj_t * screen_timeout_switch;
static lv_obj_t * screen_timeout_slider_card;
static lv_obj_t * screen_timeout_value_label;
static lv_obj_t * screen_timeout_slider;
static lv_obj_t * startup_volume_switch;
static lv_obj_t * startup_volume_slider_card;
static lv_obj_t * startup_volume_value_label;
static lv_obj_t * startup_volume_slider;
static lv_obj_t * sleep_timer_switch;
static lv_obj_t * sleep_timer_slider_card;
static lv_obj_t * sleep_timer_remaining_btn;
static lv_obj_t * sleep_timer_value_label;
static lv_obj_t * sleep_timer_slider;
static lv_obj_t * idle_shutdown_switch;
static lv_obj_t * idle_shutdown_slider_card;
static lv_obj_t * idle_shutdown_value_label;
static lv_obj_t * idle_shutdown_slider;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;
static lv_obj_t * eq_bypass_switch;
static lv_obj_t * eq_band_enabled_switch;
static lv_obj_t * eq_preamp_slider;
static lv_obj_t * eq_preamp_value_label;
static lv_obj_t * eq_freq_slider;
static lv_obj_t * eq_gain_slider;
static lv_obj_t * eq_q_slider;
static lv_obj_t * eq_type_dropdown;
static lv_obj_t * eq_freq_value_label;
static lv_obj_t * eq_gain_value_label;
static lv_obj_t * eq_q_value_label;
static int current_eq_band = 0;
static lv_obj_t * eq_band_options_screen;
static lv_obj_t * eq_band_number_label;
static lv_obj_t * eq_profile_button_label;
static lv_obj_t * eq_main_content;
static lv_obj_t * eq_footer;
static lv_obj_t * eq_bypass_state_label;
static lv_obj_t * eq_title_label;
static bool eq_graph_visible;
static lv_obj_t * eq_graph_panel, *eq_graph_chart, *eq_graph_caption;
static lv_chart_series_t * eq_graph_series;
static lv_obj_t * eq_graph_min_label, *eq_graph_zero_label, *eq_graph_max_label;
static lv_obj_t * eq_band_options_row, *eq_freq_card, *eq_gain_card, *eq_q_card;
static void refresh_all_eq_widgets(void);
static bool eq_graphic_mode;
static void eq_graphic_refresh(void);
static void eq_graphic_current_name(char * out, size_t size);

void gui_settings_refresh_font_geometry(void) {
    if (eq_profile_button_label)
        lv_obj_set_height(eq_profile_button_label, lv_font_get_line_height(lv_obj_get_style_text_font(eq_profile_button_label, 0)));
    if (eq_screen) lv_obj_update_layout(eq_screen);
    if (eq_band_options_screen) lv_obj_update_layout(eq_band_options_screen);
    if (eq_title_label) lv_obj_set_style_text_font(eq_title_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
}

#define EQ_FREQ_MIN_HZ 20.0
#define EQ_FREQ_MAX_HZ 20000.0
#define EQ_FREQ_SLIDER_MAX 1000

static int32_t freq_to_slider(double freq_hz) {
    if (freq_hz < EQ_FREQ_MIN_HZ) freq_hz = EQ_FREQ_MIN_HZ;
    if (freq_hz > EQ_FREQ_MAX_HZ) freq_hz = EQ_FREQ_MAX_HZ;
    double ratio = log(freq_hz / EQ_FREQ_MIN_HZ) / log(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ);
    return (int32_t) (ratio * EQ_FREQ_SLIDER_MAX);
}

static double slider_to_freq(int32_t slider_val) {
    double ratio = (double) slider_val / (double) EQ_FREQ_SLIDER_MAX;
    return EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, ratio);
}

static void eq_format_gain(char * out, size_t out_size, double value) {
    double tenths = round(value * 10.0);
    if (fabs(value * 10.0 - tenths) < 0.0001) snprintf(out, out_size, "%+.1f dB", value);
    else snprintf(out, out_size, "%+.2f dB", value);
}

static void eq_format_q(char * out, size_t out_size, double value) {
    double tenths = round(value * 10.0);
    if (fabs(value * 10.0 - tenths) < 0.0001) snprintf(out, out_size, "%.1f", value);
    else snprintf(out, out_size, "%.2f", value);
}

/* Which numeric field a tap-to-edit label represents -- passed through
 * show_text_entry()'s user_data so one shared done-callback can update the
 * right slider/label/peq setter instead of needing four near-identical
 * callbacks. */
typedef enum {
    EQ_FIELD_PREAMP,
    EQ_FIELD_FREQ,
    EQ_FIELD_GAIN,
    EQ_FIELD_Q,
} eq_field_t;

static void eq_numeric_entry_done_cb(const char * text, void * user_data) {
    eq_field_t field = (eq_field_t) (intptr_t) user_data;
    double val = atof(text);
    char formatted[32];
    const peq_band_t * band;

    switch (field) {
        case EQ_FIELD_PREAMP:
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_preamp_db(val);
            if (eq_preamp_slider) lv_slider_set_value(eq_preamp_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            if (eq_preamp_value_label) lv_label_set_text_fmt(eq_preamp_value_label, TR("Pre-Amp: %+.2f dB"), val);
            peq_save();
            break;
        case EQ_FIELD_FREQ:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < EQ_FREQ_MIN_HZ) val = EQ_FREQ_MIN_HZ;
            if (val > EQ_FREQ_MAX_HZ) val = EQ_FREQ_MAX_HZ;
            peq_set_band(current_eq_band, val, band->gain_db, band->q);
            if (eq_freq_slider) lv_slider_set_value(eq_freq_slider, freq_to_slider(val), LV_ANIM_OFF);
            if (eq_freq_value_label) lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), val);
            peq_save();
            break;
        case EQ_FIELD_GAIN:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_band(current_eq_band, band->freq_hz, val, band->q);
            if (eq_gain_slider) lv_slider_set_value(eq_gain_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            eq_format_gain(formatted, sizeof(formatted), val);
            if (eq_gain_value_label) lv_label_set_text(eq_gain_value_label, formatted);
            peq_save();
            break;
        case EQ_FIELD_Q:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < 0.1) val = 0.1;
            if (val > 10.0) val = 10.0;
            peq_set_band(current_eq_band, band->freq_hz, band->gain_db, val);
            if (eq_q_slider) lv_slider_set_value(eq_q_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            eq_format_q(formatted, sizeof(formatted), val);
            if (eq_q_value_label) lv_label_set_text(eq_q_value_label, formatted);
            peq_save();
            break;
    }
}

/* Tap-to-edit: attached to each slider card's value label (see
 * create_eq_slider_card()) -- opens the shared numeric keypad pre-filled
 * with the field's current exact value, for typing a precise number
 * instead of only dragging a slider. */
static void eq_field_label_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_field_t field = (eq_field_t) (intptr_t) lv_event_get_user_data(e);
    const peq_band_t * band;
    char buf[32];
    const char * title;

    switch (field) {
        case EQ_FIELD_PREAMP:
            title = TR("Pre-Amp (dB, -12 to 12)");
            snprintf(buf, sizeof(buf), "%.2f", peq_get_preamp_db());
            break;
        case EQ_FIELD_FREQ:
            title = TR("Frequency (Hz, 20 to 20000)");
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.0f", band ? band->freq_hz : 0.0);
            break;
        case EQ_FIELD_GAIN:
            title = TR("Gain (dB, -12 to 12)");
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->gain_db : 0.0);
            break;
        case EQ_FIELD_Q:
        default:
            title = "Q (0.1 to 10)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->q : 0.0);
            break;
    }

    show_text_entry(title, buf, false, true, 0, eq_numeric_entry_done_cb, (void *) (intptr_t) field);
}

static void eq_screen_loaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) refresh_all_eq_widgets();
}

static void eq_screen_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(eq_screen);
}

static void eq_bypass_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Switch shows "EQ Enabled" -- checked means NOT bypassed. */
    peq_set_bypass(!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
    if (eq_bypass_state_label)
        lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    refresh_all_eq_widgets();
}

/* Modern EQ: band chips replace the arrows and the curve
 * pseudo-band; a summary card replaces the "Band options" link. */
static lv_obj_t * eq_band_chips[PEQ_NUM_BANDS + 1];
static lv_obj_t * eq_band_summary_label;
static void eq_show_stop(int stop);

static void eq_refresh_band_chips(void) {
    for (int i = 0; i <= PEQ_NUM_BANDS; i++) {
        lv_obj_t * chip = eq_band_chips[i];
        if (!chip) continue;
        bool selected = eq_graph_visible ? i == PEQ_NUM_BANDS : i == current_eq_band;
        const peq_band_t * b = i < PEQ_NUM_BANDS ? peq_get_band(i) : NULL;
        lv_obj_set_style_bg_color(chip, selected ? accent_lv_color() : lv_color_hex(GUI_COLOR_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0),
                                    selected ? lv_color_hex(GUI_COLOR_ON_ACCENT)
                                    : (b && !b->enabled) ? lv_color_hex(GUI_COLOR_TEXT_FAINT) : lv_color_hex(GUI_COLOR_TEXT_SOFT), 0);
        if (selected) lv_obj_scroll_to_view(chip, LV_ANIM_OFF);
    }
}

static void eq_band_options_open_cb(lv_event_t * e);

static void eq_band_chip_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int stop = (int) (intptr_t) lv_event_get_user_data(e);
    /* A second tap on the current band opens its options. */
    if (!eq_graph_visible && stop == current_eq_band) {
        eq_band_options_open_cb(e);
        return;
    }
    eq_show_stop(stop);
}

static void refresh_eq_band_widgets(void) {
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    char formatted[32];

    if (eq_band_enabled_switch) {
        if (band->enabled) lv_obj_add_state(eq_band_enabled_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(eq_band_enabled_switch, LV_STATE_CHECKED);
    }

    if (eq_type_dropdown) lv_dropdown_set_selected(eq_type_dropdown, (uint32_t) band->type);

    if (eq_freq_slider) lv_slider_set_value(eq_freq_slider, freq_to_slider(band->freq_hz), LV_ANIM_OFF);
    if (eq_gain_slider) lv_slider_set_value(eq_gain_slider, (int32_t) (band->gain_db * 10.0), LV_ANIM_OFF);
    if (eq_q_slider) lv_slider_set_value(eq_q_slider, (int32_t) (band->q * 10.0), LV_ANIM_OFF);

    if (eq_freq_value_label) lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), band->freq_hz);
    eq_format_gain(formatted, sizeof(formatted), band->gain_db);
    if (eq_gain_value_label) lv_label_set_text(eq_gain_value_label, formatted);
    eq_format_q(formatted, sizeof(formatted), band->q);
    if (eq_q_value_label) lv_label_set_text(eq_q_value_label, formatted);
    if (eq_band_number_label) {
        if (eq_graph_visible) lv_label_set_text(eq_band_number_label, TR("EQ curve"));
        else lv_label_set_text_fmt(eq_band_number_label, TR("Band %d / %d"), current_eq_band + 1, PEQ_NUM_BANDS);
    }
    eq_refresh_band_chips();
    if (eq_band_summary_label) {
        static const char * const types[] = { N_("Peaking"), N_("Low Shelf"), N_("High Shelf") };
        int t = (int) band->type;
        lv_label_set_text_fmt(eq_band_summary_label, "%s · %s", TR(types[(t >= 0 && t < 3) ? t : 0]),
                              band->enabled ? TR("On") : TR("Off"));
    }
}

/* Stops 0..PEQ_NUM_BANDS-1 are bands; PEQ_NUM_BANDS is the combined curve. */
static void eq_show_stop(int stop) {
    eq_graph_visible = (stop == PEQ_NUM_BANDS);
    if (!eq_graph_visible) current_eq_band = stop;
    lv_obj_t * band_widgets[] = {eq_band_options_row, eq_freq_card, eq_gain_card, eq_q_card};
    for (size_t i = 0; i < sizeof(band_widgets) / sizeof(band_widgets[0]); ++i) {
        if (!band_widgets[i]) continue;
        if (eq_graph_visible) lv_obj_add_flag(band_widgets[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(band_widgets[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (eq_graph_panel) {
        if (eq_graph_visible) lv_obj_clear_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_all_eq_widgets();
}

static void eq_band_step_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta = (int) (intptr_t) lv_event_get_user_data(e);
    int stop = (eq_graph_visible ? PEQ_NUM_BANDS : current_eq_band) + (delta < 0 ? -1 : 1);
    if (stop < 0) stop = PEQ_NUM_BANDS;
    if (stop > PEQ_NUM_BANDS) stop = 0;
    eq_show_stop(stop);
}

static void eq_band_options_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !eq_graph_visible && eq_band_options_screen) nav_push(eq_band_options_screen);
}

static void eq_type_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_type(current_eq_band, (peq_band_type_t) lv_dropdown_get_selected(lv_event_get_target(e)));
    peq_save();
}

static void eq_preamp_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    double db = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_preamp_db(db);
        lv_label_set_text_fmt(eq_preamp_value_label, TR("Pre-Amp: %+.2f dB"), db);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_band_enabled_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_enabled(current_eq_band, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void eq_freq_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double freq = slider_to_freq(lv_slider_get_value(lv_event_get_target(e)));

    /* Format string with label prefix consistently during active dragging
     * and when released. */
    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, freq, band->gain_db, band->q);
        lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), freq);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_gain_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double gain = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        char formatted[32];
        peq_set_band(current_eq_band, band->freq_hz, gain, band->q);
        eq_format_gain(formatted, sizeof(formatted), gain);
        lv_label_set_text(eq_gain_value_label, formatted);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_q_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double q = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        char formatted[32];
        peq_set_band(current_eq_band, band->freq_hz, band->gain_db, q);
        eq_format_q(formatted, sizeof(formatted), q);
        lv_label_set_text(eq_q_value_label, formatted);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

/* Restart into mellowtone-launcher's recovery menu: from
 * Developer Options, the power menu, and About when an update is waiting on
 * the card (the launcher installs it; the player never does). */
static gui_popup_t recovery_popup;
static lv_obj_t * recovery_popup_title;

static void hide_recovery_popup(void) {
    gui_popup_hide(&recovery_popup);
}

static void recovery_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_recovery_popup();
}

static void recovery_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_recovery_popup();
}

void gui_settings_restart_into_recovery_menu(void) {
    if (!launcher_request_menu()) {
        show_error_toast(TR("Could not set the recovery menu flag"));
        return;
    }
    idle_shutdown_reboot_now();
}

static void recovery_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_recovery_popup();
    gui_settings_restart_into_recovery_menu();
}

static void build_recovery_popup(void) {
    recovery_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &recovery_popup_title, NULL, TR("Restart"), accent_lv_color(),
        recovery_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), recovery_cancel_cb, NULL,
        recovery_popup_backdrop_cb, &recovery_popup.backdrop);
}

static void show_recovery_popup(void) {
    char name[128];
    if (launcher_update_on_card(name, sizeof(name)))
        lv_label_set_text_fmt(recovery_popup_title, TR("%s is on the card.\nRestart into the recovery menu to install it?"),
                              name);
    else
        lv_label_set_text(recovery_popup_title, TR("Restart into the recovery menu?"));
    gui_popup_show(&recovery_popup);
}

static void recovery_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_recovery_popup();
}

void gui_settings_show_launcher_notice(void) {
    const char * n = launcher_notice();
    if (!n) return;
    if (strcmp(n, "boot_loop") == 0)
        show_info_toast_for(TR("Mellowtone did not start 3 times: the previous version is running"), 8000);
    else if (strcmp(n, "rolled_back") == 0)
        show_info_toast_for(TR("Mellowtone crashed repeatedly: the previous version is running"), 8000);
    else if (strcmp(n, "factory") == 0)
        show_info_toast_for(TR("Mellowtone crashed repeatedly: the factory version is running"), 8000);
}

static lv_obj_t * adb_switch = NULL;

/* The Developer Options screen is built once and cached, so its ADB row would
 * otherwise keep showing whatever the state was at build time. Called when the
 * screen opens, and again by poll_usb_mode_switch() once the gadget settles so
 * a switch that failed does not leave the toggle promising ADB. */
void gui_settings_sync_adb_toggle(void) {
    if (!adb_switch) return;
    if (gui_network_adb_active()) lv_obj_add_state(adb_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(adb_switch, LV_STATE_CHECKED);
}

static void adb_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Turning ADB off falls back to charge only, the default: Storage and
     * USB DAC are chosen per connection. */
    bool want_adb = !gui_network_adb_active();
    if (!start_usb_mode_switch(want_adb ? USB_MODE_ADB : USB_MODE_CHARGE)) {
        /* Request dropped because a switch is already in flight. Put the
         * switch back rather than leaving it showing a state nothing is
         * working toward. */
        gui_settings_sync_adb_toggle();
        return;
    }
    /* Accepted: the switch already shows the requested state, and applying it
     * takes seconds. Leave it optimistically flipped; poll_usb_mode_switch()
     * confirms or rolls it back. */
}

static void dev_options_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_sync_adb_toggle();
    nav_push(dev_options_screen);
}

/* A firmware image or a player update on the card is installed by the
 * launcher's recovery menu. The About screen is built once, so its row is
 * shown or hidden each time About opens: a package copied while the player
 * runs (USB storage) is offered without a restart. */
static lv_obj_t * about_update_row;

static void refresh_about_update_row(void) {
    if (!about_update_row) return;
    char update_name[128];
    bool available = launcher_update_on_card(update_name, sizeof(update_name));
    if (!available) {
        lv_obj_add_flag(about_update_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    size_t len = strlen(update_name);
    bool firmware = len > 4 && strcasecmp(update_name + len - 4, ".upt") == 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(about_update_row); i++) {
        lv_obj_t * child = lv_obj_get_child(about_update_row, (int32_t) i);
        if (!lv_obj_check_type(child, &lv_label_class)) continue;
        lv_label_set_text(child, firmware ? TR("Firmware update available, restart to install")
                                          : TR("Player update available, restart to install"));
        break;
    }
    lv_obj_remove_flag(about_update_row, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * build_about_screen(void) {
    static pill_list_item_t items[3];
    lv_obj_t * version_row = NULL;
    int n = 0;
    items[n] = (pill_list_item_t){ TR("Mellowtone"), PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[n++].out_row = &version_row;
    items[n] = (pill_list_item_t){ TR("Player update available, restart to install"), PILL_ACCESSORY_CHEVRON, false,
                                   recovery_row_cb, NULL, NULL };
    items[n++].out_row = &about_update_row;
    items[n++] = (pill_list_item_t){ TR("Developer Options"), PILL_ACCESSORY_CHEVRON, false,
                                      dev_options_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("About"), generic_back_cb, items, n, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    lv_label_set_text(settings_add_summary(version_row), app_version_label());
    refresh_about_update_row();
    finalize_screen_navigation(scr);
    return scr;
}

static void db_logging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.db_logging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    db_log_set_enabled(current_settings.db_logging_enabled);
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
    if (BOARD.paths.launcher_debug_flag) {
        if (current_settings.db_logging_enabled) {
            if (mt_state_prepare_write(LAUNCHER_DEBUG_FLAG)) {
                int fd = open(LAUNCHER_DEBUG_FLAG, O_WRONLY | O_CREAT, 0644);
                if (fd >= 0) close(fd);
            }
        } else {
            unlink(LAUNCHER_DEBUG_FLAG);
            if (access(BOARD.paths.launcher_debug_flag, F_OK) == 0) {
                unlink(BOARD.paths.launcher_debug_flag);
                show_info_toast(TR("Bluetooth logging stops after a restart"));
            }
        }
    }
}

static void dev_bt_dac_all_codecs_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.dev_bt_dac_all_codecs = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    bt_control_set_dac_all_codecs(current_settings.dev_bt_dac_all_codecs);
}

static void dev_covers_during_playback_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.dev_covers_during_playback = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    gui_library_set_covers_during_playback(current_settings.dev_covers_during_playback);
}

static void dev_allow_suspend_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.dev_allow_suspend = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    power_suspend_set_allowed(current_settings.dev_allow_suspend);
}

static void plugin_developer_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    plugin_manager_developer_list_item_clicked((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * build_dev_options_screen(void) {
    static pill_list_item_t items[6 + PLUGIN_MAX_DEVELOPER_LIST_ITEMS];
    /* ADB lives here rather than on the USB Mode screen: it overrides
     * Storage/DAC while on and persists across a reboot, so it sits behind
     * Developer Options as the explicit opt-in that makes re-applying it on
     * startup reasonable. The USB Mode screen still dims the other modes
     * while it owns the port. */
    items[0] = (pill_list_item_t){ TR("ADB"), PILL_ACCESSORY_TOGGLE, gui_network_adb_active(),
                                    NULL, adb_switch_event_cb, NULL, &adb_switch };
    items[1] = (pill_list_item_t){ TR("Enable debug logging"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.db_logging_enabled ||
                                        (BOARD.paths.launcher_debug_flag && access(LAUNCHER_DEBUG_FLAG, F_OK) == 0),
                                    NULL, db_logging_switch_event_cb, NULL };
    int n = 2;
    /* Experimental: off by default, see settings.h. The DAC codec switch
     * applies the next time DAC mode starts (Bluetooth DAC = the receiver). */
    if (board_has(CAP_BT_RECEIVER))
        items[n++] = (pill_list_item_t){ TR("LDAC in DAC mode (Experimental)"), PILL_ACCESSORY_TOGGLE,
                                          current_settings.dev_bt_dac_all_codecs, NULL,
                                          dev_bt_dac_all_codecs_switch_event_cb, NULL };
    if (!BOARD.ui.no_cover_loading_option)
        items[n++] = (pill_list_item_t){ TR("Load covers during playback (Experimental)"), PILL_ACCESSORY_TOGGLE,
                                          current_settings.dev_covers_during_playback, NULL,
                                          dev_covers_during_playback_switch_event_cb, NULL };
    /* Stock-style standby on this board (BOARD_POWER_NO_SUSPEND), on by default. */
    if (BOARD.power.quirks & BOARD_POWER_NO_SUSPEND)
        items[n++] = (pill_list_item_t){ TR("Allow suspend"), PILL_ACCESSORY_TOGGLE,
                                          current_settings.dev_allow_suspend, NULL,
                                          dev_allow_suspend_switch_event_cb, NULL };
    if (BOARD.launcher)
        items[n++] = (pill_list_item_t){ TR("Restart into recovery menu"), PILL_ACCESSORY_CHEVRON, false,
                                          recovery_row_cb, NULL, NULL };
    /* Plugin rows ("developer" list id), e.g. the System status page. */
    n = append_plugin_list_rows(items, n, PLUGIN_MAX_DEVELOPER_LIST_ITEMS,
        plugin_manager_get_developer_list_item_count,
        plugin_manager_get_developer_list_item_label,
        plugin_manager_get_developer_list_item_options,
        plugin_developer_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Developer Options"), generic_back_cb, items, n, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- Accent Color ----
 * A live preview, "Match album art", a saturation/brightness square with a
 * hue bar, and the preset swatches. The square is the hue as a flat color
 * under a white-to-clear and a clear-to-black gradient, so changing hue only
 * recolors one object. Dragging previews locally; the accent is applied app
 * wide (styles, decoded play/drawer art, settings file) once, on release. */
#define ACCENT_CARD_PAD BOARD_SCALE_PX(16)
#define ACCENT_SV_HEIGHT BOARD_SCALE_PX(200)
#define ACCENT_HUE_TRACK_HEIGHT BOARD_SCALE_PX(36)
#define ACCENT_HUE_BAR_HEIGHT BOARD_SCALE_PX(20)
#define ACCENT_CURSOR_SIZE BOARD_SCALE_PX(28)
#define ACCENT_PRESET_SIZE BOARD_SCALE_PX(44)

static lv_obj_t * accent_preview_swatch;
static lv_obj_t * accent_hex_label;
static lv_obj_t * accent_source_label;
static lv_obj_t * accent_dynamic_switch;
static lv_obj_t * accent_picker_card;
static lv_obj_t * accent_sv_area;
static lv_obj_t * accent_sv_cursor;
static lv_obj_t * accent_hue_track;
static lv_obj_t * accent_hue_knob;
static lv_obj_t * accent_presets[ACCENT_PALETTE_COUNT];
static int32_t accent_sv_width;
static lv_grad_dsc_t accent_sat_grad;
static lv_grad_dsc_t accent_val_grad;
static lv_grad_dsc_t accent_hue_grad;
static lv_color_hsv_t accent_pick = { 207, 86, 95 };
static bool accent_committing;

static uint32_t accent_rgb_of(lv_color_t c) {
    return ((uint32_t) c.red << 16) | ((uint32_t) c.green << 8) | c.blue;
}

static void accent_show_color(uint32_t rgb, const char * source) {
    if (!accent_preview_swatch) return;
    char hex[16];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned int) (rgb & 0xFFFFFF));
    lv_obj_set_style_bg_color(accent_preview_swatch, lv_color_hex(rgb), 0);
    lv_label_set_text(accent_hex_label, hex);
    lv_label_set_text(accent_source_label, source);
}

static void accent_place_picker(void) {
    if (!accent_sv_area) return;
    lv_obj_set_style_bg_color(accent_sv_area, lv_color_hsv_to_rgb(accent_pick.h, 100, 100), 0);
    int32_t x = (int32_t) accent_pick.s * (accent_sv_width - 1) / 100;
    int32_t y = (int32_t) (100 - accent_pick.v) * (ACCENT_SV_HEIGHT - 1) / 100;
    lv_obj_set_pos(accent_sv_cursor, x - ACCENT_CURSOR_SIZE / 2, y - ACCENT_CURSOR_SIZE / 2);
    lv_obj_set_style_bg_color(accent_sv_cursor, lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v), 0);
    int32_t hx = (int32_t) accent_pick.h * (accent_sv_width - 1) / 359;
    lv_obj_set_pos(accent_hue_knob, hx - ACCENT_CURSOR_SIZE / 2, (ACCENT_HUE_TRACK_HEIGHT - ACCENT_CURSOR_SIZE) / 2);
    lv_obj_set_style_bg_color(accent_hue_knob, lv_color_hsv_to_rgb(accent_pick.h, 100, 100), 0);
}

/* Mirrors the live accent state into the screen. The picker follows the
 * saved color, except right after its own release, where re-deriving HSV
 * from the rounded RGB would nudge the cursor. */
static void accent_screen_sync(void) {
    if (!accent_preview_swatch) return;
    bool dynamic = current_settings.accent_dynamic;
    uint32_t cover = 0;
    bool has_cover = gui_theme_cover_accent(&cover);
    const char * source = !dynamic ? TR("Custom color")
                        : has_cover ? TR("From album art")
                        : TR("From album art (no cover, using custom)");
    accent_show_color(gui_theme_accent_rgb(), source);

    if (dynamic) lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(accent_dynamic_switch, LV_STATE_CHECKED);
    lv_obj_set_style_opa(accent_picker_card, dynamic ? LV_OPA_50 : LV_OPA_COVER, 0);

    for (int i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        bool selected = !dynamic && accent_palette[i] == current_settings.accent_color;
        lv_obj_set_style_border_width(accent_presets[i], selected ? BOARD_SCALE_PX(4) : 0, 0);
    }

    if (!accent_committing) {
        lv_color_t c = lv_color_hex(current_settings.accent_color);
        accent_pick = lv_color_rgb_to_hsv(c.red, c.green, c.blue);
        accent_place_picker();
    }
}

/* Called by gui_theme.c after every accent change, including a new cover
 * while "Match album art" is on. */
void gui_settings_accent_changed(void) {
    accent_screen_sync();
    if (eq_graph_series) lv_chart_set_series_color(eq_graph_chart, eq_graph_series, accent_lv_color());
    if (eq_graph_chart) lv_obj_set_style_line_color(eq_graph_chart, accent_lv_color(), LV_PART_ITEMS);
}

static void accent_commit_pick(void) {
    accent_committing = true;
    gui_theme_apply_accent(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)));
    accent_committing = false;
}

static int32_t accent_touch_offset(lv_obj_t * obj, bool vertical, int32_t span) {
    lv_indev_t * indev = lv_indev_active();
    if (!indev) return 0;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t v = vertical ? p.y - a.y1 : p.x - a.x1;
    if (v < 0) v = 0;
    if (v > span - 1) v = span - 1;
    return v;
}

static void accent_sv_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = accent_touch_offset(accent_sv_area, false, accent_sv_width);
        int32_t y = accent_touch_offset(accent_sv_area, true, ACCENT_SV_HEIGHT);
        accent_pick.s = (uint8_t) (x * 100 / (accent_sv_width - 1));
        accent_pick.v = (uint8_t) (100 - y * 100 / (ACCENT_SV_HEIGHT - 1));
        accent_place_picker();
        accent_show_color(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)),
                          TR("Custom color"));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        accent_commit_pick();
    }
}

static void accent_hue_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = accent_touch_offset(accent_hue_track, false, accent_sv_width);
        accent_pick.h = (uint16_t) (x * 359 / (accent_sv_width - 1));
        /* A gray pick has no visible hue; lift it so the drag shows color. */
        if (accent_pick.s < 20) accent_pick.s = 80;
        if (accent_pick.v < 20) accent_pick.v = 90;
        accent_place_picker();
        accent_show_color(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)),
                          TR("Custom color"));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        accent_commit_pick();
    }
}

static void accent_preset_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_theme_apply_accent((uint32_t) (uintptr_t) lv_event_get_user_data(e));
}

static void accent_dynamic_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_theme_set_accent_dynamic(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static lv_obj_t * accent_card(lv_obj_t * parent, int32_t width) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_all(card, ACCENT_CARD_PAD, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t * accent_text(lv_obj_t * parent, const char * text, gui_font_role_t role, bool muted) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, muted ? &style_theme_text_muted : &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    return label;
}

/* Plain layer or cursor: never takes touches, so they reach the area below. */
static lv_obj_t * accent_plain_obj(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* A drag surface: keeps the drag (no body scroll, no app swipe-back). */
static void accent_make_drag_surface(lv_obj_t * obj, lv_event_cb_t cb) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                       LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_ALL, NULL);
}

static lv_obj_t * accent_cursor(lv_obj_t * parent) {
    lv_obj_t * c = accent_plain_obj(parent);
    lv_obj_set_size(c, ACCENT_CURSOR_SIZE, ACCENT_CURSOR_SIZE);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_outline_color(c, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_outline_width(c, 1, 0);
    return c;
}

static lv_obj_t * build_accent_color_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Accent Color"), generic_back_cb, NULL, NULL);

    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t card_w = BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(20);
    accent_sv_width = card_w - 2 * ACCENT_CARD_PAD;

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    /* Preview: the accent as a large swatch with its hex value and source.
     * No sample switch here: a switch that cannot be turned off read as a
     * broken setting. */
    lv_obj_t * preview = accent_card(body, card_w);
    lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview, BOARD_SCALE_PX(16), 0);
    accent_preview_swatch = accent_plain_obj(preview);
    lv_obj_set_size(accent_preview_swatch, BOARD_SCALE_PX(76), BOARD_SCALE_PX(76));
    lv_obj_set_style_radius(accent_preview_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(accent_preview_swatch, LV_OPA_COVER, 0);
    lv_obj_t * info = accent_plain_obj(preview);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(6), 0);
    accent_hex_label = accent_text(info, "", GUI_FONT_ROLE_TITLE, false);
    accent_source_label = accent_text(info, "", GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(accent_source_label, lv_pct(100));
    lv_label_set_long_mode(accent_source_label, LV_LABEL_LONG_WRAP);

    /* Match album art */
    lv_obj_t * dyn = accent_card(body, card_w);
    lv_obj_set_flex_flow(dyn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dyn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dyn, BOARD_SCALE_PX(12), 0);
    lv_obj_t * dyn_text = accent_plain_obj(dyn);
    lv_obj_set_flex_grow(dyn_text, 1);
    lv_obj_set_height(dyn_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dyn_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dyn_text, BOARD_SCALE_PX(4), 0);
    accent_text(dyn_text, TR("Match album art"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * dyn_hint = accent_text(dyn_text, TR("Takes its color from the cover of the playing track"),
                                      GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(dyn_hint, lv_pct(100));
    lv_label_set_long_mode(dyn_hint, LV_LABEL_LONG_WRAP);
    accent_dynamic_switch = lv_switch_create(dyn);
    lv_obj_add_style(accent_dynamic_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(accent_dynamic_switch, accent_dynamic_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Custom color picker */
    accent_picker_card = accent_card(body, card_w);
    lv_obj_set_flex_flow(accent_picker_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(accent_picker_card, BOARD_SCALE_PX(14), 0);
    accent_text(accent_picker_card, TR("Custom color"), GUI_FONT_ROLE_ROW, false);

    accent_sv_area = lv_obj_create(accent_picker_card);
    lv_obj_remove_style_all(accent_sv_area);
    lv_obj_set_size(accent_sv_area, accent_sv_width, ACCENT_SV_HEIGHT);
    lv_obj_set_style_radius(accent_sv_area, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_bg_opa(accent_sv_area, LV_OPA_COVER, 0);
    accent_make_drag_surface(accent_sv_area, accent_sv_event_cb);
    static const lv_opa_t fade_out[2] = { LV_OPA_COVER, LV_OPA_TRANSP };
    static const lv_opa_t fade_in[2] = { LV_OPA_TRANSP, LV_OPA_COVER };
    lv_color_t whites[2] = { lv_color_white(), lv_color_white() };
    lv_color_t blacks[2] = { lv_color_black(), lv_color_black() };
    lv_grad_init_stops(&accent_sat_grad, whites, fade_out, NULL, 2);
    lv_grad_horizontal_init(&accent_sat_grad);
    lv_grad_init_stops(&accent_val_grad, blacks, fade_in, NULL, 2);
    lv_grad_vertical_init(&accent_val_grad);
    lv_grad_dsc_t * layers[2] = { &accent_sat_grad, &accent_val_grad };
    for (int i = 0; i < 2; i++) {
        lv_obj_t * layer = accent_plain_obj(accent_sv_area);
        lv_obj_set_size(layer, accent_sv_width, ACCENT_SV_HEIGHT);
        lv_obj_set_style_radius(layer, BOARD_SCALE_PX(12), 0);
        lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad(layer, layers[i], 0);
    }
    accent_sv_cursor = accent_cursor(accent_sv_area);

    accent_hue_track = lv_obj_create(accent_picker_card);
    lv_obj_remove_style_all(accent_hue_track);
    lv_obj_set_size(accent_hue_track, accent_sv_width, ACCENT_HUE_TRACK_HEIGHT);
    accent_make_drag_surface(accent_hue_track, accent_hue_event_cb);
    lv_color_t hues[7];
    for (int i = 0; i < 7; i++) hues[i] = lv_color_hsv_to_rgb((uint16_t) (i * 60 % 360), 100, 100);
    lv_grad_init_stops(&accent_hue_grad, hues, NULL, NULL, 7);
    lv_grad_horizontal_init(&accent_hue_grad);
    lv_obj_t * hue_bar = accent_plain_obj(accent_hue_track);
    lv_obj_set_size(hue_bar, accent_sv_width, ACCENT_HUE_BAR_HEIGHT);
    lv_obj_align(hue_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(hue_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hue_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(hue_bar, &accent_hue_grad, 0);
    accent_hue_knob = accent_cursor(accent_hue_track);

    lv_obj_remove_flag(accent_picker_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(accent_picker_card);

    /* Presets */
    lv_obj_t * presets = accent_card(body, card_w);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(presets, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(presets, BOARD_SCALE_PX(14), 0);
    lv_obj_t * presets_title = accent_text(presets, TR("Presets"), GUI_FONT_ROLE_ROW, false);
    lv_obj_set_width(presets_title, lv_pct(100));
    for (int i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_t * sw = lv_obj_create(presets);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, ACCENT_PRESET_SIZE, ACCENT_PRESET_SIZE);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(accent_palette[i]), 0);
        lv_obj_set_style_border_color(sw, lv_color_white(), 0);
        lv_obj_set_style_outline_color(sw, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(sw, LV_OPA_30, 0);
        lv_obj_set_style_outline_width(sw, 1, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(sw, BOARD_SCALE_PX(6));
        lv_obj_add_event_cb(sw, accent_preset_event_cb, LV_EVENT_CLICKED, (void *) (uintptr_t) accent_palette[i]);
        accent_presets[i] = sw;
    }

    accent_screen_sync();
    finalize_screen_navigation(scr);
    return scr;
}

static void accent_color_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(accent_color_screen);
}

static lv_obj_t * custom_font_list = NULL;
static lv_obj_t * custom_font_preview_latin = NULL;
static lv_obj_t * custom_font_preview_note = NULL;
static char discovered_custom_fonts[MAX_CUSTOM_FONTS_DISCOVERED][64];
static int discovered_custom_font_count = 0;

static void custom_font_option_cb(lv_event_t * e);
static void custom_font_apply_timer_cb(lv_timer_t * timer);

static void populate_custom_font_screen(void) {
    if (!custom_font_list) return;
    lv_obj_clean(custom_font_list);

    discovered_custom_font_count = fallback_font_discover_custom(discovered_custom_fonts, MAX_CUSTOM_FONTS_DISCOVERED);

    const char * active_name = fallback_font_get_custom_name();

    /* 1. Default (built-in Montserrat) option */
    bool default_selected = (strcmp(active_name, "Default") == 0 || !current_settings.custom_font[0]);
    add_pill_option_row(custom_font_list, TR("Default (Built-in)"),
                        default_selected, custom_font_option_cb, (void *) (intptr_t) -1);

    /* 2. Discovered fonts from <SD>/Fonts */
    for (int i = 0; i < discovered_custom_font_count; i++) {
        bool selected = (strcmp(active_name, discovered_custom_fonts[i]) == 0);
        add_pill_option_row(custom_font_list, discovered_custom_fonts[i],
                            selected, custom_font_option_cb, (void *) (intptr_t) i);
    }

    if (discovered_custom_font_count == 0) {
        lv_obj_t * empty_note = lv_label_create(custom_font_list);
        lv_label_set_text(empty_note, TR("No .ttf fonts found in /Fonts"));
        lv_obj_add_style(empty_note, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_style_pad_top(empty_note, 12, 0);
    }
}

static void custom_font_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int index = (int)(intptr_t)lv_obj_get_user_data(mask) - 2;
    lv_timer_delete(timer);

    if (index < -1 || index >= discovered_custom_font_count) {
        lv_obj_delete(mask);
        show_error_toast(TR("Font selection is no longer available"));
        return;
    }
    const char * target = index < 0 ? "Default" : discovered_custom_fonts[index];
    if (!fallback_font_apply_custom(target)) {
        lv_obj_delete(mask);
        show_error_toast(TR("Failed to load font. Check format & memory."));
        return;
    }

    /* The stable app_font_* addresses now contain the candidate descriptors.
     * Perform the same bounded, one-shot refresh as live Font Size while the
     * black input mask is still covering the display.  Custom Font also
     * changes the Latin face of app_font_lyrics (but not its independent
     * size), so its existing layout gets one explicit refresh here. */
    gui_navigation_invalidate_font_snapshots();
    snprintf(current_settings.custom_font, sizeof(current_settings.custom_font), "%s",
             index < 0 ? "" : target);
    settings_save(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Same full-display walk as font_size_apply_timer_cb() -- Custom Font
     * also rewrites app_font_* metrics in place, so unvisited Settings
     * submenus need their USER_3 label boxes recomputed too. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    compact_list_refresh_all();
    gui_lyrics_refresh_layout();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void custom_font_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    const char * target = (index < 0) ? "Default" : discovered_custom_fonts[index];

    /* Match Font Size's true no-op: do not copy/validate/rebuild the active
     * font, flash the display, save settings, or reset navigation. */
    const char * active = fallback_font_get_custom_name();
    if ((index < 0 && strcmp(active, "Default") == 0) ||
        (index >= 0 && strcmp(active, target) == 0)) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    /* Encode -1 (Default) as 1 and discovered indices as 2..N+1.  The
     * discovered-name table is static and the input-blocking mask prevents
     * it from being repopulated before the one-shot callback consumes it. */
    lv_obj_set_user_data(mask, (void *)(intptr_t)(index + 2));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);

    /* As with Font Size, let the regular refresh timer paint black before
     * any SD I/O, TTF validation or cache construction begins. */
    lv_timer_create(custom_font_apply_timer_cb, 35, mask);
}

static void custom_font_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_custom_font_screen();
    nav_push(custom_font_screen);
}

static lv_obj_t * build_custom_font_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Font"), generic_back_cb, NULL, NULL);

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(8);
    lv_obj_set_size(body, lv_pct(100), BOARD_SCREEN_HEIGHT - top - BOARD_SCALE_PX(24));
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(8), 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * preview_card = lv_obj_create(body);
    lv_obj_set_size(preview_card, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(preview_card, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_row(preview_card, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(preview_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(preview_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(preview_card, 0, 0);
    lv_obj_set_style_radius(preview_card, 10, 0);
    lv_obj_remove_flag(preview_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * preview_title = lv_label_create(preview_card);
    lv_label_set_text(preview_title, TR("Preview"));
    lv_obj_add_style(preview_title, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(preview_title, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(preview_title, lv_pct(100));
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_WRAP);

    custom_font_preview_latin = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_latin, TR("The quick brown fox jumps 123"));
    lv_obj_add_style(custom_font_preview_latin, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(custom_font_preview_latin, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_width(custom_font_preview_latin, lv_pct(100));
    lv_label_set_long_mode(custom_font_preview_latin, LV_LABEL_LONG_WRAP);

    /* Custom fonts intentionally replace only the Latin face.  Rendering a
     * sample spanning every file-backed fallback here made this screen take
     * seconds to enter and leave on slow flash, while not previewing anything
     * the selected custom font can change. */
    custom_font_preview_note = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_note, TR("Custom fonts affect Latin text only."));
    lv_obj_add_style(custom_font_preview_note, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(custom_font_preview_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(custom_font_preview_note, lv_pct(100));
    lv_label_set_long_mode(custom_font_preview_note, LV_LABEL_LONG_WRAP);

    lv_obj_t * hint = lv_label_create(preview_card);
    lv_label_set_text(hint, TR("Place .ttf fonts in SD /Fonts folder."));
    lv_obj_add_style(hint, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* Scrollable font list */
    custom_font_list = lv_obj_create(body);
    lv_obj_set_size(custom_font_list, lv_pct(100), 0);
    lv_obj_set_flex_grow(custom_font_list, 1);
    lv_obj_set_scrollbar_mode(custom_font_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_opa(custom_font_list, 0, 0);
    lv_obj_set_style_border_width(custom_font_list, 0, 0);
    lv_obj_set_style_pad_all(custom_font_list, 0, 0);
    lv_obj_set_flex_flow(custom_font_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(custom_font_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(custom_font_list, 6, 0);

    finalize_screen_navigation(scr);
    return scr;
}

/* "45s" below 1 minute, "1m"/"2m" on an exact minute, "1m 30s" otherwise --
 * matches the 30s-10min range (SCREEN_TIMEOUT_MIN/MAX_SECONDS) without ever
 * needing more than whole minutes+seconds. */
static void format_screen_timeout(char * buf, size_t buf_size, int seconds) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%ds", seconds);
        return;
    }
    int minutes = seconds / 60;
    int remainder = seconds % 60;
    if (remainder == 0) snprintf(buf, buf_size, "%dm", minutes);
    else snprintf(buf, buf_size, "%dm %ds", minutes, remainder);
}

/* Index of the closest shared timeout preset. */
static int screen_timeout_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT, seconds);
}

static void screen_timeout_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_timeout_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.screen_timeout_enabled) {
        lv_obj_remove_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as the EQ sliders
 * (eq_preamp_slider_event_cb etc.) -- writing settings.c's file on every
 * drag tick would be needless disk I/O for a value that only matters once
 * the user lets go. The slider itself moves over step indices (0..
 * SCREEN_TIMEOUT_STEP_COUNT-1), mapped onto the shared preset table. */
static void screen_timeout_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_TIMEOUT_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_timeout_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_timeout_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_timeout_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Timeout"), generic_back_cb, NULL, NULL);

    /* Enable row configured with flex layout (SPACE_BETWEEN and flex-grow)
     * so the text label wraps cleanly across font sizes without overlapping
     * the switch. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Turn off screen automatically"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_timeout_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_timeout_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_timeout_enabled) lv_obj_add_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_timeout_switch, screen_timeout_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Rounded slider card with vertical clearance below the track for the
     * knob diameter and centered value label. */
    screen_timeout_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_TIMEOUT_STEP_COUNT - 1,
        screen_timeout_seconds_to_step_index(current_settings.screen_timeout_seconds),
        screen_timeout_slider_event_cb, &screen_timeout_slider, &screen_timeout_value_label);
    if (!current_settings.screen_timeout_enabled) lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_timeout_seconds);
    lv_label_set_text(screen_timeout_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void screen_timeout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_timeout_screen);
}

static int screen_dim_delay_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT, seconds);
}

static void screen_dimming_ui_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_dimming_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (!current_settings.screen_dimming_enabled) {
        backlight_set_dimmed(false);
    }
    settings_save(&current_settings);

    if (current_settings.screen_dimming_enabled) {
        lv_obj_remove_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_dim_delay_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_DIM_DELAY_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_dim_delay_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static void screen_dimming_screen_loaded_cb(lv_event_t * e) {
    int effective_max_index = SCREEN_DIM_DELAY_STEP_COUNT - 1;
    if (current_settings.screen_timeout_enabled) {
        effective_max_index = -1;
        for (int i = SCREEN_DIM_DELAY_STEP_COUNT - 1; i >= 0; i--) {
            if (SCREEN_DIM_DELAY_STEPS[i] < current_settings.screen_timeout_seconds) {
                effective_max_index = i;
                break;
            }
        }
        if (effective_max_index == -1) {
            effective_max_index = 0;
        }
    }

    lv_slider_set_range(screen_dimming_slider, 0, effective_max_index);

    int current_index = screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds);
    if (current_index > effective_max_index) {
        current_settings.screen_dim_delay_seconds = SCREEN_DIM_DELAY_STEPS[effective_max_index];
        lv_slider_set_value(screen_dimming_slider, effective_max_index, LV_ANIM_OFF);
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), current_settings.screen_dim_delay_seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_dimming_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, screen_dimming_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Dimming"), generic_back_cb, NULL, NULL);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Dim screen before timeout"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_dimming_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_dimming_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_dimming_enabled) lv_obj_add_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_dimming_switch, screen_dimming_ui_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    screen_dimming_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_DIM_DELAY_STEP_COUNT - 1,
        screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds),
        screen_dim_delay_slider_event_cb, &screen_dimming_slider, &screen_dimming_value_label);
    if (!current_settings.screen_dimming_enabled) lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_dim_delay_seconds);
    lv_label_set_text(screen_dimming_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void screen_dimming_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_dimming_screen);
}

static void startup_volume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_volume_fixed_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.startup_volume_fixed_enabled) {
        lv_obj_remove_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as
 * screen_timeout_slider_event_cb -- this only sets what the NEXT launch
 * starts at, not the current session's live volume, so there's no reason
 * to call audio_set_volume() here at all. */
static void startup_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.startup_volume_fixed_percent = (int) percent;
        lv_label_set_text_fmt(startup_volume_value_label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* Mirrors build_screen_timeout_screen()'s own layout (toggle row + a
 * dark card holding a live-readout slider, shown/hidden with the toggle) --
 * see that function's comments for the reasoning behind the specific
 * measurements reused here. */
static lv_obj_t * build_startup_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Startup Volume"), generic_back_cb, NULL, NULL);

    /* Flex row with SPACE_BETWEEN layout to prevent text/switch overlap. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Launch at a fixed volume"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    startup_volume_switch = lv_switch_create(enable_row);
    lv_obj_add_style(startup_volume_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.startup_volume_fixed_enabled) lv_obj_add_state(startup_volume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_volume_switch, startup_volume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    startup_volume_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, 100, current_settings.startup_volume_fixed_percent,
        startup_volume_slider_event_cb, &startup_volume_slider, &startup_volume_value_label);
    if (!current_settings.startup_volume_fixed_enabled) lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(startup_volume_value_label, "%d%%", current_settings.startup_volume_fixed_percent);

    finalize_screen_navigation(scr);
    return scr;
}

static void startup_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(startup_volume_screen);
}

void gui_settings_open_eq(void) {
    if (!eq_screen) return;
    nav_push(eq_screen);
}

void gui_settings_open_sleep_timer(void) {
    if (!sleep_timer_screen) return;
    gui_settings_sync_sleep_timer_toggle();
    nav_push(sleep_timer_screen);
}

void gui_settings_open_playback(void) {
    if (!music_playback_screen) return;
    nav_push(music_playback_screen);
}

/* Index into SLEEP_TIMER_STEPS closest to `minutes' -- same reasoning as
 * screen_timeout_seconds_to_step_index() above. */
static int sleep_timer_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT, minutes);
}

/* Extracted from build_sleep_timer_screen() so it can be shared with its
 * own format_duration(), needed now that SLEEP_TIMER_STEPS reaches 180
 * (settings.c) and a bare "%d min" would read as "180 min" instead of the
 * much more readable "3 hr". */
static void format_sleep_timer_duration(char * buf, size_t buf_size, int minutes) {
    int hours = minutes / 60;
    int remainder = minutes % 60;
    if (hours == 0) snprintf(buf, buf_size, TR("%d min"), remainder);
    else if (remainder == 0) snprintf(buf, buf_size, TR("%d hr"), hours);
    else snprintf(buf, buf_size, TR("%d hr %d min"), hours, remainder);
}

/* Arms or disarms the sleep timer directly, synchronizing state with the
 * quick drawer sleep timer toggle. */
static void sleep_timer_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    quick_drawer_sleep_timer_set_active(active);
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* This only sets the duration the quick-drawer sleep icon/enable toggle
 * above arms next time (or restarts the CURRENT countdown at, if already
 * armed -- same real-time-restart behavior ExtendedSleepTimer.lua's own
 * duration slider has, matching what a user dragging this while counting
 * down would expect: the countdown they're looking at should reflect the
 * duration they just picked, not the one that was armed before). */
static void sleep_timer_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = SLEEP_TIMER_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.sleep_timer_minutes = minutes;
        char duration_buf[32];
        format_sleep_timer_duration(duration_buf, sizeof(duration_buf), minutes);
        lv_label_set_text(sleep_timer_value_label, duration_buf);
        if (quick_drawer_sleep_timer_is_active()) quick_drawer_sleep_timer_set_active(true);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* "Show Time Remaining" button -- same info ExtendedSleepTimer.lua's own
 * "Show Time Remaining" settings row surfaces via a toast, in the same
 * H:MM:SS (or M:SS under an hour) shape as that plugin's format_remaining().
 * Hidden whenever the timer is disarmed (sleep_timer_switch_event_cb /
 * gui_settings_sync_sleep_timer_toggle), so this is only ever reachable
 * while armed -- no "timer is off" fallback message needed here. */
static void sleep_timer_show_remaining_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int total_seconds = quick_drawer_sleep_timer_remaining_seconds();
    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int secs = total_seconds % 60;
    char buf[48];
    if (hours > 0) snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d:%02d"), hours, minutes, secs);
    else snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d"), minutes, secs);
    show_info_toast(buf);
}

/* Mirrors build_screen_timeout_screen()'s layout (enable row + slider
 * card), plus one more row: a "Show Time Remaining" button neither that
 * screen nor this one needed before ExtendedSleepTimer.lua's own version
 * of this same feature demonstrated why one's genuinely useful here (the
 * status bar has nowhere to show a running countdown, unlike the quick
 * drawer's own quick_drawer_sleep_label). */
static lv_obj_t * build_sleep_timer_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Sleep Timer"), generic_back_cb, NULL, NULL);

    /* Same flex-row/SPACE_BETWEEN/flex_grow-label shape as build_screen_
     * timeout_screen()'s own enable_row -- see its comment for the real-
     * device overlap bug this avoids at bigger system text sizes. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Enable Sleep Timer"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    sleep_timer_switch = lv_switch_create(enable_row);
    lv_obj_add_style(sleep_timer_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (quick_drawer_sleep_timer_is_active()) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sleep_timer_switch, sleep_timer_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sleep_timer_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SLEEP_TIMER_STEP_COUNT - 1,
        sleep_timer_minutes_to_step_index(current_settings.sleep_timer_minutes),
        sleep_timer_slider_event_cb, &sleep_timer_slider, &sleep_timer_value_label);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
    char duration_buf[32];
    format_sleep_timer_duration(duration_buf, sizeof(duration_buf), current_settings.sleep_timer_minutes);
    lv_label_set_text(sleep_timer_value_label, duration_buf);

    sleep_timer_remaining_btn = lv_obj_create(scr);
    lv_obj_set_size(sleep_timer_remaining_btn, lv_pct(90), BOARD_SCALE_PX(70));
    lv_obj_align_to(sleep_timer_remaining_btn, sleep_timer_slider_card, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(sleep_timer_remaining_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(sleep_timer_remaining_btn, 0, 0);
    lv_obj_set_style_radius(sleep_timer_remaining_btn, 10, 0);
    lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_timer_remaining_btn, sleep_timer_show_remaining_cb, LV_EVENT_CLICKED, NULL);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * remaining_label = lv_label_create(sleep_timer_remaining_btn);
    lv_label_set_text(remaining_label, TR("Show Time Remaining"));
    lv_obj_add_style(remaining_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remaining_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(remaining_label);

    finalize_screen_navigation(scr);
    return scr;
}

/* All IDLE_SHUTDOWN_STEPS values are whole minutes, so unlike
 * format_screen_timeout() this never needs a seconds remainder -- "10m" /
 * "1h" / "2h", not "1h 30m", since 90 isn't one of the steps. */
static void format_idle_shutdown(char * buf, size_t buf_size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, buf_size, "%dm", minutes);
        return;
    }
    snprintf(buf, buf_size, "%dh", minutes / 60);
}

static int idle_shutdown_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT, minutes);
}

static void idle_shutdown_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.idle_shutdown_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.idle_shutdown_enabled) {
        lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Mutually-exclusive idle-action choice (Power Off vs Suspend to RAM) --
 * replaces the old single "Suspend instead of power off" switch, which
 * modeled this same 2-way choice as a toggle acting on an implicit
 * opposite (plain poweroff being "off"). Same accent-border-highlight
 * pill-row selection language as populate_usb_mode_screen()'s own Storage/
 * USB DAC rows (add_pill_row_base()), but inline on this same screen
 * rather than a separate list screen, since it stays right above the
 * slider it's a sibling setting of. */
static void idle_action_choice_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool suspend = (bool) (intptr_t) lv_event_get_user_data(e);
    current_settings.idle_suspend_enabled = suspend;
    settings_save(&current_settings);
    lv_obj_set_style_border_width(idle_action_poweroff_row, suspend ? 0 : 3, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, suspend ? 3 : 0, 0);
}

static void idle_shutdown_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = IDLE_SHUTDOWN_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.idle_shutdown_minutes = minutes;
        char buf[32];
        format_idle_shutdown(buf, sizeof(buf), minutes);
        lv_label_set_text(idle_shutdown_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_idle_shutdown_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Idle Shutdown"), generic_back_cb, NULL, NULL);

    /* Uses content-based sizing so wrapped label text does not overlap. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Automatically go idle"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    idle_shutdown_switch = lv_switch_create(enable_row);
    lv_obj_add_style(idle_shutdown_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.idle_shutdown_enabled) lv_obj_add_state(idle_shutdown_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(idle_shutdown_switch, idle_shutdown_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Mutually-exclusive idle-action choice (Power Off or Suspend to RAM).
     * Shown/hidden together with the slider card in
     * idle_shutdown_switch_event_cb(). */
    /* Section height and the two pill rows' Y offsets below were all
     * hardcoded (292 / 34 / 34+124+10) at some medium-tier row height --
     * add_pill_row_base()'s own row height already scales with
     * GUI_FONT_ROLE_BODY's line height, but these hand-rolled offsets
     * didn't, so at the largest ("BlindMF") font tier the two rows grew
     * taller than the fixed gaps between them and overflowed the fixed
     * section height into the slider card below (GitHub issue #91).
     * Derived from the same font metrics add_pill_row_base() itself uses,
     * so this stays correct at every tier without needing another manual
     * retune. */
    int32_t idle_action_explain_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    int32_t idle_action_row_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) + 32;
    if (idle_action_row_h < GUI_SETTINGS_ROW_HEIGHT) idle_action_row_h = GUI_SETTINGS_ROW_HEIGHT;
    int32_t idle_action_row1_y = idle_action_explain_h + 14;
    int32_t idle_action_row_gap = 10;
    int32_t idle_action_row2_y = idle_action_row1_y + idle_action_row_h + idle_action_row_gap;
    int32_t idle_action_section_h = idle_action_row2_y + idle_action_row_h;

    /* 100% width accommodates display-width pill rows without clipping.
     * pad_top and pad_bottom are zeroed so the child rows and label fit within
     * the section height (now font-tier-derived above) without vertical overflow. */
    idle_action_section = lv_obj_create(scr);
    lv_obj_set_size(idle_action_section, lv_pct(100), idle_action_section_h);
    /* Positioned relative to enable_row's bottom edge so wrapped label text
     * does not cause overlapping. */
    lv_obj_align_to(idle_action_section, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_top(idle_action_section, 0, 0);
    lv_obj_set_style_pad_bottom(idle_action_section, 0, 0);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_action_explain_label = lv_label_create(idle_action_section);
    lv_label_set_text(idle_action_explain_label, TR("Choose what happens when idle:"));
    lv_obj_add_style(idle_action_explain_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_action_explain_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_explain_label, LV_ALIGN_TOP_LEFT, 0, 0);

    idle_action_poweroff_row = add_pill_row_base(idle_action_section, TR("Power Off"));
    lv_obj_align(idle_action_poweroff_row, LV_ALIGN_TOP_MID, 0, idle_action_row1_y);
    lv_obj_add_style(idle_action_poweroff_row, gui_theme_accent_outline_style(), 0);
    lv_obj_add_style(idle_action_poweroff_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(idle_action_poweroff_row, current_settings.idle_suspend_enabled ? 0 : 3, 0);
    lv_obj_add_flag(idle_action_poweroff_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_poweroff_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) false);

    idle_action_suspend_row = add_pill_row_base(idle_action_section, TR("Suspend to RAM"));
    lv_obj_align(idle_action_suspend_row, LV_ALIGN_TOP_MID, 0, idle_action_row2_y);
    lv_obj_add_style(idle_action_suspend_row, gui_theme_accent_outline_style(), 0);
    lv_obj_add_style(idle_action_suspend_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, current_settings.idle_suspend_enabled ? 3 : 0, 0);
    lv_obj_add_flag(idle_action_suspend_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_suspend_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) true);
    if (BOARD.power.quirks & BOARD_POWER_NO_SUSPEND) {
        /* No suspend-to-RAM on this board: idle always powers off (gui.c). */
        lv_obj_add_flag(idle_action_suspend_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(idle_action_poweroff_row, 3, 0);
        lv_obj_remove_flag(idle_action_poweroff_row, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(idle_action_explain_label, TR("When idle:"));
    }

    /* Slider card positioned below idle_action_section. Sized at 200px height
     * to accommodate the explanatory caption above the slider. */
    idle_shutdown_slider_card = build_setting_slider_card(scr, idle_action_section, BOARD_SCALE_PX(200), BOARD_SCALE_PX(48),
        0, IDLE_SHUTDOWN_STEP_COUNT - 1,
        idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes),
        idle_shutdown_slider_event_cb, &idle_shutdown_slider, &idle_shutdown_value_label);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_shutdown_slider_caption = lv_label_create(idle_shutdown_slider_card);
    lv_label_set_text(idle_shutdown_slider_caption, TR("Idle timeout:"));
    lv_obj_add_style(idle_shutdown_slider_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_shutdown_slider_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_shutdown_slider_caption, LV_ALIGN_TOP_LEFT, 24, 14);
    char initial_buf[32];
    format_idle_shutdown(initial_buf, sizeof(initial_buf), current_settings.idle_shutdown_minutes);
    lv_label_set_text(idle_shutdown_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void idle_shutdown_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(idle_shutdown_screen);
}

/* ---- Time Zone picker ----------------------------------------------------
 * Region -> City selection. TIMEZONE_TABLE (timezone_data.h) is sorted
 * by UTC offset then city name. Region names are the top-level IANA region
 * components present in TIMEZONE_TABLE. */
static const char * const TIMEZONE_REGIONS[] = {
    N_("Africa"), N_("America"), N_("Antarctica"), N_("Arctic"), N_("Asia"), N_("Atlantic"), N_("Australia"),
    N_("Europe"), N_("Indian"), N_("Pacific")
};
#define TIMEZONE_REGION_COUNT (sizeof(TIMEZONE_REGIONS) / sizeof(TIMEZONE_REGIONS[0]))

static lv_obj_t * timezone_region_screen;
static lv_obj_t * timezone_city_screen;
/* Maps a City-screen row index (as passed to timezone_city_row_click_cb)
 * back to its real TIMEZONE_TABLE index -- the City screen only ever shows
 * one region's subset, so row index != table index. */
static int timezone_city_indices[TIMEZONE_TABLE_COUNT];
static int timezone_city_count;

static void clock_timezone_indicator_update(void) {
    if (!clock_timezone_value_label) return;
    const char * zone = current_settings.timezone[0]
        ? current_settings.timezone : TR("Not selected (UTC)");
    for (int i = 0; current_settings.timezone[0] && i < TIMEZONE_TABLE_COUNT; i++) {
        if (strcmp(current_settings.timezone, TIMEZONE_TABLE[i].iana_id) == 0) {
            zone = TIMEZONE_TABLE[i].display_name;
            break;
        }
    }
    lv_label_set_text(clock_timezone_value_label, zone);
}

static void timezone_city_row_click_cb(int row_index) {
    if (row_index < 0 || row_index >= timezone_city_count) return;
    const timezone_entry_t * entry = &TIMEZONE_TABLE[timezone_city_indices[row_index]];
    if (!timezone_apply(entry->iana_id)) {
        show_error_toast(TR("Failed to apply time zone"));
        return;
    }
    snprintf(current_settings.timezone, sizeof(current_settings.timezone), "%s", entry->iana_id);
    settings_save(&current_settings);
    refresh_clock_label(); /* topbar clock reflects the new zone immediately, not just on the next periodic refresh */
    clock_timezone_indicator_update();
    /* Skip the intermediate Region entry before starting navigation. Two
     * immediate nav_pop() calls race their slide transitions: the first
     * can finish later and restore Region over the intended Clock screen. */
    int depth = gui_navigation_get_depth();
    if (depth >= 3) nav_remove_stack_slot(depth - 2);
    nav_pop(); /* City -> Clock -- picking a leaf city completes the choice */
}

/* Rebuilt (delete + rebuild, same idiom as gui_library_get_all_songs_screen() after a library
 * rescan -- see poll_library_rescan()) every time a region is opened,
 * filtered down to just that region's entries, rather than built once --
 * needed both because the region changes on every open and so the
 * "(current)" marker always reflects current_settings.timezone as of the
 * most recent selection. Cheap even at TIMEZONE_TABLE_COUNT entries since
 * build_compact_list_screen is virtualized (screen_builders.h) -- only a
 * small pool of row widgets actually gets created, not one per entry.
 * `labels` is this function's own long-lived backing array for the
 * "(current)"-suffixed copy build_compact_list_screen's own doc comment
 * requires (label pointers must outlive the screen, not just this call) --
 * previous generation's entries are freed up to prev_count (the region
 * filter means the count varies per open, unlike a fixed-size table) right
 * before this rebuild, once the previous screen holding those pointers has
 * already been deleted by the caller. */
static void build_timezone_city_screen_items(compact_list_item_t * items, const char * region) {
    static char * labels[TIMEZONE_TABLE_COUNT];
    static int prev_count = 0;
    for (int i = 0; i < prev_count; i++) {
        free(labels[i]);
        labels[i] = NULL;
    }

    size_t region_len = strlen(region);
    timezone_city_count = 0;
    for (int i = 0; i < TIMEZONE_TABLE_COUNT; i++) {
        const char * id = TIMEZONE_TABLE[i].iana_id;
        if (strncmp(id, region, region_len) != 0 || id[region_len] != '/') continue;

        int row = timezone_city_count;
        bool is_current = strcmp(current_settings.timezone, id) == 0;
        if (is_current) {
            size_t len = strlen(TIMEZONE_TABLE[i].display_name) + strlen(TR("%s (current)")) + 1;
            labels[row] = malloc(len);
            snprintf(labels[row], len, TR("%s (current)"), TIMEZONE_TABLE[i].display_name);
        } else {
            labels[row] = strdup(TIMEZONE_TABLE[i].display_name);
        }
        timezone_city_indices[row] = i;
        items[row] = (compact_list_item_t){ labels[row] };
        timezone_city_count++;
    }
    prev_count = timezone_city_count;
}

static lv_obj_t * build_timezone_city_screen(const char * region) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) TIMEZONE_TABLE_COUNT);
    build_timezone_city_screen_items(items, region);
    lv_obj_t * scr = build_compact_list_screen(region, generic_back_cb, items, timezone_city_count, timezone_city_row_click_cb, NULL, NULL, NULL, LIST_ROW_WIDTH, false, lv_color_black());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static void open_timezone_city_screen(const char * region) {
    if (timezone_city_screen) lv_obj_delete(timezone_city_screen);
    timezone_city_screen = build_timezone_city_screen(region);
    nav_push(timezone_city_screen);
}

static void timezone_region_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    open_timezone_city_screen(TIMEZONE_REGIONS[index]); /* English key, matched against TIMEZONE_TABLE */
}

/* Small (10 rows) and fixed -- built once at startup like every other
 * screen, unlike the per-region City screen above. */
static lv_obj_t * build_timezone_region_screen(void) {
    static pill_list_item_t items[TIMEZONE_REGION_COUNT];
    for (size_t i = 0; i < TIMEZONE_REGION_COUNT; i++) {
        items[i] = (pill_list_item_t){ TR(TIMEZONE_REGIONS[i]), PILL_ACCESSORY_CHEVRON, false, timezone_region_row_cb, NULL,
                                        (void *) (intptr_t) i };
    }
    lv_obj_t * scr = build_pill_list_screen(TR("Time Zone"), generic_back_cb, items, (int) TIMEZONE_REGION_COUNT, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_setup_open_timezone(void) {
    nav_push(timezone_region_screen);
}

static void timezone_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(timezone_region_screen);
}

/* Defined later, alongside its confirmation popup (build_factory_reset_popup(),
 * right after build_eq_reset_popup() -- same hand-built top-layer overlay
 * shape). */
static void factory_reset_btn_cb(lv_event_t * e);

/* ---- Settings category sub-screens -- grouped into category screens
 * (Display, Power & Sleep, Device & Storage, Date & Time, About). ---- */

/* ---- Music Settings sub-screens -- grouped into category screens
 * (Playback, Audio, Controls & Interface, Timers, Library, Plugins). ---- */


/* Shared click handler for every plugin-registered Playback list row below
 * -- same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_playback_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_playback_list_item_clicked(index);
}

static void plugin_music_audio_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_audio_list_item_clicked(index);
}

static void plugin_music_controls_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_controls_list_item_clicked(index);
}

static void music_controls_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!music_controls_screen) music_controls_screen = build_music_controls_screen();
    if (music_controls_screen) nav_push(music_controls_screen);
}

static void plugin_music_library_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_library_list_item_clicked(index);
}

/* Filter plugin rows into the new settings groups while preserving the
 * original plugin index used by each click handler. Unknown groups stay in
 * the direct list so older/future plugins remain reachable. */
static int append_grouped_plugin_rows(pill_list_item_t * items, int count, int max_items,
                                      const char * list_id, const char * group,
                                      plugin_list_item_count_cb_t get_count_fn,
                                      plugin_list_item_label_cb_t get_label_fn,
                                      plugin_list_item_options_cb_t get_options_fn,
                                      lv_event_cb_t click_cb) {
    int plugin_count = get_count_fn();
    int added = 0;
    for (int i = 0; i < plugin_count && added < max_items; ++i) {
        const char * row_group = plugin_manager_get_list_item_group(list_id, i);
        bool include = group ? (row_group && strcmp(row_group, group) == 0)
                             : (!row_group || ((strcmp(list_id, "music_audio") == 0) &&
                                                strcmp(row_group, "effects") != 0 &&
                                                strcmp(row_group, "profiles") != 0) ||
                               ((strcmp(list_id, "display") == 0) &&
                                strcmp(row_group, "appearance") != 0 &&
                                strcmp(row_group, "player_layout") != 0));
        if (!include) continue;
        pill_list_item_t item = { get_label_fn(i), PILL_ACCESSORY_CHEVRON, false,
                                  click_cb, NULL, (void *) (intptr_t) i };
        const char * text_size = NULL;
        get_options_fn(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count + added++] = item;
    }
    return count + added;
}

static int count_plugin_rows_in_group(const char * list_id, int plugin_count, const char * group) {
    int count = 0;
    for (int i = 0; i < plugin_count; ++i) {
        const char * candidate = plugin_manager_get_list_item_group(list_id, i);
        if (candidate && strcmp(candidate, group) == 0) ++count;
    }
    return count;
}

static void music_effects_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sound_effects_screen) {
        static pill_list_item_t rows[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
        int count = append_grouped_plugin_rows(rows, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
            "music_audio", "effects", plugin_manager_get_music_audio_list_item_count,
            plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
            plugin_music_audio_list_item_click_cb);
        settings_sound_effects_screen = build_pill_list_screen(TR("Sound Effects"), generic_back_cb, rows,
            count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
        finalize_screen_navigation(settings_sound_effects_screen);
    }
    if (settings_sound_effects_screen) nav_push(settings_sound_effects_screen);
}

static void music_profiles_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_eq_profiles_menu_screen) {
        static pill_list_item_t rows[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
        int count = append_grouped_plugin_rows(rows, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
            "music_audio", "profiles", plugin_manager_get_music_audio_list_item_count,
            plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
            plugin_music_audio_list_item_click_cb);
        settings_eq_profiles_menu_screen = build_pill_list_screen(TR("Download Profiles"), generic_back_cb,
            rows, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
        finalize_screen_navigation(settings_eq_profiles_menu_screen);
    }
    if (settings_eq_profiles_menu_screen) nav_push(settings_eq_profiles_menu_screen);
}

static lv_obj_t * build_music_playback_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Resume Last Track"), PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Crossfade"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    /* Directly below Crossfade: the two are coupled (crossfade needs an
     * armed next track, which only gapless provides), so seeing them
     * together is what makes that relationship legible when one flips the
     * other -- see gui_player_set_gapless_enabled(). */
    items[2] = (pill_list_item_t){ TR("Gapless"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.gapless_enabled, NULL, gapless_switch_event_cb, NULL,
                                    &settings_gapless_toggle_img };
    items[3] = (pill_list_item_t){ TR("Buttons & Remote"), PILL_ACCESSORY_CHEVRON,
                                    false, music_controls_menu_cb, NULL, NULL };

    int count = 4;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_PLAYBACK_LIST_ITEMS,
                                    plugin_manager_get_playback_list_item_count,
                                    plugin_manager_get_playback_list_item_label,
                                    plugin_manager_get_playback_list_item_options,
                                    plugin_playback_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Playback & Controls"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_audio_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    lv_obj_t * eq_row = NULL;
    items[0] = (pill_list_item_t){ TR("Equalizer"), PILL_ACCESSORY_CHEVRON, false, eq_screen_btn_event_cb, NULL, NULL };
    items[0].out_row = &eq_row;
    int count = append_grouped_plugin_rows(items, 1, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
                                       "music_audio", NULL,
                                       plugin_manager_get_music_audio_list_item_count,
                                       plugin_manager_get_music_audio_list_item_label,
                                       plugin_manager_get_music_audio_list_item_options,
                                       plugin_music_audio_list_item_click_cb);
    if (board_has(CAP_GAIN_MODES))
        items[count++] = (pill_list_item_t){ TR("Gain"), PILL_ACCESSORY_CHEVRON, false, gain_settings_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Startup Volume"), PILL_ACCESSORY_CHEVRON, false, startup_volume_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("ReplayGain"), PILL_ACCESSORY_CHEVRON, false, replaygain_mode_settings_row_cb, NULL, NULL };
    if (count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "effects") > 0)
        items[count++] = (pill_list_item_t){ TR("Sound Effects"), PILL_ACCESSORY_CHEVRON, false, music_effects_menu_cb, NULL, NULL };

    lv_obj_t * scr = build_pill_list_screen(TR("Sound"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_eq_summary = settings_add_summary(eq_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_controls_screen(void) {
    static pill_list_item_t items[2 + PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Play/Pause Button"), PILL_ACCESSORY_CHEVRON, false, play_pause_button_mode_settings_row_cb, NULL, NULL };
    int count = 1;
    if (board_has(CAP_INLINE_REMOTE))
        items[count++] = (pill_list_item_t){ TR("In-line Remote"), PILL_ACCESSORY_TOGGLE,
                                              current_settings.inline_remote_enabled, NULL, inline_remote_switch_event_cb, NULL };
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS,
                                    plugin_manager_get_music_controls_list_item_count,
                                    plugin_manager_get_music_controls_list_item_label,
                                    plugin_manager_get_music_controls_list_item_options,
                                    plugin_music_controls_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Buttons & Remote"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void refresh_all_metadata_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_metadata_prompt();
}

static void refresh_all_covers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_covers_prompt();
}

static void refresh_sorting_options(void) {
    int album_sort = current_settings.album_sort_mode;
    if (!metadata_db_album_sort_available((metadata_db_album_sort_t) album_sort)) album_sort = 0;
    for (int i = 0; i < 5; i++) {
        bool selected = i < 2 ? current_settings.file_sort_mode == i : album_sort == i - 2;
        if (settings_sorting_options[i])
            lv_obj_set_style_border_width(settings_sorting_options[i], selected ? BOARD_SCALE_PX(3) : 0, 0);
    }
}

static void sorting_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int option = (int) (intptr_t) lv_event_get_user_data(e);
    if (option < 0 || option >= 5) return;
    if (option < 2) current_settings.file_sort_mode = option;
    else {
        metadata_db_album_sort_t sort = (metadata_db_album_sort_t) (option - 2);
        if (sort != METADATA_DB_ALBUM_SORT_NAME && !metadata_db_album_sort_available(sort)) {
            show_error_toast(TR("Update Music Database to enable this album order"));
            return;
        }
        current_settings.album_sort_mode = option - 2;
    }
    settings_save_async(&current_settings);
    gui_library_apply_sorting();
    refresh_sorting_options();
}

static lv_obj_t * build_sorting_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Sorting"), &title, &list);
    (void) title;
    add_section_header(list, TR("Files (folders stay first)"));
    static const char * labels[] = { N_("Name (A–Z)"), N_("Newest Modified"), N_("Name (A–Z)"),
                                     N_("Recently Added"), N_("Release Year (oldest first)") };
    for (int i = 0; i < 5; i++) {
        if (i == 2) add_section_header(list, TR("Albums (main list)"));
        settings_sorting_options[i] = add_pill_option_row(list, TR(labels[i]), false,
                                            sorting_option_cb, (void *) (intptr_t) i);
        lv_obj_add_style(settings_sorting_options[i], gui_theme_accent_outline_style(), 0);
    }
    lv_obj_t * help = add_section_header(list,
        TR("Files use modification dates. Albums use the newest track added; missing release years sort last. Update Music Database once to read years from existing files."));
    lv_obj_set_width(help, lv_pct(100));
    lv_obj_set_style_pad_right(help, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    refresh_sorting_options();
    return screen;
}

static void sorting_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sorting_screen) settings_sorting_screen = build_sorting_screen();
    if (!settings_sorting_screen) return;
    refresh_sorting_options();
    nav_push(settings_sorting_screen);
}

/* Update checks for added, removed and changed files; Refresh re-reads the
 * tags of every song even when a file looks unchanged. Distinct icons keep
 * the two from reading as the same action. */
static lv_obj_t * build_library_category_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS];
    lv_obj_t * native_rows[4] = { NULL };
    items[0] = (pill_list_item_t){ .label = TR("Update Music Database"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = update_music_database_row_cb,
        .out_row = &native_rows[0] };
    items[1] = (pill_list_item_t){ .label = TR("Refresh All Metadata"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_metadata_row_cb,
        .out_row = &native_rows[1] };
    items[2] = (pill_list_item_t){ .label = TR("Refresh All Covers"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_covers_row_cb,
        .out_row = &native_rows[2] };
    items[3] = (pill_list_item_t){ .label = TR("Sorting"), .accessory = PILL_ACCESSORY_CHEVRON,
        .on_click = sorting_row_cb, .out_row = &native_rows[3] };
    int count = append_plugin_list_rows(items, 4, PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS,
        plugin_manager_get_music_library_list_item_count,
        plugin_manager_get_music_library_list_item_label,
        plugin_manager_get_music_library_list_item_options,
        plugin_music_library_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Library"), generic_back_cb, items, count,
        gui_theme_accent_style(), GUI_ROW_GAP, 100);
    decorate_category_row(native_rows[0], "submenu/update_database.png", NULL);
    decorate_category_row(native_rows[1], "submenu/refresh_metadata.png", NULL);
    decorate_category_row(native_rows[2], "submenu/refresh_covers.png", NULL);
    decorate_category_row(native_rows[3], "settings/library.png", NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Display list row below --
 * same index-not-object shape plugin_settings_list_item_click_cb() (defined
 * further down, alongside build_settings_screen()) already uses. Forward-
 * declared static since build_settings_display_screen() is defined earlier
 * in the file than that sibling. */
static void plugin_display_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_display_list_item_clicked(index);
}

typedef struct {
    int scale;
    const char * label;
} animation_speed_option_t;

static const animation_speed_option_t animation_speed_options[] = {
    { 0, N_("Off") }, { 25, "0.25x" }, { 50, "0.5x" }, { 75, "0.75x" }, { 100, "1x" },
};
#define ANIMATION_SPEED_OPTION_COUNT (sizeof(animation_speed_options) / sizeof(animation_speed_options[0]))

static void animation_speed_option_row_cb(lv_event_t * e);

static void populate_animation_speed_screen(void) {
    if (!animation_speed_list) return;
    lv_obj_clean(animation_speed_list);
    for (size_t i = 0; i < ANIMATION_SPEED_OPTION_COUNT; i++) {
        bool selected = current_settings.animation_scale == animation_speed_options[i].scale;
        add_pill_option_row(animation_speed_list, TR(animation_speed_options[i].label),
                            selected, animation_speed_option_row_cb, (void *) (intptr_t) i);
    }
}

static void animation_speed_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.animation_scale = animation_speed_options[index].scale;
    settings_save(&current_settings);
    populate_animation_speed_screen();
}

static lv_obj_t * build_animation_speed_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Animation Speed"), &title_label, &animation_speed_list);
}

static void animation_speed_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_animation_speed_screen();
    nav_push(animation_speed_screen);
}

/* Gain = which stock volume table (High/Medium/Low) both players use. */
static lv_obj_t * gain_screen;
static lv_obj_t * gain_list;
static const char * const gain_option_labels[] = { N_("High"), N_("Medium"), N_("Low") };

static void gain_option_row_cb(lv_event_t * e);

static void populate_gain_screen(void) {
    if (!gain_list) return;
    lv_obj_clean(gain_list);
    for (int i = 0; i < 3; i++)
        add_pill_option_row(gain_list, TR(gain_option_labels[i]), current_settings.hw_gain_level == i,
                            gain_option_row_cb, (void *) (intptr_t) i);
}

static void gain_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int level = (int) (intptr_t) lv_event_get_user_data(e);
    if (!audio_load_board_volume_curve(level)) {
        show_error_toast(TR("Stock volume table not found"));
        return;
    }
    current_settings.hw_gain_level = level;
    settings_save(&current_settings);
    r3pro_stock_gain_write(level); /* the stock player starts with the same gain (BOARD.paths.stock_gain_ini) */
    populate_gain_screen();
}

static void gain_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gain_screen) {
        lv_obj_t * title_label;
        gain_screen = build_subsonic_list_screen(TR("Gain"), &title_label, &gain_list);
    }
    populate_gain_screen();
    nav_push(gain_screen);
}

static void upside_down_screen_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_upside_down = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_display_apply_rotation(current_settings.screen_upside_down);
}

static void quick_drawer_volume_visible_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.quick_drawer_volume_visible =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_shell_refresh_quick_drawer_volume_visibility();
}

static void artist_images_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_artist_images = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_library_set_artist_images_enabled(current_settings.show_artist_images);
}

void gui_display_apply_rotation(bool upside_down) {
    lv_display_t * disp = lv_display_get_default();
    if (!disp) return;
    lv_display_rotation_t target_rot = (upside_down != (BOARD.screen.rotation == 180)) ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0;
    if (lv_display_get_rotation(disp) == target_rot) return;
    lv_display_set_rotation(disp, target_rot);
    gui_navigation_invalidate_theme_snapshots();
    lv_obj_t * act = lv_screen_active();
    if (act) lv_obj_invalidate(act);
}

static lv_obj_t * build_settings_appearance_screen(void) {
    static pill_list_item_t items[6 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Accent Color"), PILL_ACCESSORY_CHEVRON, false, accent_color_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Font"), PILL_ACCESSORY_CHEVRON, false, custom_font_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Font Size"), PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Drawer Volume Slider"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.quick_drawer_volume_visible, NULL,
                                    quick_drawer_volume_visible_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ TR("Battery Percentage"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_battery_percent, NULL,
                                    battery_percent_switch_event_cb, NULL };
    items[5] = (pill_list_item_t){ TR("Artist Images"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_artist_images, NULL,
                                    artist_images_switch_event_cb, NULL };
    int count = append_grouped_plugin_rows(items, 6, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "appearance", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Appearance"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_player_layout_screen(void) {
    static pill_list_item_t items[3 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Lyrics"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_enabled, NULL, lyrics_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ TR("Lyrics Text Size"), PILL_ACCESSORY_CHEVRON, false,
                                    lyrics_font_size_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Hide Player/Lyrics Top Bar"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.hide_player_topbar, NULL,
                                    hide_player_topbar_switch_event_cb, NULL };
    int count = append_grouped_plugin_rows(items, 3, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "player_layout", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Player Layout"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_gestures_screen(void) {
    pill_list_item_t items[] = {
        { TR("Swipe Up for Home"), PILL_ACCESSORY_TOGGLE,
          current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL },
        { TR("Animation Speed"), PILL_ACCESSORY_CHEVRON, false,
          animation_speed_settings_row_cb, NULL, NULL },
        { TR("Upside Down Screen"), PILL_ACCESSORY_TOGGLE,
          current_settings.screen_upside_down, NULL, upside_down_screen_switch_event_cb, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Gestures & Orientation"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_appearance_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_appearance_screen) settings_appearance_screen = build_settings_appearance_screen();
    if (settings_appearance_screen) nav_push(settings_appearance_screen);
}

static void settings_player_layout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_player_layout_screen) settings_player_layout_screen = build_settings_player_layout_screen();
    if (settings_player_layout_screen) nav_push(settings_player_layout_screen);
}

static void settings_gestures_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_gestures_screen) settings_gestures_screen = build_settings_gestures_screen();
    if (settings_gestures_screen) nav_push(settings_gestures_screen);
}

static void tap_to_wake_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.tap_to_wake_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
}

static lv_obj_t * build_settings_display_screen(void) {
    static pill_list_item_t items[6 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Screen Timeout"), PILL_ACCESSORY_CHEVRON, false, screen_timeout_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Screen Dimming"), PILL_ACCESSORY_CHEVRON, false, screen_dimming_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Appearance"), PILL_ACCESSORY_CHEVRON, false, settings_appearance_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Player Layout"), PILL_ACCESSORY_CHEVRON, false, settings_player_layout_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Gestures & Orientation"), PILL_ACCESSORY_CHEVRON, false, settings_gestures_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("Double-tap to Wake"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.tap_to_wake_enabled, NULL, tap_to_wake_switch_event_cb, NULL };
    int count = append_grouped_plugin_rows(items, 6, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", NULL, plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Display"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Power list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_power_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_power_list_item_clicked(index);
}

static lv_obj_t * build_settings_charging_screen(void) {
    pill_list_item_t items[4];
    int n = 0;
    if (board_has(CAP_CHARGE_CONTROL)) {
        /* charge_control.c over the board's charger back end; the R3 Pro has
         * no CAP_CHARGE_CONTROL (its USB input-current policy is
         * CAP_INPUT_CURRENT, charger.c). */
        items[n++] = (pill_list_item_t){ TR("Charge Limit (85%)"), PILL_ACCESSORY_TOGGLE,
                                          current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL };
        if (charge_control_safe_current_supported())
            items[n++] = (pill_list_item_t){ TR("Safe Charging (500mA)"), PILL_ACCESSORY_TOGGLE,
                                              current_settings.safe_charging_enabled, NULL, safe_charging_switch_event_cb, NULL };
        /* Where a USB DAC mode exists and the charger can switch charging
         * (not with an MP2731: the AXP2101 does not own its charge path). */
        if (board_has(CAP_USB_DAC) && !board_impl_missing(CAP_USB_DAC) && charge_control_inhibit_supported())
            items[n++] = (pill_list_item_t){ TR("Don't charge in USB DAC mode"), PILL_ACCESSORY_TOGGLE,
                                              current_settings.dac_no_charge_enabled, NULL, dac_no_charge_switch_event_cb, NULL };
    }
    if (board_has(CAP_LEDS))
        items[n++] = (pill_list_item_t){ TR("LED charge indicator"), PILL_ACCESSORY_TOGGLE,
                                          current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("Charging"), generic_back_cb, items, n,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_charging_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_charging_screen) settings_charging_screen = build_settings_charging_screen();
    if (settings_charging_screen) nav_push(settings_charging_screen);
}

static void sleep_timer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_settings_open_sleep_timer();
}

static void plugin_music_timers_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        plugin_manager_music_timers_list_item_clicked((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * build_settings_power_screen(void) {
    static pill_list_item_t items[3 + PLUGIN_MAX_POWER_LIST_ITEMS + PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS];
    lv_obj_t * sleep_row = NULL;
    items[0] = (pill_list_item_t){ TR("Sleep Timer"), PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };
    items[0].out_row = &sleep_row;
    items[1] = (pill_list_item_t){ TR("Idle Shutdown"), PILL_ACCESSORY_CHEVRON, false, idle_shutdown_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Charging"), PILL_ACCESSORY_CHEVRON, false, settings_charging_row_cb, NULL, NULL };
    int count = 3;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS,
                                    plugin_manager_get_music_timers_list_item_count,
                                    plugin_manager_get_music_timers_list_item_label,
                                    plugin_manager_get_music_timers_list_item_options,
                                    plugin_music_timers_list_item_click_cb);
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_POWER_LIST_ITEMS,
                                    plugin_manager_get_power_list_item_count,
                                    plugin_manager_get_power_list_item_label,
                                    plugin_manager_get_power_list_item_options,
                                    plugin_power_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Power"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_sleep_summary = settings_add_summary(sleep_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered System list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_system_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_system_list_item_clicked(index);
}

/* Defined later, alongside the reboot-confirm popup this opens into (see
 * hostname_entry_done_cb()) -- needed here first, for build_settings_
 * system_screen()'s own row list below. */
static void hostname_row_cb(lv_event_t * e);
static void plugin_settings_list_item_click_cb(lv_event_t * e);
static void settings_about_row_cb(lv_event_t * e);
static void settings_tools_row_cb(lv_event_t * e);

static lv_obj_t * build_settings_tools_screen(void) {
    static pill_list_item_t items[PLUGIN_MAX_SETTINGS_LIST_ITEMS];
    int count = append_plugin_list_rows(items, 0, PLUGIN_MAX_SETTINGS_LIST_ITEMS,
                                        plugin_manager_get_settings_list_item_count,
                                        plugin_manager_get_settings_list_item_label,
                                        plugin_manager_get_settings_list_item_options,
                                        plugin_settings_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Additional Tools"), generic_back_cb, items,
                                            count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void clock_persist(void) {
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save(&current_settings);
}

static void clock_update_set_time_enabled(void) {
    if (!clock_set_time_row) return;
    if (current_settings.clock_automatic) {
        lv_obj_clear_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_40, 0);
    } else {
        lv_obj_add_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_COVER, 0);
    }
}

static void clock_automatic_changed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool automatic = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_set_automatic(automatic);
    current_settings.clock_automatic = automatic;
    clock_update_set_time_enabled();
    clock_persist();
    refresh_clock_label();
}

static void clock_set_time_save_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int hour = (int) lv_roller_get_selected(clock_hour_roller);
    int minute = (int) lv_roller_get_selected(clock_minute_roller);
    if (!current_settings.clock_24h) {
        hour = (hour + 1) % 12;
        if (lv_roller_get_selected(clock_ampm_roller) == 1) hour += 12;
    }
    app_clock_set_local_time(hour, minute);
    current_settings.clock_automatic = false;
    clock_persist();
    refresh_clock_label();
    nav_pop();
}

static void clock_set_time_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (current_settings.clock_automatic) {
        show_info_toast(TR("Turn off Automatic to set the clock"));
        return;
    }
    struct tm local;
    app_clock_localtime(&local);
    if (current_settings.clock_24h) {
        lv_roller_set_options(clock_hour_roller,
            "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23",
            LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) local.tm_hour, LV_ANIM_OFF);
        lv_obj_add_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_roller_set_options(clock_hour_roller, "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12",
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) ((local.tm_hour + 11) % 12), LV_ANIM_OFF);
        lv_roller_set_selected(clock_ampm_roller, local.tm_hour >= 12 ? 1 : 0, LV_ANIM_OFF);
        lv_obj_remove_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    }
    lv_roller_set_selected(clock_minute_roller, (uint32_t) local.tm_min, LV_ANIM_OFF);
    nav_push(clock_set_time_screen);
}

static void clock_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(clock_screen);
}

static lv_obj_t * build_clock_screen(void) {
    pill_list_item_t items[] = {
        { TR("Automatic"), PILL_ACCESSORY_TOGGLE, current_settings.clock_automatic,
          NULL, clock_automatic_changed_cb, NULL },
        { .label = TR("Set Time"), .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = clock_set_time_row_cb, .out_row = &clock_set_time_row },
        { TR("24-Hour Clock"), PILL_ACCESSORY_TOGGLE, current_settings.clock_24h,
          NULL, clock_24h_switch_event_cb, NULL },
        { .label = TR("Time Zone"), .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = timezone_settings_row_cb, .out_row = &clock_timezone_row },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Clock"), generic_back_cb, items, 4,
gui_theme_accent_style(), GUI_ROW_GAP, 100);
    if (clock_timezone_row) {
        /* Same two-line summary as every other settings row; it sizes the
         * row to its text instead of fixed offsets for a tall pill. */
        clock_timezone_value_label = settings_add_summary(clock_timezone_row);
        clock_timezone_indicator_update();
    }
    clock_update_set_time_enabled();
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_clock_set_time_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Set Time"), generic_back_cb, NULL, NULL);

    lv_obj_t * row = lv_obj_create(scr);
    lv_obj_set_size(row, lv_pct(92), BOARD_SCALE_PX(360));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 18);
    lv_obj_add_style(row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    clock_hour_roller = lv_roller_create(row);
    clock_minute_roller = lv_roller_create(row);
    clock_ampm_roller = lv_roller_create(row);
    lv_roller_set_options(clock_minute_roller,
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_options(clock_ampm_roller, "AM\nPM", LV_ROLLER_MODE_NORMAL);
    lv_obj_t * rollers[] = { clock_hour_roller, clock_minute_roller, clock_ampm_roller };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(rollers[i], i == 2 ? BOARD_SCALE_PX(105) : BOARD_SCALE_PX(120), BOARD_SCALE_PX(300));
        lv_obj_set_style_text_font(rollers[i], gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_add_style(rollers[i], gui_theme_accent_style(), LV_PART_SELECTED);
        /* style_accent deliberately sets both background and text to the
         * accent color (needed by several non-roller users), which makes a
         * roller's selected value disappear into its selection band. Keep
         * the accent background but explicitly restore themed primary text
         * for the selected part; adding this after the accent style gives
         * it precedence and still follows live theme color changes. */
        lv_obj_add_style(rollers[i], &style_theme_text_primary, LV_PART_SELECTED);
    }

    lv_obj_t * save = lv_button_create(scr);
    lv_obj_set_size(save, BOARD_SCALE_PX(220), BOARD_SCALE_PX(78));
    lv_obj_align_to(save, row, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);
    lv_obj_add_style(save, gui_theme_accent_style(), 0);
    lv_obj_add_event_cb(save, clock_set_time_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save);
    lv_label_set_text(save_label, TR("Save"));
    lv_obj_add_style(save_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(save_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(save_label);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_maintenance_screen(void) {
    pill_list_item_t items[] = {
        { TR("Hostname"), PILL_ACCESSORY_CHEVRON, false, hostname_row_cb, NULL, NULL },
        { TR("Reset settings"), PILL_ACCESSORY_NONE, false, factory_reset_btn_cb, NULL, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Maintenance"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_maintenance_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_system_maintenance_screen) settings_system_maintenance_screen = build_settings_maintenance_screen();
    if (settings_system_maintenance_screen) nav_push(settings_system_maintenance_screen);
}

/* Language choice: every language i18n knows, each under its own name.
 * Applying one rebuilds the UI so every screen is built in the new language. */
static void language_choice_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * code = i18n_language_code((size_t) (intptr_t) lv_event_get_user_data(e));
    if (!code || strcmp(code, i18n_get_language()) == 0) return;
    if (!i18n_set_language(code)) return;
    snprintf(current_settings.language, sizeof(current_settings.language), "%s", code);
    settings_save(&current_settings);
    show_info_toast(TR("Applying language, this may take a while"));
    gui_reload_request();
}

static void populate_language_choice_screen(void) {
    if (!language_choice_list) return;
    lv_obj_clean(language_choice_list);
    const char * active = i18n_get_language();
    for (size_t i = 0; i < i18n_language_count(); i++) {
        add_pill_option_row(language_choice_list, i18n_language_name(i), strcmp(i18n_language_code(i), active) == 0,
                            language_choice_row_cb, (void *) (intptr_t) i);
    }
}

static void language_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_language_choice_screen();
    nav_push(language_choice_screen);
}

static lv_obj_t * build_language_choice_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Language"), &title_label, &language_choice_list);
}

static lv_obj_t * build_settings_system_screen(void) {
    static pill_list_item_t items[7 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    int count = 0;
    /* Charge only, Storage and DAC (each per connection, chosen with a
     * computer on the cable); ADB alone lives in Developer Options. Only
     * where Storage or DAC is implemented (board_impl_missing()). */
    if ((board_has(CAP_USB_STORAGE) && !board_impl_missing(CAP_USB_STORAGE)) ||
        (board_has(CAP_USB_DAC) && !board_impl_missing(CAP_USB_DAC)))
        items[count++] = (pill_list_item_t){ TR("USB Mode"), PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Clock"), PILL_ACCESSORY_CHEVRON, false, clock_settings_row_cb, NULL, NULL };
    if (MT_HAS_FEATURE(I18N)) /* English only otherwise: nothing to choose */
        items[count++] = (pill_list_item_t){ TR("Language"), PILL_ACCESSORY_CHEVRON, false, language_settings_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Plugin Manager"), PILL_ACCESSORY_CHEVRON, false, gui_plugin_manage_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Maintenance"), PILL_ACCESSORY_CHEVRON, false, settings_maintenance_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("About"), PILL_ACCESSORY_CHEVRON, false, settings_about_row_cb, NULL, NULL };
    if (plugin_manager_get_settings_list_item_count() > 0) {
        items[count++] = (pill_list_item_t){ TR("Additional Tools"), PILL_ACCESSORY_CHEVRON, false,
                                             settings_tools_row_cb, NULL, NULL };
    }
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_SYSTEM_LIST_ITEMS,
                                    plugin_manager_get_system_list_item_count,
                                    plugin_manager_get_system_list_item_label,
                                    plugin_manager_get_system_list_item_options,
                                    plugin_system_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("System"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_category_music_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_audio_screen);
}

static void settings_category_playback_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_playback();
}

static void settings_category_library_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_library();
}

void gui_settings_open_library(void) {
    if (!settings_library_screen) settings_library_screen = build_library_category_screen();
    if (!settings_library_screen) return;
    nav_push(settings_library_screen);
}

static void settings_category_display_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_display_screen);
}

static void settings_category_power_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_power_screen);
}

static void settings_category_system_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_system_screen);
}

static void settings_about_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    refresh_about_update_row();
    nav_push(about_screen);
}

static void settings_tools_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !settings_tools_screen) return;
    nav_push(settings_tools_screen);
}

/* Auto-resume on launch has no settings row while AUTO_RESUME_ON_LAUNCH_ENABLED
 * is disabled at compile time. current_settings.auto_resume_enabled and
 * auto_resume_switch_event_cb remain available for when re-enabled. */
/* Shared click handler for every plugin-registered Settings list row below
 * -- index-not-object, so the row needs no per-row allocation. */
static void plugin_settings_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_settings_list_item_clicked(index);
}

/* Settings root: two titled groups (Listening, Device),
 * 44 px rows (so all eight fit below this board's header) with the current value on the right and a chevron. Wi-Fi and
 * Bluetooth live here because the clock home has no Wireless tile. Values
 * come from settings and cached radio state and refresh on every visit. */
extern char bt_connected_mac_cached[18];
extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;

typedef enum { SR_SOUND, SR_PLAYBACK, SR_DISPLAY, SR_LIBRARY, SR_WIFI, SR_BT, SR_REMOTE, SR_POWER, SR_SYSTEM, SR_STREAMING, SR_COUNT } settings_root_row_t;
static lv_obj_t * settings_root_values[SR_COUNT];

static void settings_root_open_wifi_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_wifi_screen();
}

static void settings_root_open_bt_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_bluetooth_screen();
}

/* Remote Control (and AirPlay, DLNA, Wi-Fi import where built in): the
 * clock-face Home has no Wireless shortcut, so they open from here. */
static void settings_root_open_wireless_services_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_network_open_wireless_services();
}

static void format_minutes_or_seconds(char * out, size_t size, const char * prefix, int seconds) {
    if (seconds < 60) snprintf(out, size, "%s%d s", prefix, seconds);
    else snprintf(out, size, "%s%d min", prefix, seconds / 60);
}

static void settings_root_refresh_values(void) {
    char buf[64];
    static const char * const gains[] = { N_("Gain High"), N_("Gain Medium"), N_("Gain Low") };
    int g = current_settings.hw_gain_level;
    if (settings_root_values[SR_SOUND]) lv_label_set_text(settings_root_values[SR_SOUND], TR(gains[(g >= 0 && g < 3) ? g : 0]));
    if (settings_root_values[SR_PLAYBACK])
        lv_label_set_text(settings_root_values[SR_PLAYBACK], current_settings.crossfade_enabled ? TR("Crossfade")
                          : current_settings.gapless_enabled ? TR("Gapless") : "");
    if (settings_root_values[SR_DISPLAY]) {
        if (current_settings.screen_timeout_enabled) format_minutes_or_seconds(buf, sizeof(buf), "", current_settings.screen_timeout_seconds);
        else snprintf(buf, sizeof(buf), "%s", TR("Always on"));
        lv_label_set_text(settings_root_values[SR_DISPLAY], buf);
    }
    if (settings_root_values[SR_WIFI]) {
        if (!gui_shell_wifi_effective_enabled()) snprintf(buf, sizeof(buf), "%s", TR("Off"));
        else if (!gui_network_cached_wifi_ssid(buf, sizeof(buf))) snprintf(buf, sizeof(buf), "%s", TR("On"));
        lv_label_set_text(settings_root_values[SR_WIFI], buf);
    }
    if (settings_root_values[SR_BT]) {
        const char * v = bt_is_powered_cached ? TR("On") : TR("Off");
        if (bt_connected_mac_cached[0]) {
            v = TR("Connected");
            for (int i = 0; i < bt_scan_result_count; i++)
                if (strcasecmp(bt_scan_results[i].mac, bt_connected_mac_cached) == 0 && bt_scan_results[i].name[0]) {
                    v = bt_scan_results[i].name;
                    break;
                }
        }
        lv_label_set_text(settings_root_values[SR_BT], v);
    }
    if (settings_root_values[SR_REMOTE] && gui_network_wireless_services_is_remote_only())
        lv_label_set_text(settings_root_values[SR_REMOTE], current_settings.remote_control_enabled ? TR("On") : TR("Off"));
    if (settings_root_values[SR_POWER]) {
        if (current_settings.idle_shutdown_enabled) format_minutes_or_seconds(buf, sizeof(buf), "", current_settings.idle_shutdown_minutes * 60);
        else snprintf(buf, sizeof(buf), "%s", TR("Stays on"));
        lv_label_set_text(settings_root_values[SR_POWER], buf);
    }
}

static void settings_root_loaded_cb(lv_event_t * e) {
    (void) e;
    settings_root_refresh_values();
}

static lv_obj_t * settings_root_group(lv_obj_t * list, const char * heading) {
    lv_obj_t * h = lv_label_create(list);
    lv_label_set_text(h, heading);
    lv_obj_add_style(h, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(h, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(h, LIST_ROW_WIDTH);
    lv_obj_set_style_pad_left(h, BOARD_UI_PX(10), 0);
    lv_obj_set_style_pad_top(h, BOARD_UI_PX(2), 0);
    lv_obj_t * group = lv_obj_create(list);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LIST_ROW_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(group, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group, BOARD_UI_PX(14), 0);
    lv_obj_set_style_clip_corner(group, true, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    return group;
}

static void settings_root_row(lv_obj_t * group, bool first, const char * icon, const char * label,
                              lv_event_cb_t cb, settings_root_row_t slot) {
    lv_obj_t * row = lv_obj_create(group);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(44)); /* eight rows fit one screen */
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    if (!first) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER), 0);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * img = lv_image_create(row);
    lv_image_set_src(img, asset_path(icon));
    lv_obj_set_size(img, BOARD_UI_PX(22), BOARD_UI_PX(22));
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, BOARD_UI_PX(14), 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_add_style(chev, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chev, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-14), 0);

    lv_obj_t * value = lv_label_create(row);
    lv_label_set_text(value, "");
    lv_obj_add_style(value, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(value, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(value, BOARD_UI_PX(120));
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-34), 0);
    settings_root_values[slot] = value;

    lv_obj_t * text = lv_label_create(row);
    lv_label_set_text(text, label);
    lv_obj_add_style(text, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(text, LIST_ROW_WIDTH - BOARD_UI_PX(48) - BOARD_UI_PX(160));
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, BOARD_UI_PX(48), 0);
}

static void settings_root_open_streaming_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_stream_settings_open();
}

/* The grouped settings root. */
static lv_obj_t * build_settings_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Settings"), generic_back_cb, NULL, NULL);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), lv_display_get_vertical_resolution(lv_display_get_default()) - top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, BOARD_UI_PX(4), 0);
    lv_obj_set_style_pad_bottom(list, BOARD_UI_PX(12), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t * g1 = settings_root_group(list, TR("Listening"));
    settings_root_row(g1, true, "settings/sound.png", TR("Sound"), settings_category_music_cb, SR_SOUND);
    settings_root_row(g1, false, "settings/playback.png", TR("Playback"), settings_category_playback_cb, SR_PLAYBACK);
    settings_root_row(g1, false, "settings/display.png", TR("Display"), settings_category_display_cb, SR_DISPLAY);
    settings_root_row(g1, false, "settings/library.png", TR("Library"), settings_category_library_cb, SR_LIBRARY);
    settings_root_row(g1, false, "launcher/stream_media.png", TR("Streaming"), settings_root_open_streaming_cb, SR_STREAMING);
    lv_obj_t * g2 = settings_root_group(list, TR("Device"));
    settings_root_row(g2, true, "wireless/list_wifi.png", TR("Wi-Fi"), settings_root_open_wifi_cb, SR_WIFI);
    settings_root_row(g2, false, "wireless/list_bt.png", TR("Bluetooth"), settings_root_open_bt_cb, SR_BT);
    if (gui_network_has_wireless_services())
        settings_root_row(g2, false, "wireless/list_remote.png",
                          gui_network_wireless_services_is_remote_only() ? TR("Remote Control") : TR("Wireless"),
                          settings_root_open_wireless_services_cb, SR_REMOTE);
    settings_root_row(g2, false, "settings/power.png", TR("Power"), settings_category_power_cb, SR_POWER);
    settings_root_row(g2, false, "settings/system.png", TR("System"), settings_category_system_cb, SR_SYSTEM);

    settings_root_refresh_values();
    lv_obj_add_event_cb(scr, settings_root_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Opens a home destination by its key -- used by the clock-face
 * home screen (gui_home_clock.c), which draws its own shortcuts. */
void gui_settings_open_home_destination(const char * key) {
    if (!key) return;
    if (strcmp(key, "music") == 0) nav_push(gui_library_get_music_screen());
    else if (strcmp(key, "stream_media") == 0) {
        gui_stream_home_open(); /* straight to the server's music */
    }
    else if (strcmp(key, "wireless") == 0) nav_push(gui_network_get_wireless_screen());
    else if (strcmp(key, "settings") == 0) nav_push(settings_screen);
    else if (strcmp(key, "radio") == 0) gui_radio_open();
}

/* EQ slider card with value label on top and slider track below.
 * Card uses flex-column flow with content-based sizing so card height
 * dynamically adjusts to the active font tier without overlapping. */
static lv_obj_t * create_eq_slider_card(lv_obj_t * parent, eq_field_t field, lv_obj_t ** out_value_label,
                                        lv_obj_t ** out_slider, int32_t range_min, int32_t range_max) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(96));
    /* Flex column with content height accommodates varying font heights. */
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    /* Clickable (tap-to-edit, see eq_field_label_click_cb()) -- the whole
     * label area is the tap target, not just the text glyphs, via
     * lv_obj_set_ext_click_area() below. */
    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value_label, 16);
    lv_obj_add_event_cb(value_label, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);

    /* Inset track width so the knob radius stays within the card at slider extremes. */
    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(88));
    lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
    lv_slider_set_range(slider, range_min, range_max);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(slider, BOARD_SCALE_PX(30));

    *out_value_label = value_label;
    *out_slider = slider;
    return card;
}

/* ---- PEQ named profiles (save/load to/from the SD card) ----
 *
 * peq.c's own peq_load()/peq_save() always target one fixed, always-current
 * file (loaded at startup, saved on every change) -- these profiles are a
 * separate, explicit "keep this exact setup around under a name" action,
 * living in their own SD card folder rather than mixed in with music. */
static lv_obj_t * eq_profiles_list;
static lv_obj_t * eq_profiles_title_label;
/* Name of the named profile whose values are currently loaded.  The PEQ
 * engine deliberately knows only about values and its always-current
 * autosave file, so the UI owns this bit of presentation state.  Saving
 * with this unchanged overwrites that profile; editing it creates a new
 * profile and makes the new name current. */

static void eq_set_current_profile_from_path(const char * path) {
    const char * name = basename_of(path);
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name ? name : "");
    char * dot = strrchr(eq_current_profile_name, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
}

/* Refreshes every widget on the EQ screen from peq.c's current state --
 * shared by the initial build and by "Load Profile" (which changes
 * everything at once, unlike the individual per-field setters that only
 * ever touch one widget). */
static void refresh_all_eq_widgets(void) {
    if (eq_bypass_switch) {
        if (peq_get_bypass()) lv_obj_clear_state(eq_bypass_switch, LV_STATE_CHECKED);
        else lv_obj_add_state(eq_bypass_switch, LV_STATE_CHECKED);
    }
    if (eq_bypass_state_label) lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    /* Off: the editor stays usable but reads as inactive. */
    if (eq_main_content) lv_obj_set_style_opa(eq_main_content, peq_get_bypass() ? LV_OPA_50 : LV_OPA_COVER, 0);
    if (eq_preamp_slider) lv_slider_set_value(eq_preamp_slider, (int32_t) (peq_get_preamp_db() * 10.0), LV_ANIM_OFF);
    if (eq_preamp_value_label) lv_label_set_text_fmt(eq_preamp_value_label, TR("Pre-Amp: %+.2f dB"), peq_get_preamp_db());
    refresh_eq_band_widgets();
    if (eq_profile_button_label) {
        char name[256];
        eq_graphic_current_name(name, sizeof(name));
        lv_label_set_text(eq_profile_button_label, name);
    }
    eq_graphic_refresh();
    if (eq_graph_visible && eq_graph_chart && eq_graph_series) {
        double f[128], db[128], peak = 0.0;
        for (size_t i = 0; i < 128; ++i) f[i] = EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, (double)i / 127.0);
        peq_get_response_db(f, db, 128, 48000);
        for (size_t i = 0; i < 128; ++i) if (fabs(db[i]) > peak) peak = fabs(db[i]);
        int range = (int)(ceil(peak / 6.0) * 6.0);
        if (range < 12) range = 12;
        if (range > 144) range = 144;
        if (eq_graph_min_label) lv_label_set_text_fmt(eq_graph_min_label, "-%d", range);
        if (eq_graph_zero_label) lv_label_set_text(eq_graph_zero_label, "0");
        if (eq_graph_max_label) lv_label_set_text_fmt(eq_graph_max_label, "+%d", range);
        lv_chart_set_range(eq_graph_chart, LV_CHART_AXIS_PRIMARY_Y, -range * 100, range * 100);
        for (size_t i = 0; i < 128; ++i) {
            int value = (int)lround(db[i] * 100.0);
            if (value < -range * 100) value = -range * 100;
            if (value > range * 100) value = range * 100;
            lv_chart_set_value_by_id(eq_graph_chart, eq_graph_series, (uint16_t)i, value);
        }
        lv_chart_refresh(eq_graph_chart);
        if (eq_graph_caption) lv_label_set_text(eq_graph_caption, peq_get_bypass() ? TR("Combined response (dB) · EQ off") : TR("Combined response (dB)"));
    }
}

/* ---- Reset PEQ to defaults, with a confirmation popup -- same hand-built
 * top-layer overlay shape as bt_dac_leave_popup (this codebase doesn't use
 * LVGL's lv_msgbox anywhere), since a factory reset of every band's
 * freq/gain/Q/type plus the preamp is not something a stray tap should be
 * able to trigger by accident. ---- */
static gui_popup_t eq_reset_popup;

static void hide_eq_reset_popup(void) {
    gui_popup_hide(&eq_reset_popup);
}

static void eq_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
    peq_reset_to_defaults();
    eq_current_profile_name[0] = '\0';
    refresh_all_eq_widgets();
    peq_save();
    show_error_toast(TR("PEQ reset to defaults"));
}

static void eq_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&eq_reset_popup);
}

static void build_eq_reset_popup(void) {
    eq_reset_popup.popup = build_confirm_popup(TR("Reset PEQ to defaults?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Reset"),
                                                lv_color_make(255, 120, 120), eq_reset_confirm_cb, NULL, TR("Cancel"),
                                                accent_lv_color(), eq_reset_cancel_cb, NULL, eq_reset_popup_backdrop_cb,
                                                &eq_reset_popup.backdrop);
}

/* ---- Factory Reset, with a confirmation popup -- same hand-built
 * top-layer overlay shape as eq_reset_popup right above (this codebase
 * doesn't use LVGL's lv_msgbox anywhere). Wiping every app setting is
 * exactly the kind of thing a stray tap must never be able to trigger.
 * Reboots immediately on confirm (settings_factory_reset() deletes the
 * settings file and returns -- see its own comment in settings.h for why
 * nothing here tries to hot-apply the reset settings instead of just
 * rebooting into them fresh). ---- */
static gui_popup_t factory_reset_popup;

static void hide_factory_reset_popup(void) {
    gui_popup_hide(&factory_reset_popup);
}

static void factory_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
    settings_factory_reset(); /* deletes the settings file and reboots -- see its own comment in settings.h/.c */
}

static void factory_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&factory_reset_popup);
}

static void build_factory_reset_popup(void) {
    factory_reset_popup.popup = build_confirm_popup(
        TR("Reset all settings and reboot?"), LV_LABEL_LONG_WRAP, NULL,
        TR("Your settings are kept in .mellowtone/backup on the SD card. Music and library stay."), TR("Reset"),
        lv_color_make(255, 120, 120),
        factory_reset_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), factory_reset_cancel_cb, NULL,
        factory_reset_popup_backdrop_cb, &factory_reset_popup.backdrop);
}

/* Settings -> System -> Hostname uses the shared confirmation-popup helper;
 * see hostname_apply()'s own comment for why a reboot is genuinely required
 * here (wifi_on.sh/bt_init each only read their file once, on demand). */
static gui_popup_t hostname_reboot_popup;

static void hide_hostname_reboot_popup(void) {
    gui_popup_hide(&hostname_reboot_popup);
}

static void hostname_reboot_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
    idle_shutdown_reboot_now();
}

static void show_hostname_reboot_popup(void) {
    gui_popup_show(&hostname_reboot_popup);
}

static void build_hostname_reboot_popup(void) {
    hostname_reboot_popup.popup = build_confirm_popup(
        TR("Restart now to apply the new hostname?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Restart Now"), accent_lv_color(),
        hostname_reboot_now_cb, NULL, TR("Later"), lv_color_make(160, 160, 160), hostname_reboot_later_cb, NULL,
        hostname_reboot_popup_backdrop_cb, &hostname_reboot_popup.backdrop);
}

/* RFC 952/1123 hostname-label charset -- letters/digits/hyphen only, no
 * leading/trailing hyphen. Real bug caught in review: nothing validated
 * this before it was written straight to /usr/data/hostname_override.txt
 * and handed to sethostname() (see hostname_apply.c) -- a space or
 * punctuation typed on the on-screen keyboard would produce an invalid
 * WiFi/BT broadcast name, or corrupt whatever naive parsing a downstream
 * consumer of those two bind-mounted files does. Empty is exempted --
 * that's hostname_entry_done_cb()'s own "reset to stock" sentinel below,
 * not a real hostname. */
static bool hostname_is_valid(const char * text) {
    size_t len = strlen(text);
    if (len == 0) return true;
    if (len > 63) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static void hostname_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!hostname_is_valid(text)) {
        show_error_toast(TR("Hostname can only use letters, numbers, and hyphens"));
        return;
    }
    /* Empty submission means "reset to the stock name" -- hostname_apply()
     * itself treats an empty string as a no-op (leaves whatever's already
     * in effect from a previous boot alone), so resetting to stock also
     * needs a reboot back to the un-overridden squashfs file, same as
     * setting a new one does. */
    snprintf(current_settings.hostname, sizeof(current_settings.hostname), "%s", text);
    settings_save(&current_settings);
    show_hostname_reboot_popup();
}

static void hostname_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Hostname"), current_settings.hostname, false, false, sizeof(current_settings.hostname),
                    hostname_entry_done_cb, NULL);
}

static char ** eq_profile_paths = NULL;
static int eq_profile_count = 0;
static bool eq_profiles_edit_mode = false;
/* When true, populate_eq_profiles_screen() wires rows to overwrite the
 * tapped profile with the CURRENT PEQ values instead of loading it --
 * entered only via the Save flow's "Replace Existing" choice (see
 * eq_open_save_choice_popup()), never a standalone navigation target. */
static bool eq_profiles_replace_mode = false;
static lv_obj_t * eq_profiles_edit_btn = NULL;
static char eq_profile_pending_path[512];
static gui_popup_t eq_profile_delete_popup;

static void eq_profiles_free_paths(void) {
    for (int i = 0; i < eq_profile_count; i++) free(eq_profile_paths[i]);
    free(eq_profile_paths);
    eq_profile_paths = NULL;
    eq_profile_count = 0;
}

static bool eq_profile_name_is_valid(const char * text) {
    if (!text || text[0] == '\0') return false;
    if (strchr(text, '/') || strchr(text, '\\')) return false;
    return true;
}

static void eq_profile_path_from_name(char * out, size_t out_size, const char * name) {
    snprintf(out, out_size, "%s/%s.peq", PEQ_PROFILES_DIR, name);
}

static bool eq_save_named_profile(const char * name) {
    if (!eq_profile_name_is_valid(name)) {
        show_error_toast(TR("Invalid profile name"));
        return false;
    }
    mkdir(PEQ_PROFILES_DIR, 0755);
    char path[512];
    eq_profile_path_from_name(path, sizeof(path), name);
    if (!peq_save_to_path(path)) {
        show_error_toast(TR("Failed to save profile"));
        return false;
    }
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name);
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile saved"));
    return true;
}

static void eq_profile_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_load_from_path(path)) {
        show_error_toast(TR("Failed to load profile"));
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    peq_save();
    nav_pop();
    show_info_toast(TR("Profile loaded"));
}

/* Replace-mode row tap: overwrites the tapped profile's file with the
 * CURRENT PEQ values (the opposite direction of eq_profile_row_cb's load) --
 * only reachable via the Save flow's "Replace Existing" choice, so tapping a
 * row here is already the user's one deliberate confirming action, same as
 * a normal in-place Save never asking again.
 * Writes to `path` directly (already a real, scanned, existing file -- no
 * need to re-validate a name or reconstruct it via eq_profile_path_from_name())
 * and only commits eq_current_profile_name/leaves the screen on success --
 * committing identity before the write could succeed would leave a FAILED
 * overwrite (SD full, unmount mid-write) pointing the next in-place Save at
 * the tapped file instead of whatever was actually current before this tap. */
static void eq_profile_replace_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_save_to_path(path)) {
        show_error_toast(TR("Failed to save profile"));
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile saved"));
    nav_pop();
}

static void hide_eq_profile_delete_popup(void) {
    gui_popup_hide(&eq_profile_delete_popup);
}

static void eq_profile_delete_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void eq_profile_delete_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void populate_eq_profiles_screen(void);

static void eq_profile_delete_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
    if (eq_profile_pending_path[0] == '\0') return;
    if (unlink(eq_profile_pending_path) != 0) {
        show_error_toast(TR("Failed to delete profile"));
        return;
    }
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(eq_profile_pending_path);
    if (strcmp(eq_current_profile_name, stem) == 0) eq_current_profile_name[0] = '\0';
    else snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", stem);
    eq_profile_pending_path[0] = '\0';
    populate_eq_profiles_screen();
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile deleted"));
}

static void eq_profile_delete_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    snprintf(eq_profile_pending_path, sizeof(eq_profile_pending_path), "%s", path);
    gui_popup_show(&eq_profile_delete_popup);
}

static void eq_profile_rename_done_cb(const char * text, void * user_data) {
    const char * old_path = (const char *) user_data;
    if (!eq_profile_name_is_valid(text)) {
        show_error_toast(TR("Invalid profile name"));
        return;
    }
    char new_path[512];
    eq_profile_path_from_name(new_path, sizeof(new_path), text);
    if (strcmp(old_path, new_path) == 0) return;
    if (rename(old_path, new_path) != 0) {
        show_error_toast(TR("Failed to rename profile"));
        return;
    }
    char previous[256];
    snprintf(previous, sizeof(previous), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(old_path);
    if (strcmp(eq_current_profile_name, previous) == 0)
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", text);
    else
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", previous);
    populate_eq_profiles_screen();
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile renamed"));
}

static void eq_profile_rename_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    char display[256];
    snprintf(display, sizeof(display), "%s", basename_of(path));
    char * dot = strrchr(display, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
    show_text_entry(TR("Rename Profile"), display, false, false, 0, eq_profile_rename_done_cb, (void *) path);
}

static void populate_eq_profiles_screen(void) {
    lv_obj_clean(eq_profiles_list);
    eq_profiles_free_paths();

    if (eq_profiles_title_label)
        lv_label_set_text(eq_profiles_title_label, eq_profiles_replace_mode ? TR("Replace Profile") : TR("Profiles"));
    if (eq_profiles_edit_btn) {
        lv_label_set_text(eq_profiles_edit_btn, eq_profiles_edit_mode ? TR("Done") : TR("Edit"));
        /* Rename/delete don't make sense mid-replace -- hide the entry point
         * entirely rather than let a mode switch there leave replace_mode
         * armed underneath a "Done" tap that returns to it unexpectedly. */
        if (eq_profiles_replace_mode) lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (!peq_scan_profiles(PEQ_PROFILES_DIR, &eq_profile_paths, &eq_profile_count)) {
        lv_obj_t * label = lv_label_create(eq_profiles_list);
        lv_label_set_text(label, TR("No saved profiles"));
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        if (!eq_profiles_edit_mode && !eq_profiles_replace_mode &&
            count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "profiles") > 0)
            add_pill_chevron_row(eq_profiles_list, TR("Download profiles"), music_profiles_menu_cb);
        return;
    }

    for (int i = 0; i < eq_profile_count; i++) {
        lv_obj_t * row = lv_obj_create(eq_profiles_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char display[256], stem[256];
        snprintf(stem, sizeof(stem), "%s", basename_of(eq_profile_paths[i]));
        char * dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        eq_profile_display_name(stem, display, sizeof(display));

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, display);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_width(label, LIST_ROW_WIDTH - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(64));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        if (eq_profiles_edit_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_rename_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
            lv_obj_t * delete_icon = lv_image_create(row);
            lv_image_set_src(delete_icon, asset_path("touch_list/del.png"));
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(delete_icon, eq_profile_delete_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else if (eq_profiles_replace_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_replace_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        }
    }
    if (!eq_profiles_edit_mode && !eq_profiles_replace_mode &&
        count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "profiles") > 0)
        add_pill_chevron_row(eq_profiles_list, TR("Download profiles"), music_profiles_menu_cb);
}

/* Catches every way this screen can be left (back button, back-swipe, Home
 * swipe) -- not just eq_profile_replace_row_cb's own success-path nav_pop()
 * -- so replace mode can never stay armed into a later, unrelated visit to
 * this same screen via "Load Profile". */
static void eq_profiles_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    eq_profiles_replace_mode = false;
}

static void eq_profiles_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    if (!eq_profiles_edit_mode && !eq_profiles_replace_mode) populate_eq_profiles_screen();
}

static void eq_profiles_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = !eq_profiles_edit_mode;
    populate_eq_profiles_screen();
}

static void build_eq_profile_delete_popup(void) {
    eq_profile_delete_popup.popup = build_confirm_popup(TR("Delete this profile?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Delete"),
                                                         lv_color_make(255, 120, 120), eq_profile_delete_confirm_cb, NULL,
                                                         TR("Cancel"), accent_lv_color(), eq_profile_delete_cancel_cb, NULL,
                                                         eq_profile_delete_popup_backdrop_cb, &eq_profile_delete_popup.backdrop);
}

static lv_obj_t * build_eq_profiles_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen(TR("Profiles"), &eq_profiles_title_label, &eq_profiles_list);
    lv_obj_add_event_cb(scr, eq_profiles_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, eq_profiles_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    eq_profiles_edit_btn = lv_label_create(scr);
    lv_label_set_text(eq_profiles_edit_btn, TR("Edit"));
    lv_obj_add_style(eq_profiles_edit_btn, gui_theme_accent_style(), 0);
    lv_obj_set_ext_click_area(eq_profiles_edit_btn, BOARD_SCALE_PX(16));
    lv_obj_set_style_text_font(eq_profiles_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(eq_profiles_edit_btn, 20);
    lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_profiles_edit_btn, eq_profiles_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    if (eq_profiles_title_label) reserve_title_width_before(eq_profiles_title_label, eq_profiles_edit_btn);
    return scr;
}

static void eq_load_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = false;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

/* Entered only from the Save flow's "Replace Existing" choice -- see
 * eq_open_save_choice_popup(). */
static void eq_open_profiles_for_replace(void) {
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = true;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

static void eq_save_profile_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    eq_save_named_profile(text);
}

/* Save-choice popup: offered whenever a Save action would otherwise go
 * straight to a blank/prefilled text entry (no current profile, or a
 * deliberate "Save As" hold) AND at least one profile already exists to
 * replace -- reported gap: users had no way to pick an EXISTING preset to
 * overwrite, only a text box to name a new one. `eq_save_choice_prefill_current`
 * remembers which of those two triggered it, so "New Profile" opens the
 * right text-entry variant once the choice is made. */
static gui_popup_t eq_save_choice_popup;
static bool eq_save_choice_prefill_current = false;

static void eq_hide_save_choice_popup(void) {
    gui_popup_hide(&eq_save_choice_popup);
}

static void eq_save_choice_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
}

static void eq_save_choice_new_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    const char * prefill = eq_save_choice_prefill_current ? eq_current_profile_name : "";
    const char * title = eq_save_choice_prefill_current ? TR("Save Profile As") : TR("Profile Name");
    show_text_entry(title, prefill, false, false, 0, eq_save_profile_name_done_cb, NULL);
}

static void eq_save_choice_replace_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    eq_open_profiles_for_replace();
}

static void build_eq_save_choice_popup(void) {
    eq_save_choice_popup.popup = build_confirm_popup(
        TR("Save Profile"), LV_LABEL_LONG_WRAP, NULL,
        TR("Save as a new profile, or replace one that already exists?"),
        TR("Replace Existing"), lv_color_make(255, 120, 120), eq_save_choice_replace_cb, NULL,
        TR("New Profile"), accent_lv_color(), eq_save_choice_new_cb, NULL,
        eq_save_choice_backdrop_cb, &eq_save_choice_popup.backdrop);
}

/* Shows the choice popup when there's at least one existing profile to
 * offer replacing; otherwise (first-ever save) goes straight to the same
 * text-entry a plain "New Profile" choice would have opened, since there's
 * nothing yet to replace. */
static void eq_open_save_choice_popup(bool prefill_current_name) {
    char ** existing_paths = NULL;
    int existing_count = 0;
    bool have_existing = peq_scan_profiles(PEQ_PROFILES_DIR, &existing_paths, &existing_count) && existing_count > 0;
    for (int i = 0; i < existing_count; i++) free(existing_paths[i]);
    free(existing_paths);

    if (!have_existing) {
        const char * prefill = prefill_current_name ? eq_current_profile_name : "";
        const char * title = prefill_current_name ? TR("Save Profile As") : TR("Profile Name");
        show_text_entry(title, prefill, false, false, 0, eq_save_profile_name_done_cb, NULL);
        return;
    }

    eq_save_choice_prefill_current = prefill_current_name;
    gui_popup_show(&eq_save_choice_popup);
}

/* Save Profile: a quick tap saves in place; a deliberate hold opens the save
 * choice above (prefilled with the current name if choosing "New Profile").
 * Tracked locally via our own PRESSED/CLICKED timestamps rather than LVGL's
 * global LV_EVENT_LONG_PRESSED (LV_INDEV_DEF_LONG_PRESS_TIME, 400ms, shared
 * by every long-press in the app) -- 400ms was too easy to trip on an
 * ordinary deliberate tap here, especially right after a run of slider
 * drags, so this button alone uses a longer local hold. */
#define EQ_SAVE_PROFILE_HOLD_MS 700
static uint32_t eq_save_profile_press_start_ms = 0;

static void eq_save_profile_pressed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    eq_save_profile_press_start_ms = lv_tick_get();
}

static void eq_save_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool held = lv_tick_elaps(eq_save_profile_press_start_ms) >= EQ_SAVE_PROFILE_HOLD_MS;
    if (held) {
        eq_open_save_choice_popup(true);
        return;
    }
    if (eq_current_profile_name[0]) {
        eq_save_named_profile(eq_current_profile_name);
        return;
    }
    eq_open_save_choice_popup(false);
}

static lv_obj_t * eq_make_slider_card(lv_obj_t * parent, eq_field_t field, const char * title,
                                       lv_obj_t ** value_out, lv_obj_t ** slider_out,
                                       int32_t minimum, int32_t maximum, const char * low, const char * high) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(96));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_hor(card, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(card, BOARD_SCALE_PX(8), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(3), 0);
    lv_obj_t * top_row = lv_obj_create(card);
    lv_obj_set_width(top_row, lv_pct(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_remove_flag(top_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * heading = lv_label_create(top_row);
    lv_label_set_text(heading, title);
    lv_obj_add_style(heading, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(heading, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_t * value = lv_label_create(top_row);
    lv_obj_add_style(value, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    if (BOARD.ui.stock_font) {
        /* The stock UI font's taller lines leave no room for the Montserrat-era
         * gap; the knob needs only about its own radius above the rail. */
        lv_obj_set_style_margin_bottom(top_row, BOARD_SCALE_PX(12), 0);
    } else {
        lv_obj_set_style_margin_bottom(top_row, BOARD_SCALE_PX(48), 0);
    }
    lv_obj_add_flag(value, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value, 14);
    lv_obj_add_event_cb(value, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)field);
    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(96));
    lv_obj_set_height(slider, BOARD_SCALE_PX(6));
    lv_slider_set_range(slider, minimum, maximum);
    lv_obj_set_style_bg_color(slider, lv_color_make(75, 83, 91), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_outline_style(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, BOARD_SCALE_PX(2), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, BOARD_SCALE_PX(9), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    /* Keep the thin rail while accepting touches across a 66 px tall area.
     * Disable knob-only hit testing so any point along the rail can drag. */
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(slider, BOARD_SCALE_PX(30));
    lv_obj_t * captions = lv_obj_create(card);
    lv_obj_set_width(captions, lv_pct(94));
    lv_obj_set_height(captions, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(captions, 0, 0);
    lv_obj_set_style_border_width(captions, 0, 0);
    lv_obj_set_style_pad_all(captions, 0, 0);
    if (BOARD.ui.stock_font) {
        lv_obj_set_style_margin_top(captions, BOARD_SCALE_PX(4), 0); /* all three cards fit unscrolled */
    } else {
        lv_obj_set_style_margin_top(captions, BOARD_SCALE_PX(10), 0);
    }
    lv_obj_remove_flag(captions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(captions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(captions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * min_label = lv_label_create(captions);
    lv_label_set_text(min_label, low);
    lv_obj_add_style(min_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(min_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_t * max_label = lv_label_create(captions);
    lv_label_set_text(max_label, high);
    lv_obj_add_style(max_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(max_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    /* Keep captions from intercepting the lower portion of the expanded
     * slider click area (hit testing visits later children first). */
    lv_obj_remove_flag(captions, LV_OBJ_FLAG_CLICKABLE);
    *value_out = value;
    *slider_out = slider;
    if (!BOARD.ui.stock_font) {
        lv_obj_set_flex_grow(card, 1);
    }
    /* (Stock UI font: natural height -- growing into the fixed area squeezed
     * the cards below their content and clipped title and range labels;
     * the content column scrolls instead.) */
    return card;
}

static void eq_flat_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (eq_graphic_mode) {
        static const double zeros[EQ_GRAPHIC_BANDS];
        eq_graphic_apply(zeros);
        eq_current_profile_name[0] = '\0';
        refresh_all_eq_widgets();
        peq_save();
        return;
    }
    peq_reset_to_defaults();
    eq_current_profile_name[0] = '\0';
    refresh_all_eq_widgets();
    peq_save();
}

static lv_obj_t * build_eq_band_options_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Band options"), generic_back_cb, NULL, NULL);
    lv_obj_t * content = lv_obj_create(scr);
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), screen_h - content_top);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_add_style(content, &style_theme_screen_bg, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, 16, 0);
    lv_obj_t * preamp = create_eq_slider_card(content, EQ_FIELD_PREAMP, &eq_preamp_value_label, &eq_preamp_slider, -120, 120);
    lv_obj_t * title = lv_label_create(content);
    lv_label_set_text(title, TR("Filter type"));
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    eq_type_dropdown = lv_dropdown_create(content);
    lv_dropdown_set_options(eq_type_dropdown, TR("Peaking\nLow Shelf\nHigh Shelf"));
    lv_obj_set_width(eq_type_dropdown, lv_pct(95));
    style_settings_dropdown(eq_type_dropdown);
    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_width(enable_row, lv_pct(95));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_add_style(enable_row, &style_theme_card_bg, 0);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * enable = lv_label_create(enable_row);
    lv_label_set_text(enable, TR("Enable band"));
    lv_obj_add_style(enable, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    eq_band_enabled_switch = lv_switch_create(enable_row);
    lv_obj_add_style(eq_band_enabled_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_t * reset = lv_label_create(content);
    lv_label_set_text(reset, TR("Reset to defaults"));
    lv_obj_add_style(reset, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(reset, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_color(reset, lv_color_make(255, 120, 120), 0);
    lv_obj_add_flag(reset, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reset, eq_reset_btn_cb, LV_EVENT_CLICKED, NULL);
    refresh_all_eq_widgets();
    lv_obj_add_event_cb(eq_preamp_slider, eq_preamp_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_band_enabled_switch, eq_band_enabled_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_type_dropdown, eq_type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    finalize_screen_navigation(scr);
    lv_obj_remove_flag(preamp, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(preamp);
    return scr;
}

static void eq_style_card(lv_obj_t * card, lv_obj_t * slider, lv_obj_t * value) {
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_text_font(value, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_text_color(value, lv_color_hex(GUI_COLOR_TEXT), 0);
    lv_obj_set_height(slider, 8);
    lv_obj_set_style_bg_color(slider, lv_color_hex(GUI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(GUI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 14);
    /* Compact: the value is shown (and typed) above, so the range captions go
     * and all three cards fit without scrolling. */
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_t * top_row = lv_obj_get_child(card, 0);
    lv_obj_t * captions = lv_obj_get_child(card, 2);
    if (top_row) lv_obj_set_style_margin_bottom(top_row, 10, 0);
    if (captions) lv_obj_add_flag(captions, LV_OBJ_FLAG_HIDDEN);
}

static void eq_style_footer_button(lv_obj_t * btn, bool primary) {
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(GUI_COLOR_CONTROL), 0);
    lv_obj_set_style_border_width(btn, primary ? 1 : 0, 0);
}


/* ---- Graphic EQ --------------------------------------------------------
 * Winamp-style: ten vertical sliders at fixed centres with a smooth curve
 * through the knobs, and a preset sheet whose rows show each curve. It
 * drives the same ten parametric bands (eq_presets.c); Parametric stays one
 * tap away. Touching a slider or picking a preset is what moves the bands
 * onto the graphic grid; just looking does not. */
#define EQ_CURVE_SUB 4
#define EQ_CURVE_POINTS ((EQ_GRAPHIC_BANDS - 1) * EQ_CURVE_SUB + 1)
#define EQ_GRAPHIC_RANGE 120 /* slider units: 0.1 dB */

static bool eq_graphic_mode = true;
static lv_obj_t * eq_mode_btn[2];
static lv_obj_t * eq_graphic_box;
static lv_obj_t * eq_graphic_slider[EQ_GRAPHIC_BANDS];
static lv_obj_t * eq_graphic_curve;
static lv_obj_t * eq_graphic_preamp_label;
static lv_obj_t * eq_param_widgets[6];
static lv_obj_t * eq_preset_btn_curve;
static lv_point_precise_t eq_graphic_curve_pts[EQ_CURVE_POINTS];
static lv_point_precise_t eq_preset_btn_pts[EQ_CURVE_POINTS];
static lv_point_precise_t eq_sheet_pts[24][EQ_CURVE_POINTS];
static lv_obj_t * eq_sheet_backdrop;
static lv_obj_t * eq_sheet;
static char ** eq_sheet_paths;
static int eq_sheet_path_count;
static int32_t eq_slider_x[EQ_GRAPHIC_BANDS];
static int32_t eq_slider_top, eq_slider_h;

/* Catmull-Rom through ten (x, y) points, EQ_CURVE_SUB steps per segment. */
static void eq_spline(const float * xs, const float * ys, lv_point_precise_t * out) {
    int k = 0;
    for (int i = 0; i < EQ_GRAPHIC_BANDS - 1; i++) {
        int i0 = i ? i - 1 : i, i3 = i + 2 < EQ_GRAPHIC_BANDS ? i + 2 : i + 1;
        for (int s = 0; s < EQ_CURVE_SUB; s++) {
            float t = (float) s / EQ_CURVE_SUB, t2 = t * t, t3 = t2 * t;
            float a = -0.5f * t3 + t2 - 0.5f * t, b = 1.5f * t3 - 2.5f * t2 + 1, c = -1.5f * t3 + 2 * t2 + 0.5f * t,
                  d = 0.5f * t3 - 0.5f * t2;
            out[k].x = a * xs[i0] + b * xs[i] + c * xs[i + 1] + d * xs[i3];
            out[k].y = a * ys[i0] + b * ys[i] + c * ys[i + 1] + d * ys[i3];
            k++;
        }
    }
    out[k].x = xs[EQ_GRAPHIC_BANDS - 1];
    out[k].y = ys[EQ_GRAPHIC_BANDS - 1];
}

/* Curve of `gains` in a w x h box, +-12 dB full scale. */
static void eq_thumb_points(const double * gains, int32_t w, int32_t h, lv_point_precise_t * out) {
    float xs[EQ_GRAPHIC_BANDS], ys[EQ_GRAPHIC_BANDS];
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        xs[i] = 3 + (float) i * (w - 6) / (EQ_GRAPHIC_BANDS - 1);
        ys[i] = h / 2.0f - (float) (gains[i] / EQ_GRAPHIC_GAIN_MAX_DB) * (h / 2.0f - 3);
    }
    eq_spline(xs, ys, out);
}

static lv_obj_t * eq_make_curve_line(lv_obj_t * parent, lv_point_precise_t * pts, int32_t width, lv_opa_t opa) {
    lv_obj_t * line = lv_line_create(parent);
    lv_line_set_points(line, pts, EQ_CURVE_POINTS);
    lv_obj_set_style_line_color(line, accent_lv_color(), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    return line;
}

static void eq_graphic_refresh(void) {
    if (!eq_graphic_box) return;
    double g[EQ_GRAPHIC_BANDS];
    eq_graphic_read(g);
    float xs[EQ_GRAPHIC_BANDS], ys[EQ_GRAPHIC_BANDS];
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        lv_obj_t * sl = eq_graphic_slider[i];
        if (sl && !(lv_obj_has_state(sl, LV_STATE_PRESSED)))
            lv_slider_set_value(sl, (int32_t) lround(g[i] * 10), LV_ANIM_OFF);
        double v = sl ? lv_slider_get_value(sl) / 10.0 : g[i];
        xs[i] = eq_slider_x[i];
        ys[i] = eq_slider_top + (float) (1.0 - (v + EQ_GRAPHIC_GAIN_MAX_DB) / (2 * EQ_GRAPHIC_GAIN_MAX_DB)) * eq_slider_h;
    }
    eq_spline(xs, ys, eq_graphic_curve_pts);
    if (eq_graphic_curve) lv_line_set_points(eq_graphic_curve, eq_graphic_curve_pts, EQ_CURVE_POINTS);
    if (eq_graphic_preamp_label) {
        static const char * const modes[3] = { N_("balanced"), N_("safe"), N_("off") };
        int m = current_settings.eq_preamp_mode;
        char text[96];
        /* TRANSLATORS: preamp gain in dB, then its mode ("balanced", "safe" or "off"). */
        snprintf(text, sizeof(text), TR("Preamp %+.1f dB · %s"), peq_get_preamp_db(), TR(modes[(m >= 0 && m < 3) ? m : 0]));
        size_t len = strlen(text);
        snprintf(text + len, sizeof(text) - len, "  %s", LV_SYMBOL_RIGHT);
        lv_label_set_text(eq_graphic_preamp_label, text);
    }
    if (eq_preset_btn_curve) {
        eq_thumb_points(g, BOARD_UI_PX(26), BOARD_UI_PX(16), eq_preset_btn_pts);
        lv_line_set_points(eq_preset_btn_curve, eq_preset_btn_pts, EQ_CURVE_POINTS);
    }
}

/* Name for the preset button: a matching preset, Flat, the loaded profile,
 * or Custom. */
static void eq_graphic_current_name(char * out, size_t size) {
    int p = eq_preset_match();
    if (p >= 0) {
        snprintf(out, size, "%s", TR(eq_preset_get(p)->name));
        return;
    }
    double g[EQ_GRAPHIC_BANDS];
    bool grid = eq_graphic_read(g), flat = grid;
    for (int i = 0; i < EQ_GRAPHIC_BANDS && flat; i++) flat = fabs(g[i]) < 0.05;
    if (flat) snprintf(out, size, "%s", TR("Flat"));
    else eq_profile_display_name(eq_current_profile_name, out, size);
}

static void eq_graphic_slider_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    double g[EQ_GRAPHIC_BANDS];
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) g[i] = lv_slider_get_value(eq_graphic_slider[i]) / 10.0;
    eq_graphic_apply(g);
    if (code == LV_EVENT_VALUE_CHANGED) {
        eq_graphic_refresh();
        return;
    }
    eq_current_profile_name[0] = '\0'; /* edited: Custom until saved */
    peq_save();
    refresh_all_eq_widgets();
}

/* Preamp rule picker: a small action sheet from the preamp line. */
static lv_obj_t * eq_preamp_popup, * eq_preamp_backdrop;

static void eq_preamp_popup_close(void) {
    if (eq_preamp_backdrop) { lv_obj_delete_async(eq_preamp_backdrop); eq_preamp_backdrop = NULL; }
    if (eq_preamp_popup) { lv_obj_delete_async(eq_preamp_popup); eq_preamp_popup = NULL; }
}

static void eq_preamp_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_preamp_popup_close();
}

static void eq_preamp_choose(int mode) {
    eq_preamp_popup_close();
    current_settings.eq_preamp_mode = mode;
    settings_save_async(&current_settings);
    eq_graphic_set_preamp_mode((eq_preamp_mode_t) mode);
    /* Re-derive the preamp for the current curve (graphic grid only; a
     * parametric setup keeps its own preamp). */
    double g[EQ_GRAPHIC_BANDS];
    if (eq_graphic_read(g)) {
        eq_graphic_apply(g);
        peq_save();
    }
    refresh_all_eq_widgets();
}

static void eq_preamp_balanced_cb(lv_event_t * e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_preamp_choose(0); }
static void eq_preamp_safe_cb(lv_event_t * e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_preamp_choose(1); }
static void eq_preamp_off_cb(lv_event_t * e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_preamp_choose(2); }

static void eq_preamp_line_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || eq_preamp_popup) return;
    int m = current_settings.eq_preamp_mode;
    const menu_popup_row_t rows[] = {
        { TR("Balanced: louder, limiter catches rare peaks"), eq_preamp_balanced_cb, false, false,
          m == 0 ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY },
        { TR("Safe: never clips, quietest"), eq_preamp_safe_cb, false, false, m == 1 ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY },
        { TR("Off: like Winamp, limiter only"), eq_preamp_off_cb, false, false, m == 2 ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY },
    };
    eq_preamp_popup = build_menu_popup(rows, 3, eq_preamp_backdrop_cb, &eq_preamp_backdrop);
    lv_obj_remove_flag(eq_preamp_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eq_preamp_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(eq_preamp_backdrop);
    lv_obj_move_foreground(eq_preamp_popup);
}

static void eq_set_mode(bool graphic) {
    eq_graphic_mode = graphic;
    for (int i = 0; i < 2; i++) {
        bool on = (i == 0) == graphic;
        lv_obj_set_style_bg_opa(eq_mode_btn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(eq_mode_btn[i], 0), on ? lv_color_hex(GUI_COLOR_ON_ACCENT) : lv_color_hex(GUI_COLOR_TEXT_SOFT), 0);
    }
    if (graphic) {
        lv_obj_remove_flag(eq_graphic_box, LV_OBJ_FLAG_HIDDEN);
        for (size_t i = 0; i < sizeof(eq_param_widgets) / sizeof(eq_param_widgets[0]); i++)
            if (eq_param_widgets[i]) lv_obj_add_flag(eq_param_widgets[i], LV_OBJ_FLAG_HIDDEN);
        refresh_all_eq_widgets();
    } else {
        lv_obj_add_flag(eq_graphic_box, LV_OBJ_FLAG_HIDDEN);
        if (eq_param_widgets[0]) lv_obj_remove_flag(eq_param_widgets[0], LV_OBJ_FLAG_HIDDEN);
        eq_show_stop(eq_graph_visible ? PEQ_NUM_BANDS : current_eq_band); /* re-shows cards or curve */
    }
}

static void eq_mode_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_set_mode(lv_event_get_user_data(e) != NULL);
}

static void eq_sheet_close(void) {
    /* Called from taps on the sheet's own rows: delete after the event. */
    if (eq_sheet_backdrop) { lv_obj_delete_async(eq_sheet_backdrop); eq_sheet_backdrop = NULL; }
    if (eq_sheet) { lv_obj_delete_async(eq_sheet); eq_sheet = NULL; }
    for (int i = 0; i < eq_sheet_path_count; i++) free(eq_sheet_paths[i]);
    free(eq_sheet_paths);
    eq_sheet_paths = NULL;
    eq_sheet_path_count = 0;
}

static void eq_sheet_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) eq_sheet_close();
}

static void eq_sheet_preset_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    eq_preset_apply(index);
    eq_current_profile_name[0] = '\0';
    peq_set_bypass(false); /* picking a sound means hearing it */
    peq_save();
    eq_sheet_close();
    if (!eq_graphic_mode) eq_set_mode(true);
    refresh_all_eq_widgets();
}

static void eq_sheet_profile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= eq_sheet_path_count || !peq_load_from_path(eq_sheet_paths[index])) {
        show_error_toast(TR("Failed to load profile"));
        return;
    }
    eq_set_current_profile_from_path(eq_sheet_paths[index]);
    peq_save();
    eq_sheet_close();
    refresh_all_eq_widgets();
}

static void eq_sheet_manage_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_sheet_close();
    eq_load_profile_btn_cb(e);
}

static lv_obj_t * eq_sheet_row(lv_obj_t * list, const char * text, bool selected) {
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(46));
    lv_obj_set_style_radius(row, BOARD_UI_PX(10), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_color(l, selected ? accent_lv_color() : lv_color_hex(GUI_COLOR_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, BOARD_UI_PX(88), 0);
    if (selected) {
        lv_obj_t * check = lv_label_create(row);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(check, accent_lv_color(), 0);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-12), 0);
    }
    return row;
}

static void eq_sheet_section(lv_obj_t * list, const char * text) {
    lv_obj_t * l = lv_label_create(list);
    lv_label_set_text(l, text);
    lv_obj_add_style(l, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(l, BOARD_UI_PX(10), 0);
    lv_obj_set_style_pad_left(l, BOARD_UI_PX(10), 0);
}

static void eq_preset_sheet_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || eq_sheet) return;
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    eq_sheet_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(eq_sheet_backdrop);
    lv_obj_set_size(eq_sheet_backdrop, w, h);
    lv_obj_set_style_bg_color(eq_sheet_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eq_sheet_backdrop, LV_OPA_50, 0);
    lv_obj_add_flag(eq_sheet_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_sheet_backdrop, eq_sheet_backdrop_cb, LV_EVENT_CLICKED, NULL);

    eq_sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(eq_sheet);
    lv_obj_set_size(eq_sheet, w, h - BOARD_UI_PX(70));
    lv_obj_set_pos(eq_sheet, 0, BOARD_UI_PX(70));
    lv_obj_set_style_radius(eq_sheet, BOARD_UI_PX(20), 0);
    lv_obj_set_style_bg_color(eq_sheet, lv_color_hex(GUI_COLOR_SHEET), 0);
    lv_obj_set_style_bg_opa(eq_sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(eq_sheet, LV_OBJ_FLAG_CLICKABLE); /* taps on the sheet do not close it */
    lv_obj_remove_flag(eq_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * handle = lv_obj_create(eq_sheet);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, BOARD_UI_PX(36), BOARD_UI_PX(4));
    lv_obj_set_style_radius(handle, BOARD_UI_PX(2), 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(GUI_COLOR_HANDLE), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, BOARD_UI_PX(8));
    lv_obj_t * title = lv_label_create(eq_sheet);
    lv_label_set_text(title, TR("Presets"));
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(GUI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, BOARD_UI_PX(20), BOARD_UI_PX(20));

    lv_obj_t * list = lv_obj_create(eq_sheet);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, w - BOARD_UI_PX(20), h - BOARD_UI_PX(70) - BOARD_UI_PX(56));
    lv_obj_set_pos(list, BOARD_UI_PX(10), BOARD_UI_PX(52));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(list, BOARD_UI_PX(16), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    int current = eq_preset_match();
    lv_obj_t * selected_row = NULL;
    for (int i = 0; i < eq_preset_count() && i < (int) (sizeof(eq_sheet_pts) / sizeof(eq_sheet_pts[0])); i++) {
        const eq_preset_t * p = eq_preset_get(i);
        lv_obj_t * row = eq_sheet_row(list, TR(p->name), i == current);
        if (i == current) selected_row = row;
        lv_obj_add_event_cb(row, eq_sheet_preset_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * thumb = lv_obj_create(row);
        lv_obj_remove_style_all(thumb);
        lv_obj_set_size(thumb, BOARD_UI_PX(64), BOARD_UI_PX(28));
        lv_obj_set_style_radius(thumb, BOARD_UI_PX(6), 0);
        lv_obj_set_style_bg_color(thumb, lv_color_hex(0x0F1211), 0);
        lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        lv_obj_remove_flag(thumb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(thumb, LV_ALIGN_LEFT_MID, BOARD_UI_PX(12), 0);
        double g[EQ_GRAPHIC_BANDS];
        eq_preset_fitted_gains(i, g);
        eq_thumb_points(g, BOARD_UI_PX(64), BOARD_UI_PX(28), eq_sheet_pts[i]);
        eq_make_curve_line(thumb, eq_sheet_pts[i], 2, LV_OPA_COVER);
    }

    if (peq_scan_profiles(PEQ_PROFILES_DIR, &eq_sheet_paths, &eq_sheet_path_count) && eq_sheet_path_count > 0) {
        eq_sheet_section(list, TR("Saved"));
        for (int i = 0; i < eq_sheet_path_count; i++) {
            char stem[256], display[256];
            snprintf(stem, sizeof(stem), "%s", basename_of(eq_sheet_paths[i]));
            char * dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            eq_profile_display_name(stem, display, sizeof(display));
            bool sel = current < 0 && strcmp(stem, eq_current_profile_name) == 0;
            lv_obj_t * row = eq_sheet_row(list, display, sel);
            if (sel) selected_row = row;
            lv_obj_add_event_cb(row, eq_sheet_profile_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            lv_obj_t * icon = lv_label_create(row);
            lv_label_set_text(icon, LV_SYMBOL_FILE);
            lv_obj_add_style(icon, &style_theme_text_muted, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_UI_PX(36), 0);
        }
    }
    lv_obj_t * manage = eq_sheet_row(list, TR("Manage saved profiles"), false);
    lv_obj_add_event_cb(manage, eq_sheet_manage_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * chevron = lv_label_create(manage);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-12), 0);
    if (selected_row) lv_obj_scroll_to_view(selected_row, LV_ANIM_OFF);
}

static void eq_build_graphic(lv_obj_t * content, int32_t content_h, lv_obj_t * profiles) {
    /* Graphic | Parametric */
    lv_obj_t * seg = lv_obj_create(content);
    lv_obj_remove_style_all(seg);
    lv_obj_move_to_index(seg, 0);
    lv_obj_set_size(seg, lv_pct(100), BOARD_UI_PX(36));
    lv_obj_set_style_radius(seg, BOARD_UI_PX(18), 0);
    lv_obj_set_style_bg_color(seg, lv_color_hex(GUI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(seg, BOARD_UI_PX(3), 0);
    lv_obj_set_style_pad_column(seg, BOARD_UI_PX(3), 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    static const char * const modes[2] = { N_("Graphic"), N_("Parametric") };
    for (int i = 0; i < 2; i++) {
        lv_obj_t * b = lv_obj_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, BOARD_UI_PX(15), 0);
        lv_obj_set_style_bg_color(b, accent_lv_color(), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, eq_mode_btn_cb, LV_EVENT_CLICKED, i == 0 ? (void *) 1 : NULL);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, TR(modes[i]));
        lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_center(l);
        eq_mode_btn[i] = b;
    }

    int32_t box_h = content_h - BOARD_UI_PX(36) - BOARD_UI_PX(8) - BOARD_UI_PX(8);
    eq_graphic_box = lv_obj_create(content);
    lv_obj_remove_style_all(eq_graphic_box);
    lv_obj_move_to_index(eq_graphic_box, 1);
    lv_obj_set_size(eq_graphic_box, lv_pct(100), box_h);
    lv_obj_remove_flag(eq_graphic_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(eq_graphic_box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_graphic_box);
    eq_graphic_preamp_label = lv_label_create(eq_graphic_box);
    lv_obj_add_style(eq_graphic_preamp_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(eq_graphic_preamp_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_pos(eq_graphic_preamp_label, BOARD_UI_PX(4), 0);
    lv_obj_add_flag(eq_graphic_preamp_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(eq_graphic_preamp_label, BOARD_UI_PX(8));
    lv_obj_add_event_cb(eq_graphic_preamp_label, eq_preamp_line_cb, LV_EVENT_CLICKED, NULL);
    eq_graphic_set_preamp_mode((eq_preamp_mode_t) current_settings.eq_preamp_mode);

    int32_t card_y = BOARD_UI_PX(24), card_h = box_h - card_y, card_w = BOARD_UI_PX(336);
    lv_obj_t * card = lv_obj_create(eq_graphic_box);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 0, card_y);
    lv_obj_set_size(card, lv_pct(100), card_h);
    lv_obj_set_style_radius(card, BOARD_UI_PX(16), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t * small = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t label_h = lv_font_get_line_height(small);
    eq_slider_top = BOARD_UI_PX(16);
    eq_slider_h = card_h - eq_slider_top - label_h - BOARD_UI_PX(14);
    int32_t mid = eq_slider_top + eq_slider_h / 2;
    lv_obj_t * zero = lv_obj_create(card);
    lv_obj_remove_style_all(zero);
    lv_obj_set_size(zero, card_w - BOARD_UI_PX(44), 1);
    lv_obj_set_pos(zero, BOARD_UI_PX(8), mid);
    lv_obj_set_style_bg_color(zero, lv_color_hex(0x3A3F3C), 0);
    lv_obj_set_style_bg_opa(zero, LV_OPA_COVER, 0);
    static const char * const scale_text[3] = { "+12", "0", "-12" };
    int32_t scale_y[3] = { eq_slider_top, mid, eq_slider_top + eq_slider_h };
    for (int i = 0; i < 3; i++) {
        lv_obj_t * l = lv_label_create(card);
        lv_label_set_text(l, scale_text[i]);
        lv_obj_set_style_text_font(l, small, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(GUI_COLOR_TEXT_FAINT), 0);
        lv_obj_set_pos(l, card_w - BOARD_UI_PX(34), scale_y[i] - label_h / 2);
    }
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        int32_t x = BOARD_UI_PX(22) + i * BOARD_UI_PX(30);
        eq_slider_x[i] = x;
        lv_obj_t * sl = lv_slider_create(card);
        lv_obj_set_size(sl, BOARD_UI_PX(6), eq_slider_h);
        lv_obj_set_pos(sl, x - BOARD_UI_PX(3), eq_slider_top);
        lv_slider_set_range(sl, -EQ_GRAPHIC_RANGE, EQ_GRAPHIC_RANGE);
        lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);
        lv_obj_set_style_bg_color(sl, lv_color_hex(GUI_COLOR_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sl, BOARD_UI_PX(3), LV_PART_MAIN | LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, accent_lv_color(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(GUI_COLOR_TEXT), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, BOARD_UI_PX(6), LV_PART_KNOB);
        lv_obj_set_style_border_width(sl, BOARD_UI_PX(2), LV_PART_KNOB);
        lv_obj_set_style_border_color(sl, lv_color_hex(GUI_COLOR_SURFACE), LV_PART_KNOB);
        lv_obj_remove_flag(sl, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_set_ext_click_area(sl, BOARD_UI_PX(12));
        lv_obj_remove_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(sl, eq_graphic_slider_cb, LV_EVENT_ALL, NULL);
        eq_graphic_slider[i] = sl;
        lv_obj_t * l = lv_label_create(card);
        lv_label_set_text(l, eq_graphic_freq_label[i]);
        lv_obj_set_style_text_font(l, small, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(GUI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_width(l, BOARD_UI_PX(30));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x - BOARD_UI_PX(15), eq_slider_top + eq_slider_h + BOARD_UI_PX(8));
    }
    /* Under the knobs, over the rails: the knobs stay on top. */
    eq_graphic_curve = eq_make_curve_line(card, eq_graphic_curve_pts, 2, LV_OPA_50);
    lv_obj_move_to_index(eq_graphic_curve, 1);

    /* Preset button: curve thumbnail, then the name. */
    lv_obj_remove_event_cb(profiles, eq_load_profile_btn_cb);
    lv_obj_add_event_cb(profiles, eq_preset_sheet_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * inner = lv_obj_get_child(profiles, 0);
    if (inner) {
        lv_obj_t * box = lv_obj_create(inner);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, BOARD_UI_PX(26), BOARD_UI_PX(16));
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_margin_right(box, BOARD_UI_PX(5), 0);
        lv_obj_move_to_index(box, 0);
        eq_preset_btn_curve = eq_make_curve_line(box, eq_preset_btn_pts, 2, LV_OPA_COVER);
        lv_obj_set_width(eq_profile_button_label, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(eq_profile_button_label, BOARD_UI_PX(72), 0); /* curve + name + chevron fit the pill */
        lv_obj_t * chevron = lv_obj_get_child(inner, lv_obj_get_child_count(inner) - 1);
        if (chevron) lv_obj_set_style_pad_left(chevron, BOARD_UI_PX(3), 0);
    }
}

static void eq_apply_modern_layout(lv_obj_t * title, lv_obj_t * content, lv_obj_t * bandnav, lv_obj_t * flat,
                                   lv_obj_t * profiles, lv_obj_t * save) {
    lv_label_set_text(title, TR("Equalizer"));
    /* The switch alone says on/off. */
    lv_obj_add_flag(eq_bypass_state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_hor(content, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_gap(content, BOARD_UI_PX(8), 0);
    lv_obj_set_style_pad_bottom(content, BOARD_UI_PX(8), 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);

    /* Band chips 1..N and Curve, scrolling sideways. */
    lv_obj_add_flag(bandnav, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * chips = lv_obj_create(content);
    lv_obj_remove_style_all(chips);
    lv_obj_move_to_index(chips, 0);
    lv_obj_set_size(chips, lv_pct(100), BOARD_UI_PX(36));
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, BOARD_UI_PX(6), 0);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(chips);
    for (int i = 0; i <= PEQ_NUM_BANDS; i++) {
        lv_obj_t * chip = lv_obj_create(chips);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, i < PEQ_NUM_BANDS ? BOARD_UI_PX(38) : LV_SIZE_CONTENT, BOARD_UI_PX(34));
        lv_obj_set_style_pad_hor(chip, BOARD_UI_PX(12), 0);
        lv_obj_set_style_radius(chip, BOARD_UI_PX(17), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(chip, eq_band_chip_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * l = lv_label_create(chip);
        if (i < PEQ_NUM_BANDS) lv_label_set_text_fmt(l, "%d", i + 1);
        else lv_label_set_text(l, TR("Curve"));
        lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_center(l);
        eq_band_chips[i] = chip;
    }

    /* "Peaking · On  >" card: opens the band's options. */
    lv_obj_add_flag(eq_band_options_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_move_to_index(row, 1);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(44));
    lv_obj_set_style_radius(row, BOARD_UI_PX(16), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, eq_band_options_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * caption = lv_label_create(row);
    lv_label_set_text(caption, TR("Band"));
    lv_obj_add_style(caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(caption, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, BOARD_UI_PX(14), 0);
    eq_band_summary_label = lv_label_create(row);
    lv_obj_set_style_text_font(eq_band_summary_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_color(eq_band_summary_label, lv_color_hex(GUI_COLOR_TEXT), 0);
    lv_obj_align(eq_band_summary_label, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-36), 0);
    lv_obj_t * chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-14), 0);
    eq_band_options_row = row; /* hidden with the band widgets on Curve */

    eq_style_card(eq_freq_card, eq_freq_slider, eq_freq_value_label);
    eq_style_card(eq_gain_card, eq_gain_slider, eq_gain_value_label);
    eq_style_card(eq_q_card, eq_q_slider, eq_q_value_label);
    lv_obj_set_width(eq_graph_panel, lv_pct(100));
    lv_obj_set_style_bg_color(eq_graph_panel, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(eq_graph_panel, 0, 0);

    lv_obj_set_style_pad_ver(eq_footer, BOARD_UI_PX(10), 0);
    eq_style_footer_button(flat, false);
    eq_style_footer_button(profiles, true);
    lv_obj_set_flex_grow(profiles, 3); /* curve, preset name and chevron */
    lv_obj_t * profile_inner = lv_obj_get_child(profiles, 0);
    if (profile_inner) lv_obj_set_style_pad_hor(profile_inner, BOARD_UI_PX(12), 0);
    eq_style_footer_button(save, false);

    eq_param_widgets[0] = chips;
    eq_param_widgets[1] = eq_band_options_row;
    eq_param_widgets[2] = eq_freq_card;
    eq_param_widgets[3] = eq_gain_card;
    eq_param_widgets[4] = eq_q_card;
    eq_param_widgets[5] = eq_graph_panel;
    lv_obj_update_layout(content);
    eq_build_graphic(content, lv_obj_get_content_height(content), profiles);
    eq_set_mode(eq_graphic_mode);
}

static lv_obj_t * build_eq_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_t * title = build_screen_header(scr, TR("Parametric EQ"), generic_back_cb, NULL, NULL);
    eq_bypass_switch = lv_switch_create(scr);
    align_screen_header_action(eq_bypass_switch, 16);
    lv_obj_add_style(eq_bypass_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    eq_bypass_state_label = lv_label_create(scr);
    lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    lv_obj_add_style(eq_bypass_state_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(eq_bypass_state_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_bypass_state_label, BOARD_SCALE_PX(64));
    lv_label_set_long_mode(eq_bypass_state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(eq_bypass_state_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(eq_bypass_state_label, eq_bypass_switch, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    eq_title_label = title;
    reserve_title_width_before(title, eq_bypass_state_label);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t * content = lv_obj_create(scr);
    eq_main_content = content;
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t footer_h = 64;
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), screen_h - content_top - footer_h);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, BOARD_SCALE_PX(7), 0);
    lv_obj_t * bandnav = lv_obj_create(content);
    lv_obj_set_size(bandnav, lv_pct(96), BOARD_SCALE_PX(50));
    lv_obj_set_style_bg_opa(bandnav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(bandnav, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_border_width(bandnav, 0, 0);
    lv_obj_set_style_pad_hor(bandnav, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(bandnav, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(bandnav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bandnav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bandnav, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * prev = lv_label_create(bandnav);
    lv_label_set_text(prev, LV_SYMBOL_LEFT);
    lv_obj_add_style(prev, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(prev, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(prev, 14);
    lv_obj_add_event_cb(prev, eq_band_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    eq_band_number_label = lv_label_create(bandnav);
    lv_obj_add_style(eq_band_number_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(eq_band_number_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(eq_band_number_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_band_number_label, eq_band_options_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * next = lv_label_create(bandnav);
    lv_label_set_text(next, LV_SYMBOL_RIGHT);
    lv_obj_add_style(next, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(next, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(next, 14);
    lv_obj_add_event_cb(next, eq_band_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    eq_band_options_row = lv_label_create(content);
    lv_obj_t * options = eq_band_options_row;
    lv_label_set_text_fmt(options, "%s  " LV_SYMBOL_RIGHT, TR("Band options"));
    lv_obj_add_style(options, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(options, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_ver(options, BOARD_SCALE_PX(2), 0);
    lv_obj_add_flag(options, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(options, eq_band_options_open_cb, LV_EVENT_CLICKED, NULL);
    eq_freq_card = eq_make_slider_card(content, EQ_FIELD_FREQ, TR("Frequency"), &eq_freq_value_label, &eq_freq_slider, 0, EQ_FREQ_SLIDER_MAX, "20", "20K");
    eq_gain_card = eq_make_slider_card(content, EQ_FIELD_GAIN, TR("Gain"), &eq_gain_value_label, &eq_gain_slider, -120, 120, "-12", "+12");
    eq_q_card = eq_make_slider_card(content, EQ_FIELD_Q, "Q", &eq_q_value_label, &eq_q_slider, 1, 100, "0.1", "10");
    eq_graph_panel = lv_obj_create(content);
    lv_obj_set_width(eq_graph_panel, lv_pct(96));
    lv_obj_set_flex_grow(eq_graph_panel, 1);
    lv_obj_set_style_min_height(eq_graph_panel, BOARD_SCALE_PX(240), 0);
    lv_obj_add_style(eq_graph_panel, &style_theme_card_bg, 0);
    lv_obj_set_style_radius(eq_graph_panel, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_all(eq_graph_panel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(eq_graph_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(eq_graph_panel, BOARD_SCALE_PX(4), 0);
    lv_obj_remove_flag(eq_graph_panel, LV_OBJ_FLAG_SCROLLABLE);
    eq_graph_caption = lv_label_create(eq_graph_panel);
    lv_label_set_text(eq_graph_caption, TR("Combined response (dB)"));
    lv_obj_set_width(eq_graph_caption, lv_pct(100));
    lv_label_set_long_mode(eq_graph_caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(eq_graph_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(eq_graph_caption, &style_theme_text_primary, 0);
    lv_obj_t * graph_reference = lv_label_create(eq_graph_panel);
    lv_label_set_text(graph_reference, TR("48 kHz reference"));
    lv_obj_set_style_text_font(graph_reference, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(graph_reference, &style_theme_text_muted, 0);
    lv_obj_t * graph_plot_row = lv_obj_create(eq_graph_panel);
    lv_obj_set_width(graph_plot_row, lv_pct(100));
    lv_obj_set_flex_grow(graph_plot_row, 1);
    lv_obj_set_style_bg_opa(graph_plot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_plot_row, 0, 0);
    lv_obj_set_style_pad_all(graph_plot_row, 0, 0);
    lv_obj_set_style_pad_column(graph_plot_row, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(graph_plot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(graph_plot_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(graph_plot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * graph_y_axis = lv_obj_create(graph_plot_row);
    lv_obj_set_width(graph_y_axis, BOARD_SCALE_PX(60));
    lv_obj_set_height(graph_y_axis, lv_pct(100));
    lv_obj_set_style_bg_opa(graph_y_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_y_axis, 0, 0);
    lv_obj_set_style_pad_all(graph_y_axis, 0, 0);
    lv_obj_set_flex_flow(graph_y_axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(graph_y_axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    eq_graph_max_label = lv_label_create(graph_y_axis);
    eq_graph_zero_label = lv_label_create(graph_y_axis);
    eq_graph_min_label = lv_label_create(graph_y_axis);
    lv_obj_set_style_text_font(eq_graph_max_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_zero_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_min_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(eq_graph_max_label, &style_theme_text_muted, 0);
    lv_obj_add_style(eq_graph_zero_label, &style_theme_text_muted, 0);
    lv_obj_add_style(eq_graph_min_label, &style_theme_text_muted, 0);
    eq_graph_chart = lv_chart_create(graph_plot_row);
    lv_obj_set_height(eq_graph_chart, lv_pct(100));
    lv_obj_set_style_pad_all(eq_graph_chart, 0, 0);
    lv_obj_set_flex_grow(eq_graph_chart, 1);
    lv_chart_set_type(eq_graph_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(eq_graph_chart, 128);
    lv_chart_set_div_line_count(eq_graph_chart, 5, 0);
    lv_chart_set_range(eq_graph_chart, LV_CHART_AXIS_PRIMARY_Y, -1200, 1200);
    lv_chart_set_update_mode(eq_graph_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(eq_graph_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq_graph_chart, 0, 0);
    lv_obj_set_style_line_color(eq_graph_chart, lv_color_make(128, 128, 128), LV_PART_MAIN);
    lv_obj_set_style_line_opa(eq_graph_chart, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_line_color(eq_graph_chart, accent_lv_color(), LV_PART_ITEMS);
    lv_obj_set_style_line_width(eq_graph_chart, BOARD_SCALE_PX(3), LV_PART_ITEMS);
    lv_obj_set_style_size(eq_graph_chart, 0, 0, LV_PART_INDICATOR);
    eq_graph_series = lv_chart_add_series(eq_graph_chart, accent_lv_color(), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t * graph_x_axis = lv_obj_create(eq_graph_panel);
    lv_obj_set_width(graph_x_axis, lv_pct(100));
    lv_obj_set_height(graph_x_axis, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
    lv_obj_set_style_bg_opa(graph_x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_x_axis, 0, 0);
    lv_obj_set_style_pad_all(graph_x_axis, 0, 0);
    lv_obj_set_style_pad_left(graph_x_axis, BOARD_SCALE_PX(64), 0);
    lv_obj_remove_flag(graph_x_axis, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(graph_x_axis, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(graph_x_axis, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    /* Positions follow log10(f / 20) / 3, matching the sampled curve. */
    lv_obj_t * xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "20");
    lv_obj_align(xlab, LV_ALIGN_TOP_LEFT, 0, 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "100");
    lv_obj_align(xlab, LV_ALIGN_TOP_MID, lv_pct(-27), 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "1k");
    lv_obj_align(xlab, LV_ALIGN_TOP_MID, lv_pct(7), 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "20k Hz");
    lv_obj_align(xlab, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
    eq_footer = lv_obj_create(scr);
    lv_obj_set_size(eq_footer, lv_pct(100), footer_h);
    lv_obj_align(eq_footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(eq_footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq_footer, 0, 0);
    lv_obj_set_style_pad_hor(eq_footer, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_ver(eq_footer, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_column(eq_footer, BOARD_SCALE_PX(8), 0);
    lv_obj_set_flex_flow(eq_footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(eq_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * flat = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(flat, 1);
    lv_obj_set_height(flat, BOARD_SCALE_PX(66));
    lv_obj_add_style(flat, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(flat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(flat, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_radius(flat, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_shadow_width(flat, 0, 0);
    lv_obj_add_event_cb(flat, eq_flat_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * fl = lv_label_create(flat);
    lv_label_set_text(fl, TR("Flat"));
    lv_obj_set_style_text_font(fl, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(fl);
    lv_obj_t * profiles = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(profiles, 2);
    lv_obj_set_height(profiles, BOARD_SCALE_PX(66));
    lv_obj_add_style(profiles, &style_theme_card_bg, 0);
    lv_obj_add_style(profiles, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_bg_opa(profiles, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(profiles, BOARD_SCALE_PX(2), 0);
    lv_obj_set_style_radius(profiles, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_shadow_width(profiles, 0, 0);
    /* The theme's button padding left the inner row less than one text
     * line tall, clipping the profile name. */
    lv_obj_set_style_pad_all(profiles, 0, 0);
    lv_obj_add_event_cb(profiles, eq_load_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * profile_inner = lv_obj_create(profiles);
    lv_obj_set_size(profile_inner, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(profile_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(profile_inner, 0, 0);
    lv_obj_set_style_pad_all(profile_inner, 0, 0);
    lv_obj_set_flex_flow(profile_inner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(profile_inner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(profile_inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(profile_inner, LV_OBJ_FLAG_SCROLLABLE);
    eq_profile_button_label = lv_label_create(profile_inner);
    lv_obj_set_style_text_font(eq_profile_button_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(eq_profile_button_label, gui_theme_accent_style(), 0);
    lv_label_set_long_mode(eq_profile_button_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(eq_profile_button_label, lv_pct(72));
    lv_obj_set_height(eq_profile_button_label, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
    lv_obj_set_style_text_align(eq_profile_button_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t * profile_chevron = lv_label_create(profile_inner);
    lv_label_set_text(profile_chevron, LV_SYMBOL_DOWN);
    lv_obj_add_style(profile_chevron, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(profile_chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(profile_chevron, BOARD_SCALE_PX(6), 0);
    lv_obj_t * save = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, BOARD_SCALE_PX(66));
    lv_obj_add_style(save, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(save, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_radius(save, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, eq_save_profile_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(save, eq_save_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, TR("Save"));
    lv_obj_set_style_text_font(sl, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(sl);
    refresh_all_eq_widgets();
    lv_obj_add_event_cb(scr, eq_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(eq_bypass_switch, eq_bypass_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_freq_slider, eq_freq_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_gain_slider, eq_gain_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_q_slider, eq_q_slider_event_cb, LV_EVENT_ALL, NULL);
    finalize_screen_navigation(scr);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_freq_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_gain_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_q_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_freq_card);
    register_swipe_dead_zone(eq_gain_card);
    register_swipe_dead_zone(eq_q_card);
    eq_apply_modern_layout(title, content, bandnav, flat, profiles, save);
    return scr;
}

void gui_settings_init(void) {
    about_screen = build_about_screen();
    dev_options_screen = build_dev_options_screen();
    accent_color_screen = build_accent_color_screen();
    custom_font_screen = build_custom_font_screen();
    screen_timeout_screen = build_screen_timeout_screen();
    screen_dimming_screen = build_screen_dimming_screen();
    startup_volume_screen = build_startup_volume_screen();
    sleep_timer_screen = build_sleep_timer_screen();
    idle_shutdown_screen = build_idle_shutdown_screen();
    timezone_region_screen = build_timezone_region_screen();
    clock_set_time_screen = build_clock_set_time_screen();
    clock_screen = build_clock_screen();
    music_playback_screen = build_music_playback_screen();
    music_audio_screen = build_music_audio_screen();
    music_controls_screen = NULL; /* built on first open of Playback & Controls > Buttons & Remote */
    animation_speed_screen = build_animation_speed_screen();
    if (MT_HAS_FEATURE(I18N)) language_choice_screen = build_language_choice_screen();
    settings_display_screen = build_settings_display_screen();
    settings_power_screen = build_settings_power_screen();
    settings_system_screen = build_settings_system_screen();
    settings_tools_screen = build_settings_tools_screen();
    settings_screen = build_settings_screen();
    eq_screen = build_eq_screen();
    eq_band_options_screen = build_eq_band_options_screen();
    eq_profiles_screen = build_eq_profiles_screen();
    build_recovery_popup();
    build_eq_reset_popup();
    build_eq_profile_delete_popup();
    build_eq_save_choice_popup();
    build_factory_reset_popup();
    build_hostname_reboot_popup();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_settings_init() can rebuild them from a clean slate
 * without leaking the old objects. The
 * popup-and-backdrop pairs below are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so each needs its own explicit deletion. */
void gui_settings_teardown(void) {
    gui_popup_teardown(&recovery_popup);
    gui_popup_teardown(&eq_reset_popup);
    gui_popup_teardown(&eq_profile_delete_popup);
    gui_popup_teardown(&eq_save_choice_popup);
    gui_popup_teardown(&factory_reset_popup);
    gui_popup_teardown(&hostname_reboot_popup);

    if (about_screen) { lv_obj_delete(about_screen); about_screen = NULL; }
    about_update_row = NULL;
    if (dev_options_screen) { lv_obj_delete(dev_options_screen); dev_options_screen = NULL; }
    adb_switch = NULL; /* owned by the screen just deleted; sync runs off the USB poll, not this screen's lifetime */
    if (accent_color_screen) { lv_obj_delete(accent_color_screen); accent_color_screen = NULL; }
    accent_preview_swatch = NULL; /* accent_screen_sync() is a no-op until the screen is rebuilt */
    accent_sv_area = NULL;
    if (custom_font_screen) { lv_obj_delete(custom_font_screen); custom_font_screen = NULL; }
    if (screen_timeout_screen) { lv_obj_delete(screen_timeout_screen); screen_timeout_screen = NULL; }
    if (screen_dimming_screen) { lv_obj_delete(screen_dimming_screen); screen_dimming_screen = NULL; }
    if (startup_volume_screen) { lv_obj_delete(startup_volume_screen); startup_volume_screen = NULL; }
    if (sleep_timer_screen) { lv_obj_delete(sleep_timer_screen); sleep_timer_screen = NULL; }
    if (idle_shutdown_screen) { lv_obj_delete(idle_shutdown_screen); idle_shutdown_screen = NULL; }
    if (timezone_region_screen) { lv_obj_delete(timezone_region_screen); timezone_region_screen = NULL; }
    if (clock_screen) { lv_obj_delete(clock_screen); clock_screen = NULL; }
    if (clock_set_time_screen) { lv_obj_delete(clock_set_time_screen); clock_set_time_screen = NULL; }
    clock_hour_roller = clock_minute_roller = clock_ampm_roller = NULL;
    clock_set_time_row = NULL;
    clock_timezone_row = clock_timezone_value_label = NULL;
    /* Lazily built by open_timezone_city_screen() -- NULL until the user has
     * opened at least one region, same guard shape as every screen above. */
    if (timezone_city_screen) { lv_obj_delete(timezone_city_screen); timezone_city_screen = NULL; }
    if (settings_library_screen) { lv_obj_delete(settings_library_screen); settings_library_screen = NULL; }
    if (settings_sorting_screen) { lv_obj_delete(settings_sorting_screen); settings_sorting_screen = NULL; }
    memset(settings_sorting_options, 0, sizeof(settings_sorting_options));
    if (settings_sound_effects_screen) { lv_obj_delete(settings_sound_effects_screen); settings_sound_effects_screen = NULL; }
    if (settings_eq_profiles_menu_screen) { lv_obj_delete(settings_eq_profiles_menu_screen); settings_eq_profiles_menu_screen = NULL; }
    if (settings_appearance_screen) { lv_obj_delete(settings_appearance_screen); settings_appearance_screen = NULL; }
    if (settings_player_layout_screen) { lv_obj_delete(settings_player_layout_screen); settings_player_layout_screen = NULL; }
    if (settings_gestures_screen) { lv_obj_delete(settings_gestures_screen); settings_gestures_screen = NULL; }
    if (settings_charging_screen) { lv_obj_delete(settings_charging_screen); settings_charging_screen = NULL; }
    if (settings_maintenance_screen) { lv_obj_delete(settings_maintenance_screen); settings_maintenance_screen = NULL; }
    if (settings_system_maintenance_screen) { lv_obj_delete(settings_system_maintenance_screen); settings_system_maintenance_screen = NULL; }
    if (settings_tools_screen) { lv_obj_delete(settings_tools_screen); settings_tools_screen = NULL; }
    if (music_playback_screen) { lv_obj_delete(music_playback_screen); music_playback_screen = NULL; }
    if (music_audio_screen) { lv_obj_delete(music_audio_screen); music_audio_screen = NULL; }
    settings_eq_summary = settings_sleep_summary = NULL;
    if (music_controls_screen) { lv_obj_delete(music_controls_screen); music_controls_screen = NULL; }
    if (animation_speed_screen) { lv_obj_delete(animation_speed_screen); animation_speed_screen = NULL; }
    animation_speed_list = NULL;
    if (language_choice_screen) { lv_obj_delete(language_choice_screen); language_choice_screen = NULL; }
    language_choice_list = NULL;
    if (settings_display_screen) { lv_obj_delete(settings_display_screen); settings_display_screen = NULL; }
    if (settings_power_screen) { lv_obj_delete(settings_power_screen); settings_power_screen = NULL; }
    if (settings_system_screen) { lv_obj_delete(settings_system_screen); settings_system_screen = NULL; }
    if (settings_screen) { lv_obj_delete(settings_screen); settings_screen = NULL; }
    if (eq_screen) { lv_obj_delete(eq_screen); eq_screen = NULL; }
    memset(eq_band_chips, 0, sizeof(eq_band_chips));
    eq_band_summary_label = NULL;
    eq_sheet_close();
    eq_preamp_popup_close();
    eq_graphic_box = eq_graphic_curve = eq_graphic_preamp_label = eq_preset_btn_curve = NULL;
    memset(eq_graphic_slider, 0, sizeof(eq_graphic_slider));
    memset(eq_param_widgets, 0, sizeof(eq_param_widgets));
    memset(eq_mode_btn, 0, sizeof(eq_mode_btn));
    if (eq_band_options_screen) { lv_obj_delete(eq_band_options_screen); eq_band_options_screen = NULL; }
    eq_bypass_switch = eq_band_enabled_switch = NULL;
    eq_preamp_slider = eq_preamp_value_label = NULL;
    eq_freq_slider = eq_gain_slider = eq_q_slider = NULL;
    eq_type_dropdown = eq_freq_value_label = eq_gain_value_label = eq_q_value_label = NULL;
    eq_band_number_label = eq_profile_button_label = eq_main_content = eq_footer = eq_bypass_state_label = NULL;
    eq_graph_panel = eq_graph_chart = eq_graph_caption = NULL;
    eq_graph_series = NULL;
    eq_graph_min_label = eq_graph_zero_label = eq_graph_max_label = NULL;
    eq_band_options_row = eq_freq_card = eq_gain_card = eq_q_card = NULL;
    eq_graph_visible = false;
    eq_title_label = NULL;
    if (eq_profiles_screen) { lv_obj_delete(eq_profiles_screen); eq_profiles_screen = NULL; }
    eq_profiles_edit_btn = NULL;
    eq_profiles_title_label = NULL;
    eq_profiles_free_paths();
}


/* Externs for callbacks defined in gui.c */

/* Extern widget var in gui.c */




lv_obj_t * gui_settings_get_screen(void) { return settings_screen; }
lv_obj_t * gui_settings_get_music_screen(void) { return music_playback_screen; }
lv_obj_t * gui_settings_get_display_screen(void) { return settings_display_screen; }
lv_obj_t * gui_settings_get_power_screen(void) { return settings_power_screen; }
lv_obj_t * gui_settings_get_system_screen(void) { return settings_system_screen; }
lv_obj_t * gui_settings_get_about_screen(void) { return about_screen; }
lv_obj_t * gui_settings_get_accent_screen(void) { return accent_color_screen; }
lv_obj_t * gui_settings_get_custom_font_screen(void) { return custom_font_screen; }
lv_obj_t * gui_settings_get_eq_screen(void) { return eq_screen; }



void gui_settings_sync_gapless_toggle(void) {
    if (!settings_gapless_toggle_img) return;
    if (current_settings.gapless_enabled) lv_obj_add_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
}

void gui_settings_sync_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    /* Real lv_switch now (see PILL_ACCESSORY_TOGGLE in screen_builders.c) --
     * CHECKED state alone drives its visual, no sprite swap needed. */
    if (current_settings.crossfade_enabled) lv_obj_add_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
}

/* Same bidirectional-sync role as gui_settings_sync_crossfade_toggle()
 * above, for the Sleep Timer screen's own enable switch -- called from
 * gui_shell.c whenever the drawer icon arms/disarms the timer, or the
 * countdown expires on its own, so this screen never shows a stale state
 * the next time it's opened. Also shows/hides the duration slider card and
 * the "Show Time Remaining" button, matching sleep_timer_switch_event_cb's
 * own behavior when the switch is toggled from this screen directly. */
void gui_settings_sync_sleep_timer_toggle(void) {
    if (!sleep_timer_switch) return;
    bool active = quick_drawer_sleep_timer_is_active();
    if (active) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(sleep_timer_switch, LV_STATE_CHECKED);
    if (!sleep_timer_slider_card) return;
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
