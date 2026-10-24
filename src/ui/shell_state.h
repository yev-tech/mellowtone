#ifndef SHELL_STATE_H
#define SHELL_STATE_H

#include <stdbool.h>

/* What gui_shell.c re-applies after a UI reload or theme refresh rebuilds
 * Home, the quick drawer or the status bar: the now-playing card as last
 * reported by the player, and the status-bar Bluetooth glyph. Plain data
 * with no LVGL, so the host tests can reach it. */

typedef struct {
    bool has_track;
    bool playing;
    char title[256];
    char artist[256];
    char album[256];
    char format[96];
} shell_now_playing_t;

/* NULL title: nothing loaded. The arguments may point into `np` itself --
 * re-applying the cache passes its own fields back in. */
void shell_now_playing_set_track(shell_now_playing_t * np, const char * title, const char * artist,
                                 const char * album);
void shell_now_playing_set_format(shell_now_playing_t * np, const char * text);

/* The status-bar Bluetooth glyph (an asset path relative to the theme), or
 * NULL when the icon is hidden (radio off). */
const char * shell_bt_status_glyph(bool powered, bool connected);

#endif
