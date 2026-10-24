/* Settings screens for playback and controls: Resume Last Track, Play/Pause
 * Button, Font Size and ReplayGain. */
#include "gui.h"
#include "i18n.h"
#include "gui_settings.h"
#include "gui_navigation.h"
#include "gui_notifications.h"
#include "gui_player.h"
#include "gui_shell.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include "settings.h"
#include <stdint.h>

static lv_obj_t * resume_mode_screen;
static lv_obj_t * resume_mode_list;
static lv_obj_t * play_pause_button_mode_screen;
static lv_obj_t * font_size_screen;
static lv_obj_t * replaygain_mode_screen;

/* ---- Resume Last Track selection screen (Settings > Music Settings >
 * Playback > Resume Last Track) -- same accent-colored-border single-select shape as Font Size
 * just below, but takes effect on the NEXT cold boot (there's nothing to
 * apply live -- this only ever matters at startup), so no reboot popup is
 * needed, just a toast confirming the choice. See gui_init()'s own resume
 * path for what each option actually does and its Subsonic-cache
 * guard. ---- */

typedef struct {
    int mode; /* matches player_settings_t.resume_mode */
    const char * label;
} resume_mode_option_t;

static const resume_mode_option_t resume_mode_options[] = {
    { 0, N_("Off") }, { 1, N_("Resume and Play") }, { 2, N_("Resume, but Paused") },
};
#define RESUME_MODE_OPTION_COUNT (sizeof(resume_mode_options) / sizeof(resume_mode_options[0]))

static void resume_mode_option_row_cb(lv_event_t * e);

static void populate_resume_mode_screen(void) {
    lv_obj_clean(resume_mode_list);
    for (size_t i = 0; i < RESUME_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.resume_mode == resume_mode_options[i].mode;
        add_pill_option_row(resume_mode_list, TR(resume_mode_options[i].label),
                            selected, resume_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void resume_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.resume_mode = resume_mode_options[index].mode;
    settings_save(&current_settings);
    populate_resume_mode_screen();
    show_info_toast(TR("Applies next time you launch the app"));
}

static lv_obj_t * build_resume_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Resume Last Track"), &title_label, &resume_mode_list);
}

static void open_resume_mode_screen(void) {
    populate_resume_mode_screen();
    nav_push(resume_mode_screen);
}

void resume_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_resume_mode_screen();
}

/* ---- Play/Pause button behavior selection screen (Settings > Music Settings
 * > Controls & Interface > Play/Pause Button) -- same accent-colored-border
 * single-select shape as Resume Last Track just above. Feature request: the
 * physical play/pause button can act as Previous Track instead, or combine
 * both via a short
 * double-click window -- see handle_physical_play_pause_press() near
 * update_timer_cb for the actual dispatch. Unlike Resume Last Track, this
 * takes effect on the very next physical button press, not just at next
 * boot. ---- */

typedef struct {
    int mode; /* matches player_settings_t.play_pause_button_mode */
    const char * label;
} play_pause_button_mode_option_t;

static const play_pause_button_mode_option_t play_pause_button_mode_options[] = {
    { 0, N_("Play/Pause") }, { 1, N_("Previous Track") }, { 2, N_("Play/Pause + Previous Track (Double-Click)") },
};
#define PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT (sizeof(play_pause_button_mode_options) / sizeof(play_pause_button_mode_options[0]))

static lv_obj_t * play_pause_button_mode_list;

static void play_pause_button_mode_option_row_cb(lv_event_t * e);

static void populate_play_pause_button_mode_screen(void) {
    lv_obj_clean(play_pause_button_mode_list);
    for (size_t i = 0; i < PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.play_pause_button_mode == play_pause_button_mode_options[i].mode;
        add_pill_option_row(play_pause_button_mode_list, TR(play_pause_button_mode_options[i].label),
                            selected, play_pause_button_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void play_pause_button_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.play_pause_button_mode = play_pause_button_mode_options[index].mode;
    settings_save(&current_settings);
    populate_play_pause_button_mode_screen();
    show_info_toast(TR("Applies immediately"));
}

static lv_obj_t * build_play_pause_button_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Play/Pause Button"), &title_label, &play_pause_button_mode_list);
}

static void open_play_pause_button_mode_screen(void) {
    populate_play_pause_button_mode_screen();
    nav_push(play_pause_button_mode_screen);
}

void play_pause_button_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_play_pause_button_mode_screen();
}

/* ---- Font size selection screen (Settings > Font Size).  A changed tier
 * is applied behind one rendered black frame, then navigation is reset
 * directly to Home without disturbing playback or background services. ---- */

typedef struct {
    int tier; /* matches player_settings_t.font_size_tier */
    const char * label;
} font_size_option_t;

static const font_size_option_t font_size_options[] = {
    { 0, N_("Small") }, { 1, N_("Medium") }, { 2, N_("Large") },
};
#define FONT_SIZE_OPTION_COUNT (sizeof(font_size_options) / sizeof(font_size_options[0]))

static lv_obj_t * font_size_list;

static void font_size_option_row_cb(lv_event_t * e);

static void populate_font_size_screen(void) {
    lv_obj_clean(font_size_list);
    for (size_t i = 0; i < FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.font_size_tier == font_size_options[i].tier;
        add_pill_option_row(font_size_list, TR(font_size_options[i].label),
                            selected, font_size_option_row_cb, (void *) (intptr_t) i);
    }
}

static void font_size_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int target = (int)(intptr_t)lv_obj_get_user_data(mask) - 1;
    lv_timer_delete(timer);
    if (!fallback_font_apply_size_tier(target)) {
        lv_obj_delete(mask);
        show_error_toast(TR("Could not apply font size"));
        return;
    }

    /* Stable font addresses mean existing object styles now see the new
     * descriptors.  Notify LVGL once, refresh only cached pixel geometry,
     * and discard every bitmap captured with the old metrics.  All of this
     * is one-shot work inside the mask; no timer or extra allocation remains
     * afterward. */
    gui_navigation_invalidate_font_snapshots();
    current_settings.font_size_tier = target;
    settings_save(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Every live screen, not only the Font Size nav stack. Music Settings
     * and the other unused Settings submenus keep the previous tier's
     * USER_3 label boxes unless they are in this walk. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    gui_player_refresh_font_geometry();
    compact_list_refresh_all();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    int target = font_size_options[index].tier;
    if (target == current_settings.font_size_tier) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *)(intptr_t)(target + 1));
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

    /* Leave enough time for the normal refresh timer to paint the mask;
     * synchronous lv_refr_now() here would re-enter rendering from an input
     * callback and was the source of earlier transition flicker. */
    lv_timer_create(font_size_apply_timer_cb, 35, mask);
}

static lv_obj_t * build_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Font Size"), &title_label, &font_size_list);
}

static void open_font_size_screen(void) {
    populate_font_size_screen();
    nav_push(font_size_screen);
}

void font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_font_size_screen();
}

/* ---- ReplayGain mode (Settings -> Playback) -- 3-way option (Off, Per Track,
 * Per Album). Applied immediately to take effect on the next started or queued
 * track without loudness jumps during current playback. ---- */

typedef struct {
    int mode; /* matches player_settings_t.replaygain_mode */
    const char * label;
} replaygain_mode_option_t;

static const replaygain_mode_option_t replaygain_mode_options[] = {
    { 0, N_("Off") }, { 1, N_("Per Track") }, { 2, N_("Per Album") },
};
#define REPLAYGAIN_MODE_OPTION_COUNT (sizeof(replaygain_mode_options) / sizeof(replaygain_mode_options[0]))

static lv_obj_t * replaygain_mode_list;

static void replaygain_mode_option_row_cb(lv_event_t * e);

static void populate_replaygain_mode_screen(void) {
    lv_obj_clean(replaygain_mode_list);
    for (size_t i = 0; i < REPLAYGAIN_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.replaygain_mode == replaygain_mode_options[i].mode;
        add_pill_option_row(replaygain_mode_list, TR(replaygain_mode_options[i].label),
                            selected, replaygain_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void replaygain_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.replaygain_mode = replaygain_mode_options[index].mode;
    settings_save(&current_settings);
    populate_replaygain_mode_screen();
    if (gui_player_has_active_track()) arm_next_track_for_audio(gui_player_get_playlist_index());
}

static lv_obj_t * build_replaygain_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("ReplayGain"), &title_label, &replaygain_mode_list);
}

static void open_replaygain_mode_screen(void) {
    populate_replaygain_mode_screen();
    nav_push(replaygain_mode_screen);
}

void replaygain_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_replaygain_mode_screen();
}

/* Built and deleted with the network module's screens, at their place in
 * its order (gui_network_init()/gui_network_teardown()). */
void gui_settings_playback_init(void) {
    font_size_screen = build_font_size_screen();
    replaygain_mode_screen = build_replaygain_mode_screen();
    resume_mode_screen = build_resume_mode_screen();
    play_pause_button_mode_screen = build_play_pause_button_mode_screen();
}

void gui_settings_playback_teardown(void) {
    if (font_size_screen) { lv_obj_delete(font_size_screen); font_size_screen = NULL; }
    if (replaygain_mode_screen) { lv_obj_delete(replaygain_mode_screen); replaygain_mode_screen = NULL; }
    if (resume_mode_screen) { lv_obj_delete(resume_mode_screen); resume_mode_screen = NULL; }
    if (play_pause_button_mode_screen) { lv_obj_delete(play_pause_button_mode_screen); play_pause_button_mode_screen = NULL; }
}
