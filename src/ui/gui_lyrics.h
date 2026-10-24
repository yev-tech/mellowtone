#ifndef GUI_LYRICS_H
#define GUI_LYRICS_H
#include <stdbool.h>
#include "lvgl/lvgl.h"

void gui_lyrics_init(void);
/* Deletes the screens this module owns so gui_reload.c's in-process UI
 * reload can call gui_lyrics_init() again from a clean slate. */
void gui_lyrics_teardown(void);

void gui_lyrics_poll_load(void);
void gui_lyrics_load_track(int index, const char * path);
/* Precomputes or updates line geometry without changing list visibility,
 * parenting, scroll position, or row-pool state. */
void gui_lyrics_prepare_layout(void);

/* Borrows parent for content only; no navigation or player chrome changes.
 * Caller must hide before deleting parent (including player teardown). */
void gui_lyrics_show_embedded(lv_obj_t * parent, int32_t top, int32_t height);
void gui_lyrics_prepare_embedded(lv_obj_t * parent, int32_t top, int32_t height);
void gui_lyrics_hide_embedded(void);
/* True while the shared lyrics list is embedded directly in `parent`, so a
 * caller only hides it while it still owns it (the player and the Now panel
 * both embed it). */
bool gui_lyrics_embedded_in(const lv_obj_t * parent);
void lyrics_font_size_settings_row_cb(lv_event_t * e);

bool gui_lyrics_has_background_work(void);
void gui_lyrics_cancel_background_work(void);
void gui_lyrics_refresh_layout(void);
bool gui_lyrics_has_lines(void);

#endif
