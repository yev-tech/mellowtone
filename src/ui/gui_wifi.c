/* Wi-Fi settings: the Wi-Fi screen and its list, the saved-network action
 * popup, Wi-Fi Info, manual SSID entry, DNS settings, the setup wizard's
 * network list, and the scan/connect/disconnect/forget workers and their
 * polls. Part of the network module (gui_network.c). */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "gui_setup.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_text_input.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* Screens owned by this module */
static lv_obj_t * wifi_screen;
static lv_obj_t * wifi_info_screen;
static lv_obj_t * wifi_dns_screen;

static gui_busy_handle_t wifi_connect_saved_token = 0;
static gui_busy_handle_t wifi_connect_token = 0;

/* ---- Wi-Fi settings screen ----
 *
 * Enable/disable toggle at top; everything else (info/manual entry/DNS,
 * memorized networks, available networks) only appears once Wi-Fi is on,
 * rebuilt into the one dynamic wifi_list container by populate_wifi_screen()
 * -- reachable both from the Wireless submenu's Wi-Fi tile and from a
 * long-press on the quick-access drawer's wifi icon (see
 * quick_drawer_wifi_long_press_cb near build_quick_drawer). Scan/connect/
 * disconnect/forget all share the same background-thread-plus-polled-flag
 * shape as the Subsonic connect/download machinery above, and reuse its
 * subsonic_downloading_screen/label as the shared "please wait" interstitial
 * rather than building a second one. */

#define WIFI_MAX_RESULTS 32
#define WIFI_MAX_SAVED 32
static wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
static int wifi_scan_result_count = 0;
static wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
static int wifi_saved_result_count = 0;
static lv_obj_t * wifi_list;
static lv_obj_t * wifi_rescan_btn;
static bool wifi_enable_pending_feedback = false;
static char wifi_connect_pending_ssid[WIFI_MAX_SSID_LEN];
static lv_obj_t * wifi_setup_parent;
static bool wifi_setup_requested;
static bool wifi_setup_scan_started;
static bool wifi_setup_scan_finished;
static bool wifi_setup_enable_failed;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    bool secured;
    bool connected;
    int saved_id;
} wifi_setup_row_t;

static void wifi_setup_render(void);
static void wifi_setup_start_scan_once(void);
static int wifi_setup_saved_id(const char * ssid);
static void wifi_password_entered_cb(const char * text, void * user_data);

/* Saved-network/configuration reads are deliberately kept separate from the
 * scan buffers.  The worker owns its staging snapshot; the UI copies it only
 * after joining the worker, so no LVGL code and no partially-written strings
 * can cross the thread boundary. */
typedef struct {
    wifi_saved_network_t saved[WIFI_MAX_SAVED];
    int saved_count;
    wifi_info_t info;
    bool info_connected;
    unsigned generation;
} wifi_settings_snapshot_t;

static pthread_t wifi_settings_snapshot_thread;
static bool wifi_settings_snapshot_active = false;
static atomic_bool wifi_settings_snapshot_done_flag = false;
static atomic_uint wifi_settings_snapshot_generation = 1;
static wifi_settings_snapshot_t wifi_settings_snapshot_worker;
static unsigned wifi_settings_snapshot_cached_generation = 0;
static bool wifi_settings_snapshot_cached = false;
static uint32_t wifi_settings_snapshot_tick;
static bool wifi_settings_snapshot_enabled = false;
static wifi_info_t wifi_cached_info;
static bool wifi_cached_info_connected = false;

static void populate_wifi_info_screen(void);

static bool start_wifi_scan(void);

static pthread_t wifi_connect_thread;
static bool wifi_connect_active = false;
static bool wifi_connect_saved_active;
static bool wifi_disconnect_active;
static bool wifi_scan_active;
static atomic_bool wifi_connect_done_flag = false;
static volatile wifi_connect_result_t wifi_connect_result = WIFI_CONNECT_ERROR;

/* A WPA passphrase is 8-63 characters, a raw PSK 64 hex digits. */
#define WIFI_PASSWORD_SIZE 65

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[WIFI_PASSWORD_SIZE];
} wifi_connect_request_t;

/* wifi_control_connect() waits for the supplicant's verdict itself
 * (CTRL-EVENT-CONNECTED, WRONG_KEY, not found, ...). */
static void * wifi_connect_thread_func(void * arg) {
    wifi_connect_request_t * req = (wifi_connect_request_t *) arg;
    wifi_connect_result = wifi_control_connect(req->ssid, req->password[0] != '\0' ? req->password : NULL);
    memset(req->password, 0, sizeof(req->password));
    free(req);
    atomic_store_explicit(&wifi_connect_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect only checks this flag */
    return NULL;
}

static void show_wifi_connect_result(wifi_connect_result_t result) {
    switch (result) {
    case WIFI_CONNECT_OK: break;
    case WIFI_CONNECT_WRONG_KEY: show_error_toast(TR("Wrong Wi-Fi password")); break;
    case WIFI_CONNECT_NOT_FOUND: show_error_toast(TR("Wi-Fi network not in range")); break;
    default: show_error_toast(TR("Couldn't connect to Wi-Fi network")); break;
    }
}

static void start_wifi_connect(const char * ssid, const char * password) {
    if (wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");

    atomic_store_explicit(&wifi_connect_done_flag, false, memory_order_relaxed);
    wifi_connect_active = true;

    wifi_connect_token = gui_busy_show(TR("Connecting to"), ssid);

        if (pthread_create(&wifi_connect_thread, NULL, wifi_connect_thread_func, req) != 0) {
        wifi_connect_active = false;
        free(req);
        gui_busy_hide(wifi_connect_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_connect(void) {
    if (!wifi_connect_active || !atomic_load_explicit(&wifi_connect_done_flag, memory_order_acquire)) return;

    wifi_connect_active = false;
    pthread_join(wifi_connect_thread, NULL);
    gui_busy_hide(wifi_connect_token);
    show_wifi_connect_result(wifi_connect_result);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

/* Reconnecting to an already-memorized network -- deliberately a separate
 * thread/flag pair from the plain connect above (not a shared "mode" on the
 * same one) rather than reusing wifi_connect_active's state, matching this
 * file's existing convention of one dedicated pair per background Wi-Fi
 * operation (scan/connect/disconnect/forget already each have their own).
 * See wifi_control_connect_saved()'s own doc comment for why this can't
 * just call start_wifi_connect(ssid, NULL) instead. */
static pthread_t wifi_connect_saved_thread;
static bool wifi_connect_saved_active = false;
static atomic_bool wifi_connect_saved_done_flag = false;
static volatile wifi_connect_result_t wifi_connect_saved_result = WIFI_CONNECT_ERROR;

typedef struct {
    int id;
} wifi_connect_saved_request_t;

static void * wifi_connect_saved_thread_func(void * arg) {
    wifi_connect_saved_request_t * req = (wifi_connect_saved_request_t *) arg;
    wifi_connect_saved_result = wifi_control_connect_saved(req->id);
    free(req);
    atomic_store_explicit(&wifi_connect_saved_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect_saved only checks this flag */
    return NULL;
}

static void start_wifi_connect_saved(int id, const char * ssid) {
    if (wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_connect_saved_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;

    atomic_store_explicit(&wifi_connect_saved_done_flag, false, memory_order_relaxed);
    wifi_connect_saved_active = true;

    wifi_connect_saved_token = gui_busy_show(TR("Connecting to"), ssid);

        if (pthread_create(&wifi_connect_saved_thread, NULL, wifi_connect_saved_thread_func, req) != 0) {
        wifi_connect_saved_active = false;
        free(req);
        gui_busy_hide(wifi_connect_saved_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_connect_saved(void) {
    if (!wifi_connect_saved_active || !atomic_load_explicit(&wifi_connect_saved_done_flag, memory_order_acquire)) return;

    wifi_connect_saved_active = false;
    pthread_join(wifi_connect_saved_thread, NULL);
    gui_busy_hide(wifi_connect_saved_token);
    show_wifi_connect_result(wifi_connect_saved_result);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

static pthread_t wifi_disconnect_thread;
static bool wifi_disconnect_active = false;
static atomic_bool wifi_disconnect_done_flag = false;

static void * wifi_disconnect_thread_func(void * arg) {
    (void) arg;
    wifi_control_disconnect();
    atomic_store_explicit(&wifi_disconnect_done_flag, true, memory_order_release); /* written last -- poll_wifi_disconnect only checks this flag */
    return NULL;
}

static void start_wifi_disconnect(void) {
    if (wifi_disconnect_active) return; /* a second tap would join the first on the UI thread */
    atomic_store_explicit(&wifi_disconnect_done_flag, false, memory_order_relaxed);
    wifi_disconnect_active = true;
        if (pthread_create(&wifi_disconnect_thread, NULL, wifi_disconnect_thread_func, NULL) != 0) {
        wifi_disconnect_active = false;
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_disconnect(void) {
    if (!wifi_disconnect_active || !atomic_load_explicit(&wifi_disconnect_done_flag, memory_order_acquire)) return;
    wifi_disconnect_active = false;
    pthread_join(wifi_disconnect_thread, NULL);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the disconnected state */
}

static void wifi_password_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text && text[0] && !wifi_control_password_is_valid(text)) {
        show_error_toast(TR("Wi-Fi password must be 8 to 63 characters or 64 hex digits"));
        return;
    }
    start_wifi_connect(wifi_connect_pending_ssid, text);
}

/* Each Available Networks row owns a copy of its network, so a tap acts on
 * the network the row shows even after a newer scan replaced the list. */
static void wifi_row_delete_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

static void wifi_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const wifi_network_t * net = (const wifi_network_t *) lv_event_get_user_data(e);
    if (!net) return;
    if (net->is_current) {
        start_wifi_disconnect();
        return;
    }

    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", net->ssid);
    if (net->secured) {
        show_text_entry(TR("Wi-Fi Password"), NULL, true, false, WIFI_PASSWORD_SIZE, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(net->ssid, NULL);
    }
}

static void wifi_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* An explicit rescan is a new discovery snapshot, not an append to the
     * previous one. Clear stale access points immediately so the screen
     * cannot imply that an old result is still currently visible while the
     * radio is collecting its replacement set. Saved networks remain in
     * their own section because they are configuration, not scan results. */
    wifi_scan_result_count = 0;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_screen(wifi_control_is_enabled());
    start_wifi_scan();
}

static void * wifi_settings_snapshot_thread_func(void * arg) {
    unsigned generation = (unsigned) (uintptr_t) arg;
    wifi_settings_snapshot_t * snapshot = &wifi_settings_snapshot_worker;

    snapshot->generation = generation;
    snapshot->saved_count = wifi_control_list_saved(snapshot->saved, WIFI_MAX_SAVED);
    snapshot->info_connected = wifi_control_get_info(&snapshot->info);
    atomic_store_explicit(&wifi_settings_snapshot_done_flag, true, memory_order_release);
    return NULL;
}

void wifi_settings_snapshot_invalidate(void) {
    atomic_fetch_add_explicit(&wifi_settings_snapshot_generation, 1, memory_order_acq_rel);
    wifi_settings_snapshot_cached = false;
    wifi_settings_snapshot_cached_generation = 0;
    wifi_cached_info_connected = false;
    wifi_saved_result_count = 0;
}

void wifi_settings_snapshot_start(void) {
    if (wifi_settings_snapshot_active || wifi_enable_pending_feedback || !wifi_settings_snapshot_enabled ||
        !gui_shell_wifi_effective_enabled()) return;

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_cached && wifi_settings_snapshot_cached_generation == generation &&
        lv_tick_elaps(wifi_settings_snapshot_tick) < 5000) return;

    atomic_store_explicit(&wifi_settings_snapshot_done_flag, false, memory_order_relaxed);
    wifi_settings_snapshot_active = true;
    if (pthread_create(&wifi_settings_snapshot_thread, NULL, wifi_settings_snapshot_thread_func,
                       (void *) (uintptr_t) generation) != 0) {
        wifi_settings_snapshot_active = false;
    }
}

/* Returns true when a completed snapshot was published.  Rendering is kept
 * in the UI poller, never in the worker, and a generation mismatch discards
 * results collected across an enable/disable transition. */
static bool poll_wifi_settings_snapshot(void) {
    if (!wifi_settings_snapshot_active ||
        !atomic_load_explicit(&wifi_settings_snapshot_done_flag, memory_order_acquire)) return false;

    wifi_settings_snapshot_active = false;
    pthread_join(wifi_settings_snapshot_thread, NULL);

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_worker.generation != generation) {
        wifi_settings_snapshot_start();
        return false;
    }

    wifi_saved_result_count = wifi_settings_snapshot_worker.saved_count;
    memcpy(wifi_saved_results, wifi_settings_snapshot_worker.saved,
           sizeof(wifi_saved_results[0]) * (size_t) wifi_saved_result_count);
    wifi_cached_info = wifi_settings_snapshot_worker.info;
    wifi_cached_info_connected = wifi_settings_snapshot_worker.info_connected;
    wifi_settings_snapshot_cached_generation = generation;
    wifi_settings_snapshot_cached = true;
    wifi_settings_snapshot_tick = lv_tick_get();
    return true;
}

/* Connected network name from the last background status poll (no
 * subprocess), for summaries such as the Settings root. */
bool gui_network_cached_wifi_ssid(char * out, size_t size) {
    if (!out || size == 0) return false;
    out[0] = '\0';
    if (!wifi_cached_info_is_current() || !wifi_cached_info_connected || !wifi_cached_info.ssid[0]) return false;
    snprintf(out, size, "%s", wifi_cached_info.ssid);
    return true;
}

bool wifi_cached_info_is_current(void) {
    return wifi_settings_snapshot_cached &&
           wifi_settings_snapshot_cached_generation ==
               atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
}

static void set_wifi_rescan_active(bool active) {
    if (!wifi_rescan_btn) return;
    lv_label_set_text(wifi_rescan_btn, active ? TR("Scanning...") : TR("Rescan"));
    lv_obj_set_style_opa(wifi_rescan_btn, active ? LV_OPA_60 : LV_OPA_COVER, 0);
    align_screen_header_action(wifi_rescan_btn, 20);
    if (active) {
        lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(wifi_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(wifi_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void wifi_setup_row_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) free(lv_event_get_user_data(e));
}

static void wifi_setup_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wifi_setup_row_t * row = lv_event_get_user_data(e);
    if (!row || row->connected) return;
    if (wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", row->ssid);
    int saved_id = wifi_setup_saved_id(row->ssid);
    if (saved_id < 0) saved_id = row->saved_id;
    if (saved_id >= 0) {
        start_wifi_connect_saved(saved_id, row->ssid);
    } else if (row->secured) {
        show_text_entry(TR("Wi-Fi Password"), NULL, true, false, WIFI_PASSWORD_SIZE, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(row->ssid, NULL);
    }
}

static int wifi_setup_saved_id(const char * ssid) {
    if (!wifi_cached_info_is_current()) return -1;
    for (int i = 0; i < wifi_saved_result_count; ++i)
        if (strcmp(wifi_saved_results[i].ssid, ssid) == 0) return wifi_saved_results[i].id;
    return -1;
}

static void wifi_setup_add_row(const wifi_network_t * net) {
    lv_obj_t * parent = wifi_setup_parent;
    if (!parent) return;
    lv_obj_t * row_obj = lv_obj_create(parent);
    lv_obj_set_width(row_obj, LV_PCT(100));
    lv_obj_set_height(row_obj, BOARD_SCALE_PX(76));
    lv_obj_set_style_radius(row_obj, BOARD_SCALE_PX(16), 0);
    lv_obj_add_style(row_obj, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(row_obj, 0, 0);
    lv_obj_set_style_pad_all(row_obj, 0, 0);
    lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);

    wifi_setup_row_t * context = calloc(1, sizeof(*context));
    if (!context) { lv_obj_delete(row_obj); return; }
    size_t ssid_len = strnlen(net->ssid, sizeof(context->ssid) - 1);
    memcpy(context->ssid, net->ssid, ssid_len);
    context->ssid[ssid_len] = '\0';
    context->secured = net->secured;
    context->connected = net->is_current ||
        (wifi_cached_info_is_current() && wifi_cached_info_connected &&
         strcmp(net->ssid, wifi_cached_info.ssid) == 0);
    context->saved_id = wifi_setup_saved_id(net->ssid);
    lv_obj_add_event_cb(row_obj, wifi_setup_row_delete_cb, LV_EVENT_DELETE, context);
    lv_obj_add_event_cb(row_obj, wifi_setup_row_click_cb, LV_EVENT_CLICKED, context);

    lv_obj_t * title = lv_label_create(row_obj);
    lv_label_set_text(title, net->ssid);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(10), BOARD_SCALE_PX(8));

    lv_obj_t * status = lv_label_create(row_obj);
    const char * status_text = context->connected
        ? (net->secured ? TR("Secured · Connected") : TR("Open · Connected"))
        : context->saved_id >= 0 ? TR("Saved network")
        : net->secured ? TR("Secured network") : TR("Open network");
    lv_label_set_text(status, status_text);
    lv_obj_add_style(status, context->connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(status, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, BOARD_SCALE_PX(10), -BOARD_SCALE_PX(8));

    lv_obj_t * signal = lv_image_create(row_obj);
    char icon_path[40];
    int signal_level = net->signal_level < 0 ? 0 : net->signal_level > 3 ? 3 : net->signal_level;
    snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", signal_level);
    lv_image_set_src(signal, asset_path(icon_path));
    lv_obj_set_size(signal, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
    lv_image_set_inner_align(signal, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(10), 0);
    lv_obj_set_width(title, LV_PCT(72));
    lv_obj_set_width(status, LV_PCT(72));
}

static void wifi_setup_render(void) {
    lv_obj_t * parent = wifi_setup_parent;
    if (!parent) return;
    lv_obj_clean(parent);
    if (wifi_setup_enable_failed) {
        build_list_message(parent, TR("Wi-Fi could not be enabled"), TR("Check the wireless switch and try again."));
        return;
    }
    if (!gui_shell_wifi_enabled_and_settled()) {
        build_list_message(parent, TR("Turning Wi-Fi on"), TR("Nearby networks will appear when Wi-Fi is ready."));
        return;
    }
    if (wifi_scan_active || (!wifi_setup_scan_finished && wifi_setup_scan_started)) {
        build_list_message(parent, TR("Searching for networks"), TR("This can take a few seconds."));
        return;
    }
    if (wifi_scan_result_count == 0) {
        build_list_message(parent, TR("No networks found"), TR("Try scanning again or add a hidden network."));
        return;
    }
    for (int i = 0; i < wifi_scan_result_count; ++i) wifi_setup_add_row(&wifi_scan_results[i]);
}

static void wifi_setup_start_scan_once(void) {
    if (!wifi_setup_requested || !gui_shell_wifi_enabled_and_settled()) return;
    wifi_settings_snapshot_enabled = true;
    wifi_settings_snapshot_start();
    if (wifi_setup_scan_started) return;
    if (wifi_scan_active) {
        wifi_setup_scan_started = true;
        wifi_setup_scan_finished = false;
        return;
    }
    wifi_setup_scan_finished = false;
    wifi_scan_result_count = 0;
    bool started = start_wifi_scan();
    wifi_setup_scan_started = true;
    wifi_setup_scan_finished = !started;
}

static void wifi_setup_parent_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE && lv_event_get_target(e) == wifi_setup_parent) {
        wifi_setup_parent = NULL;
    }
}

void gui_network_setup_wifi_prepare(void) {
    wifi_setup_requested = true;
    wifi_setup_enable_failed = !gui_shell_wifi_ensure_enabled();
    if (!wifi_setup_enable_failed) wifi_setup_start_scan_once();
    wifi_setup_render();
}

void gui_network_setup_wifi_mount(lv_obj_t * parent) {
    if (wifi_setup_parent && wifi_setup_parent != parent) wifi_setup_parent = NULL;
    wifi_setup_parent = parent;
    if (parent) lv_obj_add_event_cb(parent, wifi_setup_parent_delete_cb, LV_EVENT_DELETE, NULL);
    wifi_setup_render();
}

void gui_network_setup_wifi_rescan(void) {
    if (!wifi_setup_requested) gui_network_setup_wifi_prepare();
    if (wifi_scan_active) {
        wifi_setup_scan_started = true;
        wifi_setup_scan_finished = false;
        wifi_setup_render();
        return;
    }
    if (!gui_shell_wifi_enabled_and_settled()) {
        wifi_setup_enable_failed = !gui_shell_wifi_ensure_enabled();
        wifi_setup_render();
        return;
    }
    wifi_setup_enable_failed = false;
    wifi_setup_scan_started = false;
    wifi_setup_scan_finished = false;
    wifi_scan_result_count = 0;
    wifi_setup_start_scan_once();
    wifi_setup_render();
}

static void wifi_setup_hidden_ssid_cb(const char * text, void * user_data) {
    (void) user_data;
    if (wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    size_t len = text ? strlen(text) : 0;
    if (len == 0 || len >= WIFI_MAX_SSID_LEN) {
        show_error_toast(TR("SSID must be 1 to 32 bytes"));
        return;
    }
    memcpy(wifi_connect_pending_ssid, text, len + 1);
    show_text_entry(TR("Wi-Fi Password"), NULL, true, false, WIFI_PASSWORD_SIZE, wifi_password_entered_cb, NULL);
}

void gui_network_setup_wifi_add_hidden(void) {
    if (!gui_shell_wifi_enabled_and_settled()) {
        show_info_toast(TR("Wi-Fi is still starting"));
        return;
    }
    if (wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    show_text_entry(TR("Network Name (SSID)"), NULL, false, false, WIFI_MAX_SSID_LEN, wifi_setup_hidden_ssid_cb, NULL);
}

bool gui_network_setup_wifi_connected(void) {
    return gui_shell_wifi_enabled_and_settled() && wifi_cached_info_is_current() &&
           wifi_cached_info_connected && lv_tick_elaps(wifi_settings_snapshot_tick) < 10000;
}

static pthread_t wifi_scan_thread;
static atomic_bool wifi_scan_done_flag = false;
/* The scan thread's private result buffer; poll_wifi_scan() copies it into
 * wifi_scan_results on the UI thread after the join, so the rows and the
 * setup list never read an array the worker is writing. */
static wifi_network_t wifi_scan_worker_results[WIFI_MAX_RESULTS];
static int wifi_scan_worker_count;

static void * wifi_scan_thread_func(void * arg) {
    (void) arg;
    /* No auto-enable here anymore -- the screen's own toggle row is now the
     * explicit way to turn Wi-Fi on, so a stray Rescan tap while it's off
     * should just find nothing rather than silently flipping the radio on
     * behind the toggle's back. */
    wifi_control_scan_start();
    wifi_control_scan_wait(8000); /* CTRL-EVENT-SCAN-RESULTS, usually 2-4 s */
    wifi_scan_worker_count = wifi_control_get_results(wifi_scan_worker_results, WIFI_MAX_RESULTS);
    atomic_store_explicit(&wifi_scan_done_flag, true, memory_order_release); /* written last -- poll_wifi_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. The list just updates in place
 * (populate_wifi_screen(), below) once the scan finishes, whichever screen
 * the user happens to be on by then. An earlier version pushed a
 * "Scanning for networks..." interstitial shared with bt scan/the bt
 * toggle's own overlay -- see git history if that's ever needed again, but
 * it was also the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay, plus the overlay
 * getting buried under further navigation), which not having an overlay at
 * all sidesteps entirely. */
static bool start_wifi_scan(void) {
    if (wifi_scan_active) return false;

    atomic_store_explicit(&wifi_scan_done_flag, false, memory_order_relaxed);
    wifi_scan_active = true;
    set_wifi_rescan_active(true);

    if (pthread_create(&wifi_scan_thread, NULL, wifi_scan_thread_func, NULL) != 0) {
        wifi_scan_active = false;
        set_wifi_rescan_active(false);
        return false;
    }
    return true;
}

/* Supplicant events (wifi_status.h): the screens follow connects, drops
 * and scans made by the supplicant itself (roaming, auto-reconnect), and a
 * saved network whose password stopped working is reported. */
static void poll_wifi_events(void) {
    unsigned ev = wifi_status_take_events();
    if (!ev) return;
    bool connect_in_flight = wifi_connect_active || wifi_connect_saved_active;
    if ((ev & WIFI_STATUS_EVT_WRONG_KEY) && !connect_in_flight && wifi_screen && gui_navigation_is_top(wifi_screen))
        show_error_toast(TR("Wrong Wi-Fi password"));
    if ((ev & (WIFI_STATUS_EVT_CONNECTED | WIFI_STATUS_EVT_DISCONNECTED)) && !connect_in_flight) {
        wifi_settings_snapshot_invalidate();
        wifi_settings_snapshot_start();
    }
}

void poll_wifi_scan(void) {
    poll_wifi_events();
    if (wifi_settings_snapshot_enabled && !gui_shell_wifi_effective_enabled()) {
        wifi_settings_snapshot_enabled = false;
        wifi_settings_snapshot_invalidate();
    }
    bool snapshot_published = poll_wifi_settings_snapshot();
    bool scan_published = false;
    if (wifi_scan_active && atomic_load_explicit(&wifi_scan_done_flag, memory_order_acquire)) {
        wifi_scan_active = false;
        pthread_join(wifi_scan_thread, NULL);
        int count = wifi_scan_worker_count;
        if (count < 0) count = 0;
        if (count > WIFI_MAX_RESULTS) count = WIFI_MAX_RESULTS;
        memcpy(wifi_scan_results, wifi_scan_worker_results, (size_t) count * sizeof(wifi_scan_results[0]));
        wifi_scan_result_count = count;
        set_wifi_rescan_active(false);
        scan_published = true;
    }
    if (scan_published && wifi_setup_scan_started) wifi_setup_scan_finished = true;

    if ((snapshot_published || scan_published) &&
        wifi_screen && gui_navigation_is_top(wifi_screen))
        populate_wifi_screen(wifi_control_is_enabled());
    if (snapshot_published || scan_published) wifi_setup_render();

    if (snapshot_published) {
        if (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) populate_wifi_info_screen();
        if (FEATURE_SCREEN(WIFI_IMPORT, gui_network_get_import_wifi_screen()) &&
            gui_navigation_is_top(gui_network_get_import_wifi_screen()))
            populate_import_wifi_screen();
        if (FEATURE_SCREEN(REMOTE, gui_remote_get_screen()) && gui_navigation_is_top(gui_remote_get_screen()))
            remote_control_refresh_address();
    }
    /* The Remote Control screen's PIN and Bluetooth lines (gui_remote.c). */
    remote_control_poll_screen();
    if ((wifi_screen && gui_navigation_is_top(wifi_screen)) ||
        (wifi_setup_requested && !current_settings.setup_complete &&
         gui_shell_wifi_enabled_and_settled()) ||
        (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) ||
        (FEATURE_SCREEN(WIFI_IMPORT, gui_network_get_import_wifi_screen()) &&
         gui_navigation_is_top(gui_network_get_import_wifi_screen())) ||
        (FEATURE_SCREEN(REMOTE, gui_remote_get_screen()) && gui_navigation_is_top(gui_remote_get_screen())))
        wifi_settings_snapshot_start();
}

static void show_wifi_toggle_pending_async(void * user_data) {
    bool enabled = (bool) (intptr_t) user_data;
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) return;

    wifi_enable_pending_feedback = enabled;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_scan_result_count = 0;
    populate_wifi_screen(enabled);
    set_wifi_rescan_active(enabled || wifi_scan_active);
}

void gui_network_show_wifi_toggle_pending(bool enabled) {
    /* Rebuilding wifi_list synchronously from its toggle row callback would
     * delete LVGL's active event target. Defer by one UI turn, matching the
     * safe pattern used elsewhere for destructive screen rebuilds. */
    lv_async_call(show_wifi_toggle_pending_async, (void *) (intptr_t) enabled);
}

void gui_network_wifi_toggle_completed(bool enabled) {
    wifi_enable_pending_feedback = false;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_settings_snapshot_start();
    if (wifi_setup_requested) {
        wifi_setup_enable_failed = !enabled && !gui_shell_wifi_effective_enabled();
        if (enabled) wifi_setup_start_scan_once();
        wifi_setup_render();
    }
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) {
        set_wifi_rescan_active(wifi_scan_active);
        return;
    }

    populate_wifi_screen(enabled);
    if (enabled) start_wifi_scan();
    else set_wifi_rescan_active(wifi_scan_active);
}

static pthread_t wifi_forget_thread;
static bool wifi_forget_active = false;
static atomic_bool wifi_forget_done_flag = false;

typedef struct {
    int id;
} wifi_forget_request_t;

static void * wifi_forget_thread_func(void * arg) {
    wifi_forget_request_t * req = (wifi_forget_request_t *) arg;
    wifi_control_forget(req->id);
    free(req);
    atomic_store_explicit(&wifi_forget_done_flag, true, memory_order_release); /* written last -- poll_wifi_forget only checks this flag */
    return NULL;
}

static void start_wifi_forget(int id) {
    if (wifi_forget_active) return; /* a second tap would join the first on the UI thread */
    wifi_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;
    atomic_store_explicit(&wifi_forget_done_flag, false, memory_order_relaxed);
    wifi_forget_active = true;
        if (pthread_create(&wifi_forget_thread, NULL, wifi_forget_thread_func, req) != 0) {
        wifi_forget_active = false;
        free(req);
    }
}

void poll_wifi_forget(void) {
    if (!wifi_forget_active || !atomic_load_explicit(&wifi_forget_done_flag, memory_order_acquire)) return;
    wifi_forget_active = false;
    pthread_join(wifi_forget_thread, NULL);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh both memorized and available sections */
}

/* ---- Wi-Fi saved-network action popup ------------------------------------
 * Presents options to Connect or Forget for a selected memorized network,
 * matching bt_action_popup's overlay and backdrop pattern. */
static gui_popup_t wifi_action_popup;
static lv_obj_t * wifi_action_popup_title;
static int wifi_action_popup_network_index = -1;

static void hide_wifi_action_popup(void) {
    gui_popup_hide(&wifi_action_popup);
    wifi_action_popup_network_index = -1;
}

static void wifi_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_action_popup();
}

static void wifi_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    wifi_saved_network_t * net = &wifi_saved_results[wifi_action_popup_network_index];
    int id = net->id;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", net->ssid);
    hide_wifi_action_popup();
    start_wifi_connect_saved(id, ssid);
}

static void wifi_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    int id = wifi_saved_results[wifi_action_popup_network_index].id;
    hide_wifi_action_popup();
    start_wifi_forget(id);
}

void build_wifi_action_popup(void) {
    wifi_action_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &wifi_action_popup_title, NULL, TR("Connect"),
                                                   accent_lv_color(), wifi_action_connect_cb, NULL, TR("Forget"),
                                                   lv_color_make(255, 120, 120), wifi_action_forget_cb, NULL,
                                                   wifi_action_popup_backdrop_cb, &wifi_action_popup.backdrop);
}

static void show_wifi_action_popup(int index) {
    wifi_action_popup_network_index = index;
    lv_label_set_text(wifi_action_popup_title, wifi_saved_results[index].ssid);
    gui_popup_show(&wifi_action_popup);
}

static void wifi_saved_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_wifi_action_popup(index);
}

/* ---- Wi-Fi Info screen (read-only) ---- */

static lv_obj_t * wifi_info_list;

static void populate_wifi_info_screen(void) {
    lv_obj_clean(wifi_info_list);

    wifi_settings_snapshot_start();
    if (!wifi_cached_info_is_current() || !wifi_cached_info_connected) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, wifi_settings_snapshot_active ? TR("Loading...") : TR("Not connected"));
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        return;
    }

    wifi_info_t info = wifi_cached_info;
    static const char * signal_labels[] = { N_("Weak"), N_("Fair"), N_("Good"), N_("Excellent") };
    int signal_index = (info.signal_level >= 0 && info.signal_level <= 3) ? info.signal_level : 0;
    char lines[5][160];
    snprintf(lines[0], sizeof(lines[0]), TR("SSID: %s"), info.ssid);
    snprintf(lines[1], sizeof(lines[1]), TR("IP Address: %s"), info.ip[0] ? info.ip : "-");
    snprintf(lines[2], sizeof(lines[2]), TR("Gateway: %s"), info.gateway[0] ? info.gateway : "-");
    snprintf(lines[3], sizeof(lines[3]), TR("MAC Address: %s"), info.mac[0] ? info.mac : "-");
    snprintf(lines[4], sizeof(lines[4]), TR("Signal: %s"), TR(signal_labels[signal_index]));

    for (int i = 0; i < 5; i++) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, lines[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        lv_obj_set_style_pad_right(label, BOARD_SCALE_PX(24), 0);
        lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(12), 0);
    }
}

static lv_obj_t * build_wifi_info_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Wi-Fi Info"), &title_label, &wifi_info_list);
}

static void open_wifi_info_screen(void) {
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_info_screen();
    nav_push(wifi_info_screen);
}

static void wifi_info_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_info_screen();
}

/* ---- Manual SSID entry -- chains the same show_text_entry() flow scan
 * results already use for a secured network's password, just starting from
 * a typed SSID instead of a tapped scan row. An empty password submission
 * connects as an open network (start_wifi_connect() already treats an empty
 * password as "no password"), so there's no separate "is this secured?"
 * question to ask up front. ---- */

static void wifi_manual_ssid_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", text);
    show_text_entry(TR("Wi-Fi Password"), NULL, true, false, WIFI_PASSWORD_SIZE, wifi_password_entered_cb, NULL);
}

static void wifi_manual_entry_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Network Name (SSID)"), NULL, false, false, WIFI_MAX_SSID_LEN, wifi_manual_ssid_entered_cb, NULL);
}

/* ---- DNS settings screen -- view/edit the 2 servers currently in
 * /etc/resolv.conf; wifi_control_set_dns() keeps them and re-applies them
 * after every DHCP lease. ---- */

static lv_obj_t * wifi_dns_list;
static char wifi_dns_servers[2][16];
static int wifi_dns_server_count = 0;

static void wifi_dns_row_cb(lv_event_t * e);

static void populate_wifi_dns_screen(void) {
    lv_obj_clean(wifi_dns_list);

    char current[2][16];
    int count = wifi_control_get_dns(current, 2);
    for (int i = 0; i < 2; i++) {
        snprintf(wifi_dns_servers[i], sizeof(wifi_dns_servers[i]), "%s", i < count ? current[i] : "");
    }
    wifi_dns_server_count = count;

    static const char * slot_labels[2] = { N_("Primary DNS"), N_("Secondary DNS") };
    for (int i = 0; i < 2; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s: %s", TR(slot_labels[i]), wifi_dns_servers[i][0] ? wifi_dns_servers[i] : TR("Not set"));
        lv_obj_t * row = add_pill_chevron_row(wifi_dns_list, text, NULL);
        lv_obj_add_event_cb(row, wifi_dns_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void wifi_dns_entered_cb(const char * text, void * user_data) {
    int slot = (int) (intptr_t) user_data;
    if (text && text[0] && !wifi_control_dns_server_is_valid(text)) {
        show_error_toast(TR("Enter an IPv4 address, such as 1.1.1.1"));
        return;
    }
    snprintf(wifi_dns_servers[slot], sizeof(wifi_dns_servers[slot]), "%s", text);
    if (slot + 1 > wifi_dns_server_count) wifi_dns_server_count = slot + 1;

    const char * servers[2] = { wifi_dns_servers[0], wifi_dns_servers[1] };
    wifi_control_set_dns(servers, wifi_dns_server_count);
    populate_wifi_dns_screen();
}

static void wifi_dns_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    show_text_entry(slot == 0 ? TR("Primary DNS") : TR("Secondary DNS"), wifi_dns_servers[slot], false, false,
                    sizeof(wifi_dns_servers[slot]), wifi_dns_entered_cb,
                    (void *) (intptr_t) slot);
}

static lv_obj_t * build_wifi_dns_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("DNS Settings"), &title_label, &wifi_dns_list);
}

static void open_wifi_dns_screen(void) {
    populate_wifi_dns_screen();
    nav_push(wifi_dns_screen);
}

static void wifi_dns_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_dns_screen();
}

/* ---- Master rebuild for the Wi-Fi screen's one dynamic list: toggle row,
 * then (only while enabled) the info/manual-entry/DNS rows, memorized
 * networks, and available networks -- called after every state change
 * (toggle, scan, connect, disconnect, forget) rather than patched
 * incrementally, same full-rebuild convention as every other list in this
 * file. ---- */
/* enabled is passed in rather than read via wifi_control_is_enabled()
 * internally so the tap-to-toggle handler (quick_drawer_wifi_event_cb) can
 * pass its own optimistic target state instead of the real (not-yet-caught-
 * up) one -- see that function's own comment. Every other caller just
 * passes the real wifi_control_is_enabled() value, same as before this was
 * a parameter. */
void populate_wifi_screen(bool enabled) {
    /* Rebuilt after every scan (and the screen rescans periodically): keep
     * the user's scroll position, as populate_bt_screen() does. */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(wifi_list);
    lv_obj_clean(wifi_list);
    wifi_settings_snapshot_enabled = enabled;

    if (wifi_rescan_btn) {
        if (enabled) lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* quick_drawer_wifi_event_cb is the SAME real toggle-thread trigger the
     * drawer's own wifi icon uses -- no drawer-specific logic in it, so
     * it's reused directly here rather than duplicating the thread-kickoff
     * boilerplate. poll_wifi_toggle() repopulates this screen once the
     * toggle completes. */
    add_pill_toggle_row(wifi_list, TR("Wi-Fi"), enabled, quick_drawer_wifi_event_cb);

    if (!enabled) return;

    wifi_settings_snapshot_start();

    add_pill_chevron_row(wifi_list, TR("Wi-Fi Info"), wifi_info_row_cb);
    add_pill_chevron_row(wifi_list, TR("Manual SSID Entry"), wifi_manual_entry_row_cb);
    /* Not on the HiBy back end (R3 Pro): DNS comes from the stock udhcpc lease. */
    if (wifi_control_supports_custom_dns())
        add_pill_chevron_row(wifi_list, TR("DNS Settings"), wifi_dns_settings_row_cb);

    add_section_header(wifi_list, TR("Memorized Networks"));
    /* During an optimistic cold enable there is no wpa_supplicant control
     * socket yet. Keep the last cached saved-network rows instead of
     * querying a supplicant that cannot answer yet. */
    bool have_settings_snapshot = wifi_cached_info_is_current();
    if (wifi_saved_result_count == 0) {
        if (have_settings_snapshot)
            build_list_message(wifi_list, TR("No memorized networks"), TR("Networks you connect to will appear here."));
        else
            build_list_message(wifi_list, TR("Loading Wi-Fi settings"), TR("Saved network details are being checked."));
    }
    /* Cross-reference the connection status against saved network SSIDs to
     * indicate which saved network is currently connected. */
    bool wifi_currently_connected = have_settings_snapshot && wifi_cached_info_connected;
    for (int i = 0; i < wifi_saved_result_count; i++) {
        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool is_connected = wifi_currently_connected && strcmp(wifi_saved_results[i].ssid, wifi_cached_info.ssid) == 0;
        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, wifi_saved_results[i].ssid);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        configure_network_row_line(label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                                   gui_theme_font(GUI_FONT_ROLE_BODY));

        lv_obj_t * status = lv_label_create(row);
        lv_label_set_text(status, is_connected ? TR("Connected") : TR("Saved network"));
        lv_obj_add_style(status, is_connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
        configure_network_row_line(status, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                                   gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
        layout_network_row(row, LIST_ROW_LABEL_INSET, label, status, NULL);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_saved_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    add_section_header(wifi_list, TR("Available Networks"));
    for (int i = 0; i < wifi_scan_result_count; i++) {
        wifi_network_t * net = &wifi_scan_results[i];

        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = lv_image_create(row);
        char icon_path[40];
        snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", net->signal_level);
        lv_image_set_src(icon, asset_path(icon_path));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(20), 0);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, net->ssid);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        configure_network_row_line(label, LIST_ROW_WIDTH - BOARD_SCALE_PX(88),
                                   gui_theme_font(GUI_FONT_ROLE_BODY));

        lv_obj_t * status = lv_label_create(row);
        const char * network_status = net->is_current
            ? (net->secured ? TR("Secured · Connected") : TR("Open · Connected"))
            : net->secured ? TR("Secured network") : TR("Open network");
        lv_label_set_text(status, network_status);
        lv_obj_add_style(status, net->is_current ? gui_theme_accent_style() : &style_theme_text_muted, 0);
        configure_network_row_line(status, LIST_ROW_WIDTH - BOARD_SCALE_PX(88),
                                   gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
        layout_network_row(row, BOARD_SCALE_PX(64), label, status, NULL);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(20), 0);

        wifi_network_t * row_net = malloc(sizeof(*row_net));
        if (row_net) {
            *row_net = *net;
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, wifi_row_delete_cb, LV_EVENT_DELETE, row_net);
            lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, row_net);
        }
    }
    if (wifi_scan_result_count == 0) {
        if (wifi_enable_pending_feedback)
            build_list_message(wifi_list, TR("Scanning for networks"), TR("Nearby Wi-Fi networks will appear here."));
        else
            build_list_message(wifi_list, TR("No networks found"), TR("Check that Wi-Fi is enabled, then rescan."));
    }
    lv_obj_update_layout(wifi_list);
    lv_obj_scroll_to_y(wifi_list, saved_scroll_y, LV_ANIM_OFF);
}

static lv_obj_t * build_wifi_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen(TR("Wi-Fi"), &title_label, &wifi_list);

    wifi_rescan_btn = lv_label_create(scr);
    lv_label_set_text(wifi_rescan_btn, TR("Rescan"));
    lv_obj_add_style(wifi_rescan_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(wifi_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(wifi_rescan_btn, 20);
    lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_rescan_btn, wifi_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void gui_setup_open_wifi(void) {
    open_wifi_screen();
}

void open_wifi_screen(void) {
    bool enabled = wifi_control_is_enabled();
    wifi_settings_snapshot_invalidate();
    populate_wifi_screen(enabled);
    nav_push(wifi_screen);
    if (enabled) start_wifi_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with */
}

/* The cached Wi-Fi details, for the screens in other files that show them
 * (Import via Wi-Fi, Remote Control); only meaningful while
 * wifi_cached_info_is_current(). */
const wifi_info_t * gui_wifi_cached_info(void) { return &wifi_cached_info; }
bool gui_wifi_cached_info_connected(void) { return wifi_cached_info_connected; }

/* Keeps the snapshot refreshing while one of those screens is open, as
 * opening Wi-Fi Info does. */
void gui_wifi_set_snapshot_enabled(bool enabled) { wifi_settings_snapshot_enabled = enabled; }

void gui_wifi_init(void) {
    wifi_screen = build_wifi_screen();
    wifi_info_screen = build_wifi_info_screen();
    wifi_dns_screen = build_wifi_dns_screen();
}

void gui_wifi_teardown_setup(void) {
    wifi_setup_parent = NULL;
    wifi_setup_requested = false;
    wifi_setup_scan_started = false;
    wifi_setup_scan_finished = false;
    wifi_setup_enable_failed = false;
    wifi_settings_snapshot_enabled = false;
    wifi_settings_snapshot_invalidate();
}

void gui_wifi_teardown_action_popup(void) {
    gui_popup_teardown(&wifi_action_popup);
}

void gui_wifi_teardown_screens(void) {
    if (wifi_screen) { lv_obj_delete(wifi_screen); wifi_screen = NULL; }
    if (wifi_info_screen) { lv_obj_delete(wifi_info_screen); wifi_info_screen = NULL; }
    if (wifi_dns_screen) { lv_obj_delete(wifi_dns_screen); wifi_dns_screen = NULL; }
}

bool gui_wifi_has_background_work(void) {
    return wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active ||
           wifi_scan_active || wifi_forget_active || wifi_settings_snapshot_active;
}

void gui_wifi_cancel_background_work(void) {
    if (wifi_connect_active) {
        pthread_join(wifi_connect_thread, NULL);
        wifi_connect_active = false;
        gui_busy_hide(wifi_connect_token);
    }
    if (wifi_connect_saved_active) {
        pthread_join(wifi_connect_saved_thread, NULL);
        wifi_connect_saved_active = false;
        gui_busy_hide(wifi_connect_saved_token);
    }
    if (wifi_disconnect_active) {
        pthread_join(wifi_disconnect_thread, NULL);
        wifi_disconnect_active = false;
    }
    if (wifi_scan_active) {
        pthread_join(wifi_scan_thread, NULL);
        wifi_scan_active = false;
    }
    if (wifi_forget_active) {
        pthread_join(wifi_forget_thread, NULL);
        wifi_forget_active = false;
    }
    if (wifi_settings_snapshot_active) {
        pthread_join(wifi_settings_snapshot_thread, NULL);
        wifi_settings_snapshot_active = false;
    }
}

lv_obj_t * gui_network_get_wifi_screen(void) { return wifi_screen; }

/* ---- Shared Wi-Fi dependency guard for AirPlay/DLNA/Import via Wi-Fi --
 * these require the Wi-Fi radio/interface to be enabled
 * (NOT association with an access point; individual screens keep showing
 * their own existing "connect first" state for that). Checks gui_shell.c's
 * effective state (an in-flight disable counts as already off, see that
 * function's own comment) rather than wifi_control_is_enabled() directly,
 * shows the exact required toast, and returns false -- callers must bail
 * out without navigating, touching a setting, or starting anything. Shared
 * by the Wireless tile taps (screen not open yet) and each feature's own
 * enable/toggle path (screen may already be open when Wi-Fi goes away). */
bool wifi_feature_guard(void) {
    if (gui_shell_wifi_effective_enabled()) return true;
    show_error_toast(TR("Enable WiFi to access"));
    return false;
}
