#ifndef GUI_HOME_CLOCK_H
#define GUI_HOME_CLOCK_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

/* Clock-face home screen, on every board. */
lv_obj_t * gui_home_clock_build(void);
void gui_home_clock_set_track(const char * title, const char * artist);
void gui_home_clock_refresh_cover(void);
void gui_home_clock_set_playing(bool playing);
/* Pauses the once-a-second clock refresh while the screen is off; it catches
 * up at once when the screen comes back. */
void gui_home_clock_set_screen_on(bool on);

#endif
