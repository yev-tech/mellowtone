#ifndef GUI_NOW_PANEL_H
#define GUI_NOW_PANEL_H

/* Now Playing second panel: Up next / Lyrics / Info / More under a mini
 * header. */
typedef enum {
    GUI_NOW_PANEL_TAB_UP_NEXT,
    GUI_NOW_PANEL_TAB_LYRICS,
    GUI_NOW_PANEL_TAB_INFO,
    GUI_NOW_PANEL_TAB_MORE,
} gui_now_panel_tab_t;

void gui_now_panel_open(void);
void gui_now_panel_open_tab(gui_now_panel_tab_t tab);
void gui_now_panel_teardown(void);

#endif
