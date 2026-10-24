#ifndef PLAYER_LAYOUTS_H
#define PLAYER_LAYOUTS_H

#include "lvgl.h"

/* The Player screen's XML layout. One layout ships: the LVGL XML file
 * player_layouts/cover_first.xml under the theme root, resolved through
 * asset_path_plain() so a theme override replaces it. It fills an empty
 * screen with a widget tree that follows the named-widget contract in
 * docs/PLAYER_LAYOUTS.md; gui_player.c then wires that tree up by widget
 * name, and falls back to its own hand-written C layout when the XML layout
 * cannot be loaded. */

#define PLAYER_LAYOUT_FILE "player_layouts/cover_first.xml"

/* Creates the XML layout under `parent` (an empty screen). Returns its root,
 * or NULL on failure (logged, `parent` left as it was). The XML engine
 * (LVGL's lv_xml_init(), which lv_init() no longer calls in 9.5) is started
 * by the first call, once per process. */
lv_obj_t * player_layouts_create(lv_obj_t * parent);

/* Releases the XML component registered for the layout. Call after the tree
 * built from it has been deleted: its styles live in that component. */
void player_layouts_release(void);

#endif /* PLAYER_LAYOUTS_H */
