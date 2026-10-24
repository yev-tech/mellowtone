#ifndef GUI_RADIO_H
#define GUI_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl/lvgl.h"

void gui_radio_open(void);
/* Deletes the Radio screen (rebuilt lazily when next opened). */
void gui_radio_teardown(void);
/* Station name from the last-loaded Radio.txt for a stream URL. */
bool gui_radio_station_name(const char * url, char * out, size_t out_size);

#endif
