#pragma once
#include <lvgl/lvgl.h>

/* Screen accessors */
lv_obj_t * gui_settings_get_screen(void);
lv_obj_t * gui_settings_get_music_screen(void);
lv_obj_t * gui_settings_get_display_screen(void);
lv_obj_t * gui_settings_get_power_screen(void);
lv_obj_t * gui_settings_get_system_screen(void);
lv_obj_t * gui_settings_get_about_screen(void);
lv_obj_t * gui_settings_get_accent_screen(void);
/* Refreshes the Accent Color screen after any accent change (gui_theme.c). */
void gui_settings_accent_changed(void);
lv_obj_t * gui_settings_get_custom_font_screen(void);
lv_obj_t * gui_settings_get_eq_screen(void);
/* Direct drawer/navigation entry points. */
void gui_settings_open_eq(void);
void gui_settings_open_sleep_timer(void);
void gui_settings_open_playback(void);
void gui_settings_open_library(void);

void gui_settings_init(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_settings_init() again from a clean slate. */
void gui_settings_teardown(void);
void gui_settings_refresh_font_geometry(void);
/* Resume Last Track, Play/Pause Button, Font Size and ReplayGain screens
 * (gui_settings_playback.c): built and deleted with the network module's
 * screens, at their place in its order (gui_network_init()/
 * gui_network_teardown()). */
void gui_settings_playback_init(void);
void gui_settings_playback_teardown(void);

void gui_settings_sync_crossfade_toggle(void);
void gui_settings_sync_gapless_toggle(void);
void gui_settings_sync_sleep_timer_toggle(void);
void gui_settings_sync_adb_toggle(void);
void gui_display_apply_rotation(bool upside_down);
