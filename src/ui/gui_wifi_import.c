/* Import via Wi-Fi (optional feature WIFI_IMPORT): the screen showing the
 * address of the stock web upload server, and starting and stopping it.
 * Part of the network module (gui_network.c). */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_navigation.h"
#include "gui_library.h"
#include "screen_builders.h"
#include "import_web.h"
#include "wifi_control.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

static lv_obj_t * import_wifi_screen;

/* ---- Import via Wi-Fi screen -- this is just the on-device "here's the
 * address to open" front end; the actual file-manager webapp is the stock
 * firmware's own thttpd + CGI binaries, managed via import_web.h. ---- */

static lv_obj_t * import_wifi_status_label;
static lv_obj_t * import_wifi_url_label;
#if LV_USE_QRCODE
static lv_obj_t * import_wifi_qrcode;
#endif

void populate_import_wifi_screen(void) {
    wifi_settings_snapshot_start();
    if (!wifi_cached_info_is_current() || !gui_wifi_cached_info_connected() || gui_wifi_cached_info()->ip[0] == '\0') {
        lv_label_set_text(import_wifi_status_label, TR("Connect to Wi-Fi first"));
        lv_label_set_text(import_wifi_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:4399", gui_wifi_cached_info()->ip);
    lv_label_set_text(import_wifi_status_label, TR("Open this address on your phone or computer:"));
    lv_label_set_text(import_wifi_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(import_wifi_qrcode, url, strlen(url));
#endif
}

/* Background worker and polling state for stopping the Wi-Fi web import server
 * asynchronously without blocking the UI thread during process teardown. */
static pthread_t import_web_stop_thread;
static bool import_web_stop_active = false;
static bool import_web_stop_via_service_worker = false;
static atomic_bool import_web_stop_done_flag = false;
/* Import via Wi-Fi screen's own stack slot, recorded right before the busy
 * screen gets pushed on top of it -- spliced out via nav_remove_stack_slot()
 * once teardown finishes (same pattern text_entry_commit()/
 * poll_subsonic_download() already use), so backing out later lands on
 * whatever was open before Import via Wi-Fi, not on this now-defunct
 * "Closing Web Server..." screen or a stale extra copy of the screen
 * underneath it. */
static int import_web_stop_nav_slot = -1;

/* True from a successful open_import_wifi_screen() until teardown (either
 * the back button or gui_network_handle_wifi_disabled()) is INITIATED --
 * i.e. "the web server is running or a stop for it hasn't been kicked off
 * yet". Distinct from import_web_stop_active (which only covers the actual
 * background-thread window): this is what lets gui_network_handle_wifi_
 * disabled() tell "nothing to do" apart from "need to tear down", and
 * flips false the instant teardown starts so a second caller (back button
 * racing the wifi-disabled path, or vice versa) sees it as already handled
 * rather than starting a second worker. */
static bool import_web_active = false;

/* Set by gui_network_handle_wifi_disabled() right before it kicks off the
 * same async stop worker/busy-overlay import_wifi_back_cb() below uses --
 * consumed once by gui_network_poll_services() so the automatic rescan below
 * only ever fires after a real user-initiated exit, never merely because
 * Wi-Fi disappeared out from under the screen (which may not have
 * downloaded anything worth rescanning for at all). */
static bool import_web_stop_suppress_rescan = false;

static void * import_web_stop_thread_func(void * arg) {
    (void) arg;
    import_web_stop();
    atomic_store_explicit(&import_web_stop_done_flag, true, memory_order_release); /* written last -- gui_network_poll_services() only checks this flag */
    return NULL;
}

/* gui_network_poll_services()'s own part, once gui_network.c has polled the
 * service worker. */
void import_wifi_poll_stop(void) {
    if (!import_web_stop_active) return;
    if (import_web_stop_via_service_worker) {
        if (network_service_is_busy(NETWORK_SERVICE_IMPORT)) return;
    } else if (!atomic_load_explicit(&import_web_stop_done_flag, memory_order_acquire)) {
        return;
    }

    import_web_stop_active = false;
    if (!import_web_stop_via_service_worker) pthread_join(import_web_stop_thread, NULL);
    import_web_stop_via_service_worker = false;

    if (import_web_stop_nav_slot >= 0 && import_web_stop_nav_slot < gui_navigation_get_depth()) {
        nav_remove_stack_slot(import_web_stop_nav_slot);
    }
    gui_busy_hide(import_web_stop_token);
    import_web_stop_nav_slot = -1;

    bool suppress_rescan = import_web_stop_suppress_rescan;
    import_web_stop_suppress_rescan = false;
    if (suppress_rescan) return;

    /* Automatically rescan with the "Updating music database..." busy
     * screen, preserving recovered snapshots and libraries that failed to load. */
    if (gui_library_auto_rescan_enabled()) start_library_auto_rescan();
}

/* Shared by import_wifi_back_cb() (user-initiated exit) and import_wifi_
 * handle_wifi_disabled() (Wi-Fi-loss-initiated) below -- both need the exact
 * same async stop-worker/busy-overlay sequence, just with a different
 * suppress_rescan value. import_web_active is deliberately only cleared
 * AFTER pthread_create() actually succeeds, not optimistically beforehand:
 * if thread launch fails, import_web_stop() never runs and thttpd/
 * udp_server stay up, so leaving import_web_active true keeps that
 * reflected -- a retry (another back-button tap, or a later Wi-Fi-disabled
 * confirmation) still sees a stop as needed instead of wrongly treating
 * this as already handled and silently leaving the servers running
 * forever. */
static void import_web_stop_start(bool suppress_rescan) {
    import_web_stop_suppress_rescan = suppress_rescan;
    import_web_stop_nav_slot = gui_navigation_get_depth() - 1; /* this screen's own slot, before pushing the busy screen on top of it */
    import_web_stop_token = gui_busy_show(TR("Closing\nWeb Server..."), "");

    import_web_stop_active = true;
    import_web_stop_via_service_worker = false;
    if (network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
        if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, false, true)) {
            import_web_stop_active = false;
            import_web_stop_suppress_rescan = false;
            gui_busy_hide(import_web_stop_token);
            show_error_toast(TR("Thread launch failed"));
            return;
        }
        import_web_stop_via_service_worker = true;
        import_web_active = false;
        return;
    }
    atomic_store_explicit(&import_web_stop_done_flag, false, memory_order_relaxed);
    if (pthread_create(&import_web_stop_thread, NULL, import_web_stop_thread_func, NULL) != 0) {
        import_web_stop_active = false;
        import_web_stop_suppress_rescan = false;
        gui_busy_hide(import_web_stop_token);
        show_error_toast(TR("Thread launch failed"));
        return; /* import_web_active stays true -- nothing was actually stopped */
    }
    import_web_active = false; /* only now that the stop is genuinely in flight */
}

static void import_wifi_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    import_web_stop_start(false); /* a real user exit always wants the rescan prompt */
}

/* Wi-Fi-disabled counterpart to import_wifi_back_cb() above -- triggered by
 * gui_network_handle_wifi_disabled() instead of the user's own back-button
 * tap, and skips the rescan popup on completion since nothing was actually
 * imported this session. import_web_active being true guarantees import_
 * wifi_screen is the currently active screen: it skips finalize_screen_
 * navigation() (no swipe-to-back/swipe-up-home), so the explicit back
 * button -- which only runs import_web_stop_start() once it actually
 * launches the worker -- is the only way to leave it. Guarding on
 * import_web_stop_active additionally means a teardown already in flight
 * (from either caller) is left alone rather than racing a second worker
 * against it. */
void import_wifi_handle_wifi_disabled(void) {
    if (!import_web_active || import_web_stop_active) return; /* not running, or already tearing down */
    import_web_stop_start(true);
}

static lv_obj_t * build_import_wifi_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Import via Wi-Fi"), import_wifi_back_cb, NULL, NULL);

    import_wifi_status_label = lv_label_create(scr);
    lv_obj_set_width(import_wifi_status_label, lv_pct(90));
    lv_label_set_long_mode(import_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(import_wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(import_wifi_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(import_wifi_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(import_wifi_status_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(30));

#if LV_USE_QRCODE
    import_wifi_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(import_wifi_qrcode, BOARD_SCALE_PX(220));
    lv_qrcode_set_dark_color(import_wifi_qrcode, lv_color_black());
    lv_qrcode_set_light_color(import_wifi_qrcode, lv_color_white());
    lv_obj_set_style_border_width(import_wifi_qrcode, 4, 0);
    lv_obj_set_style_border_color(import_wifi_qrcode, lv_color_white(), 0);
    lv_obj_align(import_wifi_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(110));
#endif

    import_wifi_url_label = lv_label_create(scr);
    lv_obj_add_style(import_wifi_url_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(import_wifi_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_align(import_wifi_url_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(350));

    /* Deliberately skips finalize_screen_navigation()'s swipe-to-back --
     * an accidental swipe here would leave thttpd/udp_server running with
     * nothing to tear them down, same reasoning as text_entry_screen
     * skipping it for its keyboard. The back button above is the only way
     * out, and always runs import_web_stop(). */
    return scr;
}

static void open_import_wifi_screen(void) {
    if (!wifi_feature_guard()) return;
    if (import_web_active || import_web_stop_active || network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
        show_info_toast(TR("Web Server is busy"));
        return;
    }
    wifi_settings_snapshot_invalidate();
    gui_wifi_set_snapshot_enabled(true);
    if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, true, false)) {
        show_error_toast(TR("Thread launch failed"));
        return;
    }
    populate_import_wifi_screen();
    nav_push(import_wifi_screen);
    import_web_active = true;
}

void import_wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_import_wifi_screen();
}

void gui_wifi_import_init(void) {
    import_wifi_screen = build_import_wifi_screen();
}

void gui_wifi_import_teardown(void) {
    if (import_wifi_screen) { lv_obj_delete(import_wifi_screen); import_wifi_screen = NULL; }
}

bool import_wifi_stop_active(void) { return import_web_stop_active; }

void import_wifi_cancel_stop(void) {
    if (import_web_stop_active) {
        if (!import_web_stop_via_service_worker) pthread_join(import_web_stop_thread, NULL);
        import_web_stop_active = false;
        import_web_stop_via_service_worker = false;
        gui_busy_hide(import_web_stop_token);
    }
}

lv_obj_t * gui_network_get_import_wifi_screen(void) { return import_wifi_screen; }
