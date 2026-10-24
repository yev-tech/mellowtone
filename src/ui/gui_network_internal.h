#pragma once
/* Shared inside the network module: the Wireless hub and orchestration
 * (gui_network.c) and the files it is split into -- gui_wifi.c,
 * gui_bluetooth.c, gui_usb.c and the optional features' gui_airplay.c,
 * gui_dlna.c, gui_remote.c and gui_wifi_import.c. Other modules use
 * gui_network.h. A feature that is not built (mt_features.h) leaves its file
 * out of the build, and its entries below turn into inert inline stubs. */
#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mt_features.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "settings.h"
#include "wifi_control.h"

/* An optional feature's screen, or NULL when the feature is not built
 * (mt_features.h): the code behind each use then folds away with it. */
#define FEATURE_SCREEN(feature, screen) (MT_HAS_FEATURE(feature) ? (screen) : NULL)

/* Externs to gui.c state and functions */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_remove_stack_slot(int depth);
extern void start_library_auto_rescan(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void generic_back_cb(lv_event_t * e);
extern gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
extern void gui_busy_hide(gui_busy_handle_t handle);

/* ---- gui_network.c ---- */

/* The services started and stopped on the module's one service worker. */
typedef enum {
    NETWORK_SERVICE_IMPORT,
    NETWORK_SERVICE_REMOTE,
    NETWORK_SERVICE_DLNA,
    NETWORK_SERVICE_COUNT
} network_service_t;

bool network_service_enqueue(network_service_t service, bool start, bool force_stop);
bool network_service_is_busy(network_service_t service);

/* The shared Wi-Fi dependency guard for the Wi-Fi-based features. */
bool wifi_feature_guard(void);

void configure_network_row_line(lv_obj_t * label, int32_t width,
                                const lv_font_t * font);
void layout_network_row(lv_obj_t * row, int32_t x, lv_obj_t * line1, lv_obj_t * line2,
                        lv_obj_t * line3);
void set_label_if_changed(lv_obj_t * label, const char * text);
void format_rate(char * out, size_t size, unsigned int rate);

/* ---- gui_wifi.c ---- */

void wifi_settings_snapshot_invalidate(void);
void wifi_settings_snapshot_start(void);
bool wifi_cached_info_is_current(void);
const wifi_info_t * gui_wifi_cached_info(void);
bool gui_wifi_cached_info_connected(void);
void gui_wifi_set_snapshot_enabled(bool enabled);

void gui_wifi_init(void);
void gui_wifi_teardown_setup(void);
void gui_wifi_teardown_action_popup(void);
void gui_wifi_teardown_screens(void);
bool gui_wifi_has_background_work(void);
void gui_wifi_cancel_background_work(void);

/* ---- gui_bluetooth.c ---- */

lv_obj_t * gui_bluetooth_get_dac_screen(void);
void gui_bluetooth_update_dac_stream_label(void);

void gui_bluetooth_init(void);
void gui_bluetooth_teardown_action_popup(void);
void gui_bluetooth_teardown_dac_leave_popup(void);
void gui_bluetooth_teardown_screens(void);
bool gui_bluetooth_has_background_work(void);
void gui_bluetooth_cancel_background_work(void);

/* ---- gui_usb.c ---- */

void gui_usb_update_dac_stream_labels(void);

void gui_usb_init(void);
void gui_usb_init_lend(void);
void gui_usb_teardown_popups(void);
void gui_usb_teardown_screens(void);
bool gui_usb_has_background_work(void);
void gui_usb_cancel_background_work(void);

/* ---- gui_airplay.c (AIRPLAY) ---- */

#if MT_HAS_FEATURE(AIRPLAY)
void populate_airplay_screen(void);
void airplay_tile_cb(lv_event_t * e);
lv_obj_t * gui_airplay_get_overlay_screen(void);
void gui_airplay_init(void);
void gui_airplay_teardown(void);
#else
static inline void populate_airplay_screen(void) {}
static inline void airplay_tile_cb(lv_event_t * e) { (void) e; }
static inline lv_obj_t * gui_airplay_get_overlay_screen(void) { return NULL; }
static inline void gui_airplay_init(void) {}
static inline void gui_airplay_teardown(void) {}
#endif

/* ---- gui_dlna.c (DLNA_RENDERER) ---- */

#if MT_HAS_FEATURE(DLNA_RENDERER)
void populate_dlna_screen(void);
void dlna_tile_cb(lv_event_t * e);
void gui_dlna_init(void);
void gui_dlna_teardown(void);
#else
static inline void populate_dlna_screen(void) {}
static inline void dlna_tile_cb(lv_event_t * e) { (void) e; }
static inline void gui_dlna_init(void) {}
static inline void gui_dlna_teardown(void) {}
#endif

/* ---- gui_remote.c (REMOTE) ---- */

#if MT_HAS_FEATURE(REMOTE)
lv_obj_t * gui_remote_get_screen(void);
void remote_control_refresh_address(void);
void remote_control_poll_screen(void);
void remote_control_tile_cb(lv_event_t * e);
void gui_remote_open_screen(void);
void gui_remote_init(void);
void build_remote_control_new_pin_popup(void);
void gui_remote_teardown_widgets(void);
void gui_remote_teardown_screen(void);
#else
static inline lv_obj_t * gui_remote_get_screen(void) { return NULL; }
static inline void remote_control_refresh_address(void) {}
static inline void remote_control_poll_screen(void) {}
static inline void remote_control_tile_cb(lv_event_t * e) { (void) e; }
static inline void gui_remote_init(void) {}
static inline void build_remote_control_new_pin_popup(void) {}
static inline void gui_remote_teardown_widgets(void) {}
static inline void gui_remote_teardown_screen(void) {}
#endif

/* ---- gui_wifi_import.c (WIFI_IMPORT) ---- */

#if MT_HAS_FEATURE(WIFI_IMPORT)
void populate_import_wifi_screen(void);
void import_wifi_handle_wifi_disabled(void);
void import_wifi_tile_cb(lv_event_t * e);
void import_wifi_poll_stop(void);
bool import_wifi_stop_active(void);
void import_wifi_cancel_stop(void);
void gui_wifi_import_init(void);
void gui_wifi_import_teardown(void);
#else
static inline void populate_import_wifi_screen(void) {}
static inline void import_wifi_handle_wifi_disabled(void) {}
static inline void import_wifi_tile_cb(lv_event_t * e) { (void) e; }
static inline void import_wifi_poll_stop(void) {}
static inline bool import_wifi_stop_active(void) { return false; }
static inline void import_wifi_cancel_stop(void) {}
static inline void gui_wifi_import_init(void) {}
static inline void gui_wifi_import_teardown(void) {}
#endif
