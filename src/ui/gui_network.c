/* The Wireless hub screen, and the module's init/teardown/poll orchestration
 * over the files it is split into (gui_network_internal.h): Wi-Fi
 * (gui_wifi.c), Bluetooth (gui_bluetooth.c), USB (gui_usb.c), the optional
 * features' screens (gui_airplay.c, gui_dlna.c, gui_remote.c,
 * gui_wifi_import.c) and the playback settings screens built with them
 * (gui_settings_playback.c). Also the helpers and the service worker they
 * share. */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
extern int subprocess_run(char * const argv[], char ** out_output, int timeout_sec);
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_settings.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_subsonic.h"
#include "screen_builders.h"
#include "settings.h"
#include "airplay_control.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "bt_remote_control.h"
#include "import_web.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* Screens owned by this module */
static lv_obj_t * wireless_screen;

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR BOARD_SD_ROOT ""
#endif

void configure_network_row_line(lv_obj_t * label, int32_t width,
                                const lv_font_t * font) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, font, 0);
    /* Exactly one line tall (dots on overflow). The shared bounded-height
     * helper adds a top pad sized for Montserrat, which pushed the stock
     * UI font's text below the row on the R3 Pro. */
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

/* Stacks a network row's one-line labels (name, status, optional third
 * line) from their fonts' real line heights, vertically centred, and grows
 * the row to fit -- replaces fixed y offsets that assumed Montserrat.
 * Heights come from the fonts: the labels' own sizes are not computed
 * until the next layout pass. */
void layout_network_row(lv_obj_t * row, int32_t x, lv_obj_t * line1, lv_obj_t * line2,
                        lv_obj_t * line3) {
    lv_obj_t * lines[3] = { line1, line2, line3 };
    int32_t gap = BOARD_SCALE_PX(2);
    int32_t pad = BOARD_SCALE_PX(10);
    int32_t group = 0;
    for (int i = 0; i < 3; i++) {
        if (!lines[i]) continue;
        if (group) group += gap;
        group += lv_font_get_line_height(lv_obj_get_style_text_font(lines[i], LV_PART_MAIN));
    }
    int32_t height = group + 2 * pad;
    if (height < LIST_ROW_HEIGHT) height = LIST_ROW_HEIGHT;
    lv_obj_set_height(row, height);
    lv_obj_set_style_pad_all(row, 0, 0);
    int32_t y = (height - group) / 2;
    for (int i = 0; i < 3; i++) {
        if (!lines[i]) continue;
        lv_obj_align(lines[i], LV_ALIGN_TOP_LEFT, x, y);
        y += lv_font_get_line_height(lv_obj_get_style_text_font(lines[i], LV_PART_MAIN)) + gap;
    }
}

void set_label_if_changed(lv_obj_t * label, const char * text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void format_rate(char * out, size_t size, unsigned int rate) {
    if (rate && rate % 1000 == 0) snprintf(out, size, "%u kHz", rate / 1000);
    else if (rate) snprintf(out, size, "%.1f kHz", (double) rate / 1000.0);
    else snprintf(out, size, "%s", TR("Unknown rate"));
}

/* Shared N-row action-menu popup shape is built by build_menu_popup in gui.c. */

/* All service lifecycle calls below are process/socket teardown or setup and
 * must not run on the LVGL thread.  Keep one bounded, joinable worker and a
 * tiny FIFO so Wi-Fi-loss cleanup can serialize several stops without ever
 * overlapping a start/stop for the same service.  The worker contains no UI
 * code; completion is joined and consumed by poll_network_service_ops(). */
typedef struct {
    network_service_t service;
    bool start;
} network_service_job_t;

#define NETWORK_SERVICE_QUEUE_CAPACITY 8
static pthread_mutex_t network_service_mutex = PTHREAD_MUTEX_INITIALIZER;
static network_service_job_t network_service_queue[NETWORK_SERVICE_QUEUE_CAPACITY];
static unsigned network_service_queue_head;
static unsigned network_service_queue_count;
static bool network_service_busy_flags[NETWORK_SERVICE_COUNT];
static bool network_service_stop_queued[NETWORK_SERVICE_COUNT];
static bool network_service_worker_active;
static pthread_t network_service_thread;
static atomic_bool network_service_done = false;

static void poll_network_service_ops(void);

static void * network_service_worker_func(void * arg) {
    (void) arg;
    for (;;) {
        network_service_job_t job;
        pthread_mutex_lock(&network_service_mutex);
        if (network_service_queue_count == 0) {
            pthread_mutex_unlock(&network_service_mutex);
            break;
        }
        job = network_service_queue[network_service_queue_head];
        network_service_queue_head = (network_service_queue_head + 1) % NETWORK_SERVICE_QUEUE_CAPACITY;
        network_service_queue_count--;
        pthread_mutex_unlock(&network_service_mutex);

        if (job.service == NETWORK_SERVICE_IMPORT) {
            if (job.start) import_web_start();
            else import_web_stop();
        } else if (job.service == NETWORK_SERVICE_REMOTE) {
            if (job.start) {
                remote_control_start();
#ifndef HOST_BUILD
                bt_remote_control_start();
#endif
            } else {
                remote_control_stop();
#ifndef HOST_BUILD
                bt_remote_control_stop();
#endif
            }
        } else if (job.service == NETWORK_SERVICE_DLNA) {
            if (job.start) dlna_control_start();
            else dlna_control_stop();
        }
        pthread_mutex_lock(&network_service_mutex);
        bool same_service_queued = false;
        for (unsigned i = 0; i < network_service_queue_count; i++) {
            unsigned index = (network_service_queue_head + i) % NETWORK_SERVICE_QUEUE_CAPACITY;
            if (network_service_queue[index].service == job.service) {
                same_service_queued = true;
                break;
            }
        }
        if (!same_service_queued) network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
    }
    atomic_store_explicit(&network_service_done, true, memory_order_release);
    return NULL;
}

bool network_service_is_busy(network_service_t service) {
    pthread_mutex_lock(&network_service_mutex);
    bool busy = network_service_busy_flags[service];
    pthread_mutex_unlock(&network_service_mutex);
    return busy;
}

static bool network_service_launch_locked(void) {
    atomic_store_explicit(&network_service_done, false, memory_order_relaxed);
    network_service_worker_active = true;
    if (pthread_create(&network_service_thread, NULL, network_service_worker_func, NULL) != 0) {
        network_service_worker_active = false;
        return false;
    }
    return true;
}

bool network_service_enqueue(network_service_t service, bool start, bool force_stop) {
    /* A completed worker may still await the next UI poll. Reap it before
     * appending work, otherwise a new job could be stranded behind a thread
     * that has already exited. */
    poll_network_service_ops();
    pthread_mutex_lock(&network_service_mutex);
    if ((!force_stop && network_service_busy_flags[service]) ||
        (force_stop && network_service_stop_queued[service]) ||
        network_service_queue_count >= NETWORK_SERVICE_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&network_service_mutex);
        return false;
    }
    unsigned tail = (network_service_queue_head + network_service_queue_count) % NETWORK_SERVICE_QUEUE_CAPACITY;
    bool was_busy = network_service_busy_flags[service];
    bool had_stop = network_service_stop_queued[service];
    network_service_queue[tail] = (network_service_job_t){ .service = service, .start = start };
    network_service_queue_count++;
    network_service_busy_flags[service] = true;
    if (!start) network_service_stop_queued[service] = true;
    if (!network_service_worker_active) {
        if (!network_service_launch_locked()) {
            network_service_queue_count--;
            network_service_busy_flags[service] = was_busy;
            network_service_stop_queued[service] = had_stop;
            network_service_worker_active = false;
            pthread_mutex_unlock(&network_service_mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&network_service_mutex);
    return true;
}

static void poll_network_service_ops(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool active = network_service_worker_active;
    pthread_mutex_unlock(&network_service_mutex);
    if (active && !atomic_load_explicit(&network_service_done, memory_order_acquire)) return;
    if (active) pthread_join(network_service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    if (active) network_service_worker_active = false;
    /* The worker may have observed an empty queue just before an enqueue
     * published a job and set done. Re-launch any such job while holding the
     * lifecycle lock; this closes the exit/enqueue handshake race. */
    if (network_service_queue_count > 0) {
        /* Retry creation on every UI poll. A transient pthread resource
         * failure must not strand a busy flag or an Import teardown. */
        (void) network_service_launch_locked();
    }
    pthread_mutex_unlock(&network_service_mutex);
}

void gui_network_poll_services(void) {
    poll_network_service_ops();
    import_wifi_poll_stop();
}

/* Centralized Wi-Fi-disabled cleanup for Wi-Fi-dependent features
 * -- called by gui_shell.c's poll_wifi_toggle() only once a Wi-Fi disable is
 * authoritatively confirmed (see that call site's own comment). Stops
 * whichever of AirPlay/DLNA/Import via Wi-Fi is actually running and
 * corrects their persisted settings, refreshing any of their
 * screens/toggles already built so nothing shows stale "on" state. A single
 * settings_save() at the end covers every flag this pass touched, rather
 * than one write per feature. */
void gui_network_handle_wifi_disabled(void) {
    bool settings_changed = false;

    /* Subsonic requests are ordinary background sockets, not persistent
     * services with a settings toggle. Interrupt them before tearing down
     * the rest of the Wi-Fi-owned features so their active flags cannot
     * veto idle suspend after the radio is already off. */
    gui_subsonic_handle_wifi_disabled();

    if (MT_HAS_FEATURE(AIRPLAY) && current_settings.wifi_dac_mode_enabled) {
        airplay_control_stop();
        current_settings.wifi_dac_mode_enabled = false;
        settings_changed = true;
        populate_airplay_screen();
    }

    if (MT_HAS_FEATURE(DLNA_RENDERER) && current_settings.dlna_renderer_enabled) {
        if (network_service_enqueue(NETWORK_SERVICE_DLNA, false, true)) {
            current_settings.dlna_renderer_enabled = false;
            settings_changed = true;
            populate_dlna_screen();
        }
    }

    /* Import via Wi-Fi has no persisted enabled flag (session-only, started
     * only when its screen opens) -- import_wifi_handle_wifi_disabled() is
     * a cheap no-op whenever it isn't currently running. */
    if (MT_HAS_FEATURE(WIFI_IMPORT)) import_wifi_handle_wifi_disabled();

    if (settings_changed) settings_save(&current_settings);
}

static void wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_screen();
}

static void bt_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bluetooth_screen();
}

static lv_obj_t * build_wireless_screen(void) {
    static icon_grid_item_t items[6];
    /* No .bg_image here: the stock per-tile background pill isn't present in
     * every board's own firmware (missing on R1, so the tile silently rendered
     * without one there) and measurably added render/decode cost on R3 Pro II
     * for a background that isn't load-bearing to this screen. */
    int n = 0;
    items[n++] = (icon_grid_item_t){ "wireless/list_wifi.png", NULL, TR("Wi-Fi"), wifi_tile_cb, NULL };
    items[n++] = (icon_grid_item_t){ "wireless/list_bt.png", NULL, TR("Bluetooth"), bt_tile_cb, NULL };
    /* Optional features: AirPlay (shairport), DLNA renderer
     * (dmrd), Remote Control and Wi-Fi Import (thttpd/udp_server). */
    if (MT_HAS_FEATURE(AIRPLAY))
        items[n++] = (icon_grid_item_t){ "wireless/list_airplay.png", NULL, TR("AirPlay"), airplay_tile_cb, NULL };
    if (MT_HAS_FEATURE(DLNA_RENDERER))
        items[n++] = (icon_grid_item_t){ "wireless/list_dlna.png", NULL, TR("DLNA"), dlna_tile_cb, NULL };
    if (MT_HAS_FEATURE(REMOTE))
        items[n++] = (icon_grid_item_t){ "wireless/list_remote.png", NULL, TR("Remote"), remote_control_tile_cb, NULL };
    if (MT_HAS_FEATURE(WIFI_IMPORT))
        items[n++] = (icon_grid_item_t){ "wireless/list_import.png", NULL, TR("Import"), import_wifi_tile_cb, NULL };
    lv_obj_t * scr = build_category_menu_screen(TR("Wireless"), generic_back_cb, items, n,
                                                &launcher_layout_config.wireless);
    finalize_screen_navigation(scr);
    return scr;
}

bool gui_network_has_wireless_services(void) {
    return MT_HAS_FEATURE(AIRPLAY) || MT_HAS_FEATURE(DLNA_RENDERER) || MT_HAS_FEATURE(REMOTE) ||
           MT_HAS_FEATURE(WIFI_IMPORT);
}

bool gui_network_wireless_services_is_remote_only(void) {
    return MT_HAS_FEATURE(REMOTE) && !MT_HAS_FEATURE(AIRPLAY) && !MT_HAS_FEATURE(DLNA_RENDERER) &&
           !MT_HAS_FEATURE(WIFI_IMPORT);
}

void gui_network_open_wireless_services(void) {
#if MT_HAS_FEATURE(REMOTE)
    if (gui_network_wireless_services_is_remote_only()) {
        gui_remote_open_screen();
        return;
    }
#endif
    if (wireless_screen) nav_push(wireless_screen);
}

void gui_network_refresh_wireless_screen(void) {
    lv_obj_t * old = wireless_screen;
    lv_obj_t * fresh = build_wireless_screen();
    if (!fresh) return;
    wireless_screen = fresh;
    gui_navigation_replace_static_screen(3, old, fresh);
    if (old) lv_obj_delete(old);
}

static lv_timer_t * dac_stream_labels_timer = NULL;

static void dac_stream_labels_timer_cb(lv_timer_t * timer) {
    (void) timer;
    gui_bluetooth_update_dac_stream_label();
    gui_usb_update_dac_stream_labels();
}

void gui_network_set_screen_on(bool on) {
    if (!dac_stream_labels_timer) return;
    if (on) {
        lv_timer_resume(dac_stream_labels_timer);
        lv_timer_ready(dac_stream_labels_timer);
    } else {
        lv_timer_pause(dac_stream_labels_timer);
    }
}

void gui_network_init(void) {
    gui_wifi_init();
    gui_bluetooth_init();
    gui_settings_playback_init();
    gui_usb_init();
    if (MT_HAS_FEATURE(WIFI_IMPORT)) gui_wifi_import_init();
    if (MT_HAS_FEATURE(AIRPLAY)) gui_airplay_init();
    if (MT_HAS_FEATURE(DLNA_RENDERER)) gui_dlna_init();
    if (MT_HAS_FEATURE(REMOTE)) gui_remote_init();
    wireless_screen = build_wireless_screen();
    build_bt_action_popup();
    build_wifi_action_popup();
    build_usb_dac_leave_popup();
    build_bt_dac_leave_popup();
    if (MT_HAS_FEATURE(REMOTE)) build_remote_control_new_pin_popup();
    dac_stream_labels_timer_cb(NULL);
    gui_usb_init_lend();
    /* Guarded like gui_library.c's az_index_drag_timer -- gui_network_init()
     * can run again after a UI reload, and an unguarded lv_timer_create()
     * here would leak/duplicate a recurring timer each time. */
    if (!dac_stream_labels_timer) dac_stream_labels_timer = lv_timer_create(dac_stream_labels_timer_cb, 500, NULL);
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_network_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * dac_stream_labels_timer (already guarded/reused correctly by gui_network_
 * init() itself). The five popup-and-backdrop pairs below are built
 * directly on lv_layer_top() (see build_confirm_popup()'s own comment), not
 * as children of any of these screens, so each needs its own explicit
 * deletion. */
void gui_network_teardown(void) {
    gui_wifi_teardown_setup();
    gui_bluetooth_teardown_action_popup();
    gui_wifi_teardown_action_popup();
    gui_usb_teardown_popups();
    gui_bluetooth_teardown_dac_leave_popup();
    gui_remote_teardown_widgets();

    gui_wifi_teardown_screens();
    gui_bluetooth_teardown_screens();
    gui_settings_playback_teardown();
    gui_usb_teardown_screens();
    gui_wifi_import_teardown();
    gui_airplay_teardown();
    gui_dlna_teardown();
    gui_remote_teardown_screen();
    if (wireless_screen) { lv_obj_delete(wireless_screen); wireless_screen = NULL; }
}

bool gui_network_has_background_work(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool network_service_work = network_service_worker_active || network_service_queue_count > 0;
    pthread_mutex_unlock(&network_service_mutex);
    return gui_wifi_has_background_work() || gui_bluetooth_has_background_work() ||
           gui_usb_has_background_work() || import_wifi_stop_active() ||
           network_service_work;
}

void gui_network_cancel_background_work(void) {
    gui_wifi_cancel_background_work();
    gui_bluetooth_cancel_background_work();
    gui_usb_cancel_background_work();
    /* Drain the serialized service worker before teardown. If a worker could
     * not be created, execute its bounded queue synchronously so cancellation
     * cannot leave lifecycle flags or an Import busy overlay permanently set. */
    pthread_mutex_lock(&network_service_mutex);
    bool service_worker_active = network_service_worker_active;
    pthread_t service_thread = network_service_thread;
    pthread_mutex_unlock(&network_service_mutex);
    if (service_worker_active) pthread_join(service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    network_service_worker_active = false;
    pthread_mutex_unlock(&network_service_mutex);
    for (;;) {
        pthread_mutex_lock(&network_service_mutex);
        if (network_service_queue_count == 0) {
            pthread_mutex_unlock(&network_service_mutex);
            break;
        }
        network_service_job_t job = network_service_queue[network_service_queue_head];
        network_service_queue_head = (network_service_queue_head + 1) % NETWORK_SERVICE_QUEUE_CAPACITY;
        network_service_queue_count--;
        network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
        if (job.service == NETWORK_SERVICE_IMPORT) {
            if (job.start) import_web_start(); else import_web_stop();
        } else if (job.service == NETWORK_SERVICE_REMOTE) {
            if (job.start) {
                remote_control_start();
#ifndef HOST_BUILD
                bt_remote_control_start();
#endif
            } else {
                remote_control_stop();
#ifndef HOST_BUILD
                bt_remote_control_stop();
#endif
            }
        } else if (job.service == NETWORK_SERVICE_DLNA) {
            if (job.start) dlna_control_start(); else dlna_control_stop();
        }
    }
    import_wifi_cancel_stop();
}

lv_obj_t * gui_network_get_wireless_screen(void) { return wireless_screen; }

/* A feature that is not built (mt_features.h) leaves its screen's file out;
 * its public entry points answer as the feature's own code does when off. */
#if !MT_HAS_FEATURE(AIRPLAY)
bool gui_network_toggle_airplay(void) { return false; }
void gui_network_poll_airplay_overlay(void) {}
#endif
#if !MT_HAS_FEATURE(DLNA_RENDERER)
bool gui_network_toggle_dlna(void) { return false; }
#endif
#if !MT_HAS_FEATURE(REMOTE)
bool gui_network_toggle_remote_control(void) { return false; }
#ifdef TEST_BOOT_RC
void gui_network_start_test_boot_services(void) {}
#endif
#endif
#if !MT_HAS_FEATURE(WIFI_IMPORT)
lv_obj_t * gui_network_get_import_wifi_screen(void) { return NULL; }
#endif
