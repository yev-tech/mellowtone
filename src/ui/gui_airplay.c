/* AirPlay (optional feature AIRPLAY): the AirPlay screen and the overlay
 * shown while a stream is playing. Part of the network module
 * (gui_network.c). */
#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "gui.h"
#include "i18n.h"
#include "gui_theme.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_navigation.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "plugin_manager.h"
#include "bluetooth_control.h"
#include "airplay_control.h"
#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * airplay_screen;
static lv_obj_t * airplay_overlay_screen;
static lv_obj_t * airplay_overlay_cover_img;
static lv_obj_t * airplay_overlay_title_label;
static bool airplay_overlay_showing = false;
static lv_image_dsc_t airplay_overlay_cover_dsc;
static uint8_t * airplay_overlay_cover_bytes = NULL;
/* This screen's own stack slot, recorded right before nav_push() -- same
 * import_web_stop_nav_slot pattern gui_wifi_import.c uses for Import via
 * Wi-Fi's busy screen. Needed because the overlay can be torn down by
 * session end while the user has already navigated to some other screen on
 * top of it (or underneath it in history); without spawning it, nav_pop()
 * alone would either pop the wrong screen or leave this now-defunct one
 * buried in the back-navigation history for a later "back" to resurface. */
static int airplay_overlay_nav_slot = -1;

/* ---- AirPlay screen -- WiFi analogue of Bluetooth DAC mode, see
 * airplay_control.h for the real mechanism (stock firmware's own shairport
 * binary). Same shape as build_bt_dac_screen(): a toggle plus an
 * explanation, and the same three-way mutual exclusion with local playback
 * and Bluetooth DAC. ---- */

static lv_obj_t * airplay_list;

static void airplay_toggle_cb(lv_event_t * e);

void populate_airplay_screen(void) {
    lv_obj_clean(airplay_list);
    add_pill_toggle_row(airplay_list, TR("AirPlay"), current_settings.wifi_dac_mode_enabled, airplay_toggle_cb);

    lv_obj_t * explanation = lv_label_create(airplay_list);
    lv_label_set_text(explanation,
                      TR("When on, this device is visible to AirPlay senders on your Wi-Fi network -- stream "
                      "audio from an iPhone, iPad, or Mac to be played through this device's own output."));
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
}

/* The settings screens below are built lazily, so anything reachable from
 * the quick drawer (which can toggle these long before their own screen has
 * ever been opened) has to refresh through these rather than calling
 * populate_*() directly -- lv_obj_clean() on a NULL list is not survivable. */
static void refresh_airplay_screen_if_built(void) {
    if (airplay_list) populate_airplay_screen();
}

/* Shared by the AirPlay settings screen's toggle row and the quick drawer's
 * AirPlay tile -- the drawer must not reimplement any of the Wi-Fi guard,
 * transactional-start, or BT-DAC mutual-exclusion logic below. Returns the
 * resulting enabled state, which is NOT always the requested one (a rejected
 * enable returns false). */
bool gui_network_toggle_airplay(void) {
    if (!MT_HAS_FEATURE(AIRPLAY)) return false;
    bool turning_on = !current_settings.wifi_dac_mode_enabled;
    /* Guard the enable path even though the tile itself is already guarded
     * (airplay_tile_cb() below) -- this screen can already be open when
     * Wi-Fi goes away, so a stale toggle tap must still be rejected here.
     * Checked before touching the setting at all, so a rejected enable
     * never even briefly persists as on. Turning OFF is always allowed,
     * Wi-Fi state notwithstanding -- an already-enabled feature must still
     * be toggleable off. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_airplay_screen_if_built(); /* nothing changed, but keeps the toggle row's own drawn state honest */
        return current_settings.wifi_dac_mode_enabled;
    }
    current_settings.wifi_dac_mode_enabled = turning_on;
    settings_save_async(&current_settings);

    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        if (!airplay_control_start(name)) {
            /* Startup is transactional (see airplay_control_start()'s own
             * comment) -- a false return means nothing was actually left
             * running, so just undo the toggle rather than reporting the
             * device as "on" when it can't produce sound or be discovered. */
            current_settings.wifi_dac_mode_enabled = false;
            settings_save_async(&current_settings);
            show_error_toast(TR("Failed to enable AirPlay"));
            refresh_airplay_screen_if_built();
            return current_settings.wifi_dac_mode_enabled;
        }

        /* Deliberately does NOT touch local playback here -- toggling
         * AirPlay only makes this device discoverable/ready to receive
         * (airplay_bridge_start() starts in a LISTENING state, see its own
         * comment); an actual incoming AirPlay stream starting is what
         * stops local playback (gui_network_poll_airplay_overlay() below,
         * reacting to airplay_bridge_is_streaming()), not this
         * toggle. Still mutually exclusive with Bluetooth DAC mode
         * specifically, unlike local playback: BT DAC mode's bluealsa-aplay
         * writes straight to the local hw:0 device as an entirely separate
         * process, outside this app's own audio_output.c arbitration, so
         * the two really can conflict at the hardware level the moment
         * AirPlay does start streaming -- turning BT DAC off here, rather
         * than only when AirPlay actually starts streaming, avoids ever
         * having both simultaneously fighting over real hardware. */
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
    } else {
    }

    refresh_airplay_screen_if_built();
    return current_settings.wifi_dac_mode_enabled;
}

static void airplay_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_airplay();
}

static lv_obj_t * build_airplay_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("AirPlay"), &title_label, &airplay_list);
}

static void open_airplay_screen(void) {
    populate_airplay_screen();
    nav_push(airplay_screen);
}

void airplay_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_airplay_screen();
}

/* ---- AirPlay overlay: shown only while a stream is actually active -----
 * Dedicated overlay screen displaying cover art, song name, and stream info
 * updated dynamically via airplay_metadata.c. Navigating back leaves the
 * stream running in the background until it completes or is stopped. ---- */

static void airplay_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_obj_t * build_airplay_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, airplay_overlay_back_cb, NULL, NULL);

    airplay_overlay_cover_img = lv_image_create(scr);
    lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_align(airplay_overlay_cover_img, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-60));

    airplay_overlay_title_label = lv_label_create(scr);
    lv_label_set_text(airplay_overlay_title_label, TR("AirPlay"));
    lv_obj_add_style(airplay_overlay_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(airplay_overlay_title_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(airplay_overlay_title_label, lv_pct(85));
    lv_label_set_long_mode(airplay_overlay_title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(airplay_overlay_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(airplay_overlay_title_label, airplay_overlay_cover_img, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    /* Source caption: plain text, no third-party logo artwork. */
    lv_obj_t * airplay_caption = lv_label_create(scr);
    lv_label_set_text(airplay_caption, TR("AirPlay"));
    lv_obj_add_style(airplay_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(airplay_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(airplay_caption, LV_ALIGN_BOTTOM_MID, 0, BOARD_SCALE_PX(-24));

    return scr;
}

/* Poll from update_timer_cb, same shape as every other background-thread-
 * to-UI-thread poll in this codebase. Shows/hides the overlay based on
 * airplay_bridge_is_streaming() transitions and applies whatever metadata
 * has arrived meanwhile -- decoupled from whether the overlay happens to
 * be the currently active screen, so metadata keeps updating even if the
 * user navigated away from it. */
void gui_network_poll_airplay_overlay(void) {
    /* Self-heals a respawn failure from airplay_control_disconnect_active_
     * stream()'s kill-and-respawn (gui_player.c, triggered by local playback
     * resuming while AirPlay was streaming) -- that path deliberately
     * ignores airplay_control_start()'s return value (see its own comment),
     * so this is the only place such a failure ever gets noticed and the
     * persisted setting corrected, same as airplay_toggle_cb()'s own
     * synchronous failure handling above. */
    if (!MT_HAS_FEATURE(AIRPLAY)) return;
    if (current_settings.wifi_dac_mode_enabled && !airplay_control_is_active()) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        show_error_toast(TR("AirPlay stopped unexpectedly"));
        populate_airplay_screen();
    }

    bool streaming = airplay_bridge_is_streaming();
    if (streaming != airplay_overlay_showing) {
        airplay_overlay_showing = streaming;
        if (streaming) {
            /* airplay_bridge.c's own reader thread already called
             * audio_stop() before flipping is_streaming() true (see its own
             * comment) -- that only stops the backend, same as usb_mode_
             * control_apply()'s own DAC-mode takeover (gui_usb.c's
             * poll_usb_mode_switch()); the play button icon and plugin state still
             * need their own explicit refresh here on the UI thread, or
             * they'd keep claiming playback is active/paused for a track
             * that AirPlay just silently took the output device out from
             * under. */
            set_play_button_state(false);
            plugin_manager_notify_stopped();

            airplay_overlay_nav_slot = gui_navigation_get_depth(); /* this screen's own slot, about to be occupied */
            nav_push(airplay_overlay_screen);
        } else {
            /* Reset to defaults here, on SESSION END, not on the next
             * session's start -- metadata for a new session can (and
             * regularly does) arrive slightly before airplay_bridge_is_
             * streaming() flips true for it (see the unconditional-apply
             * comment below), and resetting on start would wipe that
             * early update right back out, leaving the "AirPlay"
             * placeholder up for the entire session since nothing else
             * would trigger a re-apply. Resetting here instead means by
             * the time a NEW session's early metadata can possibly arrive,
             * this session's leftover content is already gone -- closing
             * the same staleness gap without racing a legitimate early
             * update for the next session. */
            lv_label_set_text(airplay_overlay_title_label, TR("AirPlay"));
            free(airplay_overlay_cover_bytes);
            airplay_overlay_cover_bytes = NULL;
            lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));

            if (lv_screen_active() == airplay_overlay_screen) {
                /* Only auto-dismiss if the user is still looking at it. */
                nav_pop();
            } else if (airplay_overlay_nav_slot >= 0 && airplay_overlay_nav_slot < gui_navigation_get_depth()) {
                /* User already navigated elsewhere with the overlay still
                 * buried somewhere in their back-navigation history --
                 * splice its now-defunct slot out entirely (same pattern as
                 * gui_network_poll_services()'s import_web_stop_nav_slot) rather
                 * than leaving it there for a later "back" to resurface a
                 * screen with no active stream behind it. */
                nav_remove_stack_slot(airplay_overlay_nav_slot);
            }
            airplay_overlay_nav_slot = -1;
        }
    }

    airplay_metadata_update_t upd;
    if (!airplay_metadata_consume_update(&upd)) return;

    /* Applied regardless of `streaming` -- the metadata and PCM bridge are
     * two independent threads/connections (airplay_metadata.c vs. airplay_
     * bridge.c), so a title/cover update for a session can arrive slightly
     * before airplay_bridge_is_streaming() actually flips true. Discarding
     * it here (an earlier version of this function did) meant the overlay
     * showed the plain "AirPlay" placeholder for the entire first track of
     * every session unless a later track change happened to resend it.
     * Applying it always is safe: the streaming->false reset above already
     * runs the instant a session ends, before a NEW session's own metadata
     * could possibly arrive, so nothing from a PREVIOUS session is ever
     * still sitting in these widgets by the time an early update for a new
     * one shows up here. */
    lv_label_set_text(airplay_overlay_title_label, upd.title[0] ? upd.title : TR("AirPlay"));

    if (!upd.has_cover) {
        free(airplay_overlay_cover_bytes);
        airplay_overlay_cover_bytes = NULL;
        lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
        return;
    }

    free(airplay_overlay_cover_bytes);
    airplay_overlay_cover_bytes = (uint8_t *) upd.cover_pixels;

    /* LV_IMAGE_HEADER_MAGIC is required so LVGL's image decoders recognize
     * the custom RGB565 image descriptor without attempting format fixups. */
    memset(&airplay_overlay_cover_dsc, 0, sizeof(airplay_overlay_cover_dsc));
    airplay_overlay_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    airplay_overlay_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    airplay_overlay_cover_dsc.header.w = AIRPLAY_COVER_WIDTH;
    airplay_overlay_cover_dsc.header.h = AIRPLAY_COVER_HEIGHT;
    airplay_overlay_cover_dsc.header.stride = AIRPLAY_COVER_WIDTH * 2;
    airplay_overlay_cover_dsc.data = airplay_overlay_cover_bytes;
    airplay_overlay_cover_dsc.data_size = (uint32_t) AIRPLAY_COVER_WIDTH * AIRPLAY_COVER_HEIGHT * 2;
    lv_image_set_src(airplay_overlay_cover_img, &airplay_overlay_cover_dsc);
}

void gui_airplay_init(void) {
    airplay_screen = build_airplay_screen();
    airplay_overlay_screen = build_airplay_overlay_screen();
}

void gui_airplay_teardown(void) {
    if (airplay_screen) { lv_obj_delete(airplay_screen); airplay_screen = NULL; }
    if (airplay_overlay_screen) { lv_obj_delete(airplay_overlay_screen); airplay_overlay_screen = NULL; }
}

lv_obj_t * gui_airplay_get_overlay_screen(void) { return airplay_overlay_screen; }

/* /usr/resource/hostname is the same file wifi_on.sh already reads for the
 * DHCP hostname it advertises -- reusing it here keeps the name AirPlay
 * senders see consistent with how this device already identifies itself on
 * the network, rather than inventing a second name. "HiBy_Music" matches
 * that script's own fallback default when the file is missing. */
void get_device_name(char * out, size_t out_size) {
    snprintf(out, out_size, "HiBy_Music");
    FILE * f = fopen("/usr/resource/hostname", "r");
    if (f) {
        if (fgets(out, (int) out_size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}
