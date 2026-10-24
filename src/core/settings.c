#include "board.h"
#include "board_config.h"
#include "mt_features.h"
#include "settings.h"
#include "debug_log.h"
player_settings_t current_settings;
#include "subprocess.h"
#include "storage_paths.h"
#include "utf8_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef HOST_BUILD
#include <sys/reboot.h>
#endif
#include <sys/stat.h>

#define SETTINGS_TMP_FILE_PATH SETTINGS_FILE_PATH ".tmp"

const int SCREEN_TIMEOUT_STEPS[SCREEN_TIMEOUT_STEP_COUNT] = { 15, 30, 60, 120, 300, 600, 1800 };
const int SCREEN_DIM_DELAY_STEPS[SCREEN_DIM_DELAY_STEP_COUNT] = { 5, 10, 15, 30, 60, 120, 300 };
const int IDLE_SHUTDOWN_STEPS[IDLE_SHUTDOWN_STEP_COUNT] = { 10, 15, 30, 60, 120 };
const int SLEEP_TIMER_STEPS[SLEEP_TIMER_STEP_COUNT] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180 };

bool settings_validate_subsonic_download_subfolder(const char *value, char *out, size_t out_size) {
    if (!value || !out || out_size == 0) return false;
    size_t length = strlen(value);
    if (length >= out_size) return false;
    if (length == 0) { out[0] = '\0'; return true; }

    char valid_utf8[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    utf8_truncate_safe_bounded(valid_utf8, sizeof(valid_utf8), value, length);
    if (strcmp(valid_utf8, value) != 0) return false;

    size_t component_length = 0;
    bool component_start = true;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '/') {
            if (component_length == 0) return false;
            component_length = 0;
            component_start = true;
            continue;
        }
        if (c == '\\' || (c < 0x80 && strchr(":*?\"<>|", c)) || c < 0x20 || c == 0x7F ||
            (component_start && c == '.')) return false;
        component_start = false;
        if (++component_length > 255) return false;
    }
    if (component_length == 0) return false;
    memcpy(out, value, length + 1);
    return true;
}

/* Snap hand-edited and legacy values onto the same presets used by the UI. */
static int nearest_step(int value, const int * steps, int count) {
    int best = steps[0];
    int best_diff = abs(value - best);
    for (int i = 1; i < count; i++) {
        int diff = abs(value - steps[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = steps[i];
        }
    }
    return best;
}

static bool valid_bt_output_mac(const char * value) {
    if (!value || strlen(value) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (value[i] != ':') return false;
        } else {
            const char c = value[i];
            const bool hex = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
    }
    return true;
}

static bool valid_remote_control_pin(const char * value) {
    if (!value) return false;
    size_t length = strlen(value);
    if (length < 4 || length > REMOTE_CONTROL_PIN_MAX_LENGTH) return false;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

static void set_defaults(player_settings_t * out) {
    out->setup_complete = false;
    out->setup_intro_played = false;
    out->setup_step = 0;
    out->volume = 1.0f;
    out->last_track[0] = '\0';
    out->last_position = 0.0;
    out->last_source_kind = 0;
    out->last_source_name[0] = '\0';
    out->resume_mode = 0;
    out->play_pause_button_mode = 0;
    out->accent_color = DEFAULT_ACCENT_COLOR;
    out->accent_dynamic = false;
    out->crossfade_enabled = false;
    out->gapless_enabled = true; /* on by default -- see settings.h */
    out->replaygain_mode = 1; /* Per Track -- preserves the old replaygain_enabled=true default */
    out->animation_scale = 100;
    out->inline_remote_enabled = true;
    out->lyrics_enabled = true;
    out->subsonic_url[0] = '\0';
    out->subsonic_username[0] = '\0';
    out->subsonic_password[0] = '\0';
    out->subsonic_verify_tls = true;
    out->subsonic_stream_quality = 0;
    out->subsonic_download_subfolder[0] = '\0';
    out->subsonic_download_layout = 0;
    out->bt_volume_sync_enabled = true;
    out->bt_dac_mode_enabled = false;
    out->bt_speexrate_enabled = true;
    out->bt_sample_rate = 0;
    out->bt_device_rate_count = 0;
    memset(out->bt_device_rates, 0, sizeof(out->bt_device_rates));
    snprintf(out->bt_codec, sizeof(out->bt_codec), "auto");
    /* LDAC Quality (909 kbps at 44.1 kHz): confirmed smooth on the R3 Pro. */
    snprintf(out->bt_ldac_quality, sizeof(out->bt_ldac_quality), "ldac_hq");
    out->bt_last_output_mac[0] = '\0';
    out->bt_hide_unnamed_devices = true;
    out->wifi_dac_mode_enabled = false;
    out->dlna_renderer_enabled = false;
    out->remote_control_enabled = false;
    out->remote_control_pin[0] = '\0';
    out->screen_timeout_enabled = true;
    out->screen_timeout_seconds = 30;
    out->screen_dimming_enabled = true;
    out->screen_dim_delay_seconds = 10;
    out->hide_player_topbar = false;
    out->screen_upside_down = false;
    out->led_indicator_enabled = true;
    out->tap_to_wake_enabled = true;
    out->hw_gain_level = 0;
    out->db_logging_enabled = false; /* opt-in developer diagnostic, off by default */
    out->dev_bt_dac_all_codecs = false;
    out->dev_covers_during_playback = false;
    out->dev_allow_suspend = true;
    out->dev_allow_suspend_default_migrated = false;
    out->charge_limiter_enabled = false; /* opt-in -- caps max charge voltage to 4.2V, a real behavior change the user should choose, not a default surprise */
    out->safe_charging_enabled = false; /* off means leave the PMIC charge-current setting untouched */
    out->dac_no_charge_enabled = false; /* opt-in: charging in DAC mode stays as it was */
    out->show_battery_percent = true; /* on by default -- matches every previous version's always-on behavior */
    out->show_artist_images = true;
    out->file_sort_mode = 0;
    out->album_sort_mode = 0;
    /* Defaults on: a device left screen-off with idle_shutdown_enabled=false
     * sits at full power indefinitely. Suspend-to-RAM (not a full poweroff)
     * avoids resetting the music queue. 10 minutes (IDLE_SHUTDOWN_STEPS min)
     * to reliably catch devices left idle. */
    out->idle_shutdown_enabled = true;
    out->idle_shutdown_minutes = 10;
    out->idle_suspend_enabled = true;
    out->idle_suspend_default_migrated = false; /* settings_load()'s own one-time migration sets this true */
    out->adb_enabled = false;
    out->play_mode = 0; /* PLAY_MODE_SEQUENTIAL */
    out->swipe_up_home_enabled = true;
    out->quick_drawer_volume_visible = true;
    out->startup_volume_fixed_enabled = true; /* matches stock's own default */
    out->startup_volume_fixed_percent = 20;
    out->sleep_timer_minutes = 15;
    out->timezone[0] = '\0';
    out->hostname[0] = '\0'; /* empty -- stock's own /usr/resource/hostname stays in effect */
    out->font_size_tier = 0;
    out->lyrics_font_size_tier = 2; /* Large -- see settings.h's own comment */
    out->eq_preamp_mode = 0;
    out->brightness_percent = 80;
    out->clock_24h = true; /* matches the app's original, only-ever clock format -- existing installs see no change */
    out->clock_automatic = true;
    out->clock_manual_epoch = 0;
    out->clock_system_reference = 0;
    out->custom_font[0] = '\0';
    snprintf(out->language, sizeof(out->language), "en");
}

void settings_subsonic_clear_active(player_settings_t * settings, const char * url) {
    if (!settings || !url) return;
    if (strcmp(settings->subsonic_url, url) == 0) {
        settings->subsonic_url[0] = '\0';
        settings->subsonic_username[0] = '\0';
        settings->subsonic_password[0] = '\0';
    }
}

/* Runs once a writable card appears after saves were refused. */
static void settings_flush_pending(void) {
    settings_save(&current_settings);
}

bool settings_load(player_settings_t * out) {
    mt_state_set_flush_hook(MT_PENDING_SETTINGS, settings_flush_pending);
    bool lyrics_small_applied = false;
    set_defaults(out);

    FILE * f = fopen(SETTINGS_FILE_PATH, "r");
    if (!f) return false;

    /* Pre-setup settings files belong to existing users. An explicit zero
     * survives early boot saves and restarts until setup is finished. */
    out->setup_complete = true;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * key = line;
        const char * value = eq + 1;

        if (strcmp(key, "setup_complete") == 0) {
            out->setup_complete = strcmp(value, "1") == 0;
        } else if (strcmp(key, "setup_intro_played") == 0) {
            out->setup_intro_played = strcmp(value, "1") == 0;
        } else if (strcmp(key, "setup_step") == 0) {
            out->setup_step = atoi(value);
        } else if (strcmp(key, "volume") == 0) {
            out->volume = (float) atof(value);
        } else if (strcmp(key, "last_track") == 0) {
            snprintf(out->last_track, sizeof(out->last_track), "%s", value);
        } else if (strcmp(key, "last_position") == 0) {
            out->last_position = atof(value);
        } else if (strcmp(key, "last_source_kind") == 0) {
            out->last_source_kind = atoi(value);
        } else if (strcmp(key, "last_source_name") == 0) {
            snprintf(out->last_source_name, sizeof(out->last_source_name), "%s", value);
        } else if (strcmp(key, "resume_mode") == 0) {
            out->resume_mode = atoi(value);
        } else if (strcmp(key, "play_pause_button_mode") == 0) {
            out->play_pause_button_mode = atoi(value);
        } else if (strcmp(key, "accent_color") == 0) {
            out->accent_color = (uint32_t) strtoul(value, NULL, 16);
        } else if (strcmp(key, "accent_dynamic") == 0) {
            out->accent_dynamic = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "crossfade") == 0) {
            out->crossfade_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "gapless") == 0) {
            out->gapless_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "replaygain_enabled") == 0) {
            /* Legacy on/off key, from before the Off/Per Track/Per Album
             * mode selector -- settings_save() only ever writes the new
             * "replaygain_mode" key going forward, so an old config file has
             * this one but never both; if both somehow appear, whichever
             * comes later in the file wins (plain sequential key parsing),
             * same as every other key here. */
            out->replaygain_mode = (strcmp(value, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "replaygain_mode") == 0) {
            out->replaygain_mode = atoi(value);
        } else if (strcmp(key, "animation_scale") == 0) {
            out->animation_scale = atoi(value);
        } else if (strcmp(key, "inline_remote_enabled") == 0) {
            out->inline_remote_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "lyrics_enabled") == 0) {
            out->lyrics_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "subsonic_url") == 0) {
            snprintf(out->subsonic_url, sizeof(out->subsonic_url), "%s", value);
        } else if (strcmp(key, "subsonic_username") == 0) {
            snprintf(out->subsonic_username, sizeof(out->subsonic_username), "%s", value);
        } else if (strcmp(key, "subsonic_password") == 0) {
            snprintf(out->subsonic_password, sizeof(out->subsonic_password), "%s", value);
        } else if (strcmp(key, "subsonic_verify_tls") == 0) {
            out->subsonic_verify_tls = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "subsonic_stream_quality") == 0) {
            int quality = atoi(value);
            out->subsonic_stream_quality = quality >= 0 && quality <= 3 ? quality : 0;
        } else if (strcmp(key, "subsonic_download_subfolder") == 0) {
            (void) settings_validate_subsonic_download_subfolder(value, out->subsonic_download_subfolder,
                sizeof(out->subsonic_download_subfolder));
        } else if (strcmp(key, "subsonic_download_layout") == 0) {
            int layout = atoi(value);
            out->subsonic_download_layout = layout == 1 ? 1 : 0;
        } else if (strcmp(key, "bt_volume_sync") == 0) {
            out->bt_volume_sync_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_device_rate") == 0) {
            /* "AA:BB:CC:DD:EE:FF,44100" */
            char * comma = strchr(value, ',');
            char * rate_end = NULL;
            unsigned long parsed = comma ? strtoul(comma + 1, &rate_end, 10) : 0;
            /* A malformed line is dropped rather than stored: a bad rate here
             * would be pushed straight at the Bluetooth daemon. */
            if (comma && out->bt_device_rate_count < BT_DEVICE_RATE_MAX &&
                    (size_t) (comma - value) == 17 && valid_bt_output_mac(value) &&
                    rate_end && rate_end != comma + 1 && *rate_end == '\0' &&
                    (parsed == 0 || (parsed >= 8000 && parsed <= 192000))) {
                int i = out->bt_device_rate_count++;
                memcpy(out->bt_device_rates[i].mac, value, 17);
                out->bt_device_rates[i].mac[17] = '\0';
                out->bt_device_rates[i].rate = (unsigned int) parsed;
            }
        } else if (strcmp(key, "bt_sample_rate") == 0) {
            out->bt_sample_rate = (unsigned int) strtoul(value, NULL, 10);
        } else if (strcmp(key, "bt_speexrate") == 0) {
            out->bt_speexrate_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_codec") == 0) {
            snprintf(out->bt_codec, sizeof(out->bt_codec), "%s", value);
        } else if (strcmp(key, "bt_ldac_quality") == 0) {
            snprintf(out->bt_ldac_quality, sizeof(out->bt_ldac_quality), "%s", value);
        } else if (strcmp(key, "bt_last_output_mac") == 0) {
            if (valid_bt_output_mac(value)) {
                snprintf(out->bt_last_output_mac, sizeof(out->bt_last_output_mac), "%s", value);
            } else {
                out->bt_last_output_mac[0] = '\0';
            }
        } else if (strcmp(key, "bt_hide_unnamed_devices") == 0) {
            out->bt_hide_unnamed_devices = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "wifi_dac_mode") == 0) {
            out->wifi_dac_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dlna_renderer_enabled") == 0) {
            out->dlna_renderer_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "remote_control_enabled") == 0) {
            out->remote_control_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "remote_control_pin") == 0) {
            if (valid_remote_control_pin(value))
                snprintf(out->remote_control_pin, sizeof(out->remote_control_pin), "%s", value);
        } else if (strcmp(key, "screen_timeout_enabled") == 0) {
            out->screen_timeout_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_timeout_seconds") == 0) {
            out->screen_timeout_seconds = atoi(value);
        } else if (strcmp(key, "screen_dimming_enabled") == 0) {
            out->screen_dimming_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_dim_delay_seconds") == 0) {
            out->screen_dim_delay_seconds = atoi(value);
        } else if (strcmp(key, "hide_player_topbar") == 0) {
            out->hide_player_topbar = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_upside_down") == 0) {
            out->screen_upside_down = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "led_indicator_enabled") == 0) {
            out->led_indicator_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "tap_to_wake_enabled") == 0) {
            out->tap_to_wake_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "hw_gain_level") == 0) {
            int level = atoi(value);
            out->hw_gain_level = (level >= 0 && level <= 2) ? level : 0;
        } else if (strcmp(key, "db_logging_enabled") == 0) {
            out->db_logging_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_bt_dac_all_codecs") == 0) {
            out->dev_bt_dac_all_codecs = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_covers_during_playback") == 0) {
            /* Not where the developer switch was removed (R3 Pro): covers
             * stay off during playback. */
            if (!BOARD.ui.no_cover_loading_option) out->dev_covers_during_playback = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_allow_suspend") == 0) {
            out->dev_allow_suspend = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_allow_suspend_default_migrated") == 0) {
            out->dev_allow_suspend_default_migrated = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "charge_limiter_enabled") == 0) {
            out->charge_limiter_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "safe_charging_enabled") == 0) {
            out->safe_charging_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dac_no_charge_enabled") == 0) {
            out->dac_no_charge_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "show_battery_percent") == 0) {
            out->show_battery_percent = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "file_sort_mode") == 0) {
            out->file_sort_mode = strcmp(value, "1") == 0 ? 1 : 0;
        } else if (strcmp(key, "album_sort_mode") == 0) {
            out->album_sort_mode = strcmp(value, "1") == 0 ? 1 : (strcmp(value, "2") == 0 ? 2 : 0);
        } else if (strcmp(key, "show_artist_images") == 0) {
            out->show_artist_images = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_enabled") == 0) {
            out->idle_shutdown_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_minutes") == 0) {
            out->idle_shutdown_minutes = atoi(value);
        } else if (strcmp(key, "idle_suspend_enabled") == 0) {
            out->idle_suspend_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_suspend_default_migrated") == 0) {
            out->idle_suspend_default_migrated = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "adb_enabled") == 0) {
            out->adb_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "usb_mode") == 0) {
            /* Older files: the USB mode was saved; only ADB (2) still is. */
            if (atoi(value) == 2) out->adb_enabled = true;
        } else if (strcmp(key, "play_mode") == 0) {
            out->play_mode = atoi(value);
        } else if (strcmp(key, "swipe_up_home_enabled") == 0) {
            out->swipe_up_home_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "quick_drawer_volume_visible") == 0) {
            out->quick_drawer_volume_visible = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_enabled") == 0) {
            out->startup_volume_fixed_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_percent") == 0) {
            out->startup_volume_fixed_percent = atoi(value);
        } else if (strcmp(key, "sleep_timer_minutes") == 0) {
            out->sleep_timer_minutes = atoi(value);
        } else if (strcmp(key, "timezone") == 0) {
            snprintf(out->timezone, sizeof(out->timezone), "%s", value);
        } else if (strcmp(key, "hostname") == 0) {
            snprintf(out->hostname, sizeof(out->hostname), "%s", value);
        } else if (strcmp(key, "font_size_tier") == 0) {
            out->font_size_tier = atoi(value);
        } else if (strcmp(key, "eq_preamp_mode") == 0) {
            out->eq_preamp_mode = atoi(value);
        } else if (strcmp(key, "lyrics_font_size_tier") == 0) {
            out->lyrics_font_size_tier = atoi(value);
        } else if (strcmp(key, "brightness_percent") == 0) {
            out->brightness_percent = atoi(value);
        } else if (strcmp(key, "clock_24h") == 0) {
            out->clock_24h = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "clock_automatic") == 0) {
            out->clock_automatic = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "clock_manual_epoch") == 0) {
            out->clock_manual_epoch = (int64_t) strtoll(value, NULL, 10);
        } else if (strcmp(key, "clock_system_reference") == 0) {
            out->clock_system_reference = (int64_t) strtoll(value, NULL, 10);
        } else if (strcmp(key, "custom_font") == 0) {
            if (!strchr(value, '/') && !strchr(value, '\\') && !strstr(value, "..")) {
                snprintf(out->custom_font, sizeof(out->custom_font), "%s", value);
            } else {
                out->custom_font[0] = '\0';
            }
        } else if (strcmp(key, "lyrics_small_applied") == 0) {
            lyrics_small_applied = true;
        } else if (strcmp(key, "language") == 0) {
            /* Short code made of letters, digits, '-' or '_'; anything else is English. */
            size_t n = strlen(value);
            bool ok = n > 0 && n < sizeof(out->language);
            for (size_t i = 0; ok && i < n; i++) {
                char c = value[i];
                ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            }
            snprintf(out->language, sizeof(out->language), "%s", ok ? value : "en");
        }
    }

    if (out->setup_step < 0 || out->setup_step > 5) out->setup_step = 0;
    if (out->play_mode < 0 || out->play_mode > 3) out->play_mode = 0;
    if (out->font_size_tier < 0 || out->font_size_tier > 2) out->font_size_tier = 0;
    if (out->eq_preamp_mode < 0 || out->eq_preamp_mode > 2) out->eq_preamp_mode = 0;
    if (out->lyrics_font_size_tier < 0 || out->lyrics_font_size_tier > 2) out->lyrics_font_size_tier = 2; /* Small/Medium/Large, see settings.h */
    if (out->replaygain_mode < 0 || out->replaygain_mode > 2) out->replaygain_mode = 1; /* Off/Per Track/Per Album only, see settings.h */
    if (out->animation_scale != 0 && out->animation_scale != 25 && out->animation_scale != 50 &&
        out->animation_scale != 75 && out->animation_scale != 100) out->animation_scale = 100;
    if (out->startup_volume_fixed_percent < 0 || out->startup_volume_fixed_percent > 100) out->startup_volume_fixed_percent = 20;
    if (out->brightness_percent < 0 || out->brightness_percent > 100) out->brightness_percent = 80;
    if (out->resume_mode < 0 || out->resume_mode > 2) out->resume_mode = 0;
    if (out->play_pause_button_mode < 0 || out->play_pause_button_mode > 2) out->play_pause_button_mode = 0;

    /* One-time forced migration for installs that already have a
     * settings.txt predating idle_suspend_enabled's default flip (see
     * set_defaults() above) -- settings_save() always writes every field,
     * so an existing file already pins idle_shutdown_enabled/
     * idle_suspend_enabled/idle_shutdown_minutes back to their old
     * false/false/30 values, silently defeating the new default for every
     * existing user rather than just new installs. Same
     * marker-key-in-the-file idiom as playlist_files.c's own
     * PLAYLIST_MIGRATION_MARKER_NAME, just inline here instead of a
     * separate marker file since settings.txt is already a single
     * key=value blob rewritten in full on every save. Runs once: after
     * this, idle_suspend_default_migrated=1 is itself persisted on the next
     * settings_save(), so a user who deliberately turns any of these back
     * off afterward stays off. */
    if (!out->idle_suspend_default_migrated) {
        out->idle_shutdown_enabled = true;
        out->idle_shutdown_minutes = 10;
        out->idle_suspend_enabled = true;
        out->idle_suspend_default_migrated = true;
    }
    /* Suspend on the R3 Pro left testing on by default; same once-only rule. */
    if (!out->dev_allow_suspend_default_migrated) {
        out->dev_allow_suspend = true;
        out->dev_allow_suspend_default_migrated = true;
    }

    out->screen_timeout_seconds = nearest_step(out->screen_timeout_seconds, SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT);
    out->screen_dim_delay_seconds = nearest_step(out->screen_dim_delay_seconds, SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT);
    out->idle_shutdown_minutes = nearest_step(out->idle_shutdown_minutes, IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT);
    out->sleep_timer_minutes = nearest_step(out->sleep_timer_minutes, SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT);

    /* Small lyrics fit the 360 px panel and the Now panel; applied once. */
    if (BOARD.ui.lyrics_small_default && !lyrics_small_applied) out->lyrics_font_size_tier = 0;
    /* Capabilities and features this board doesn't offer (their rows are
     * hidden): a value saved by an earlier build must not switch them on --
     * BT/AirPlay receiver modes in particular block local playback. */
    if (!board_has(CAP_BT_RECEIVER)) {
        out->bt_dac_mode_enabled = false;
        out->dev_bt_dac_all_codecs = false;
    }
    if (!MT_HAS_FEATURE(AIRPLAY)) out->wifi_dac_mode_enabled = false;
    if (!MT_HAS_FEATURE(DLNA_RENDERER)) out->dlna_renderer_enabled = false;
    if (!MT_HAS_FEATURE(REMOTE)) out->remote_control_enabled = false;
    if (!MT_HAS_FEATURE(I18N)) snprintf(out->language, sizeof(out->language), "en");
    if (!board_has(CAP_INLINE_REMOTE)) out->inline_remote_enabled = false;
    if (!board_has(CAP_CHARGE_CONTROL)) {
        out->charge_limiter_enabled = false;
        out->safe_charging_enabled = false;
    }
    /* "Don't charge in USB DAC mode": the charger switch (CAP_CHARGE_CONTROL)
     * or the input-current policy's no-charge row (R3 Pro). */
    bool dac_no_charge_possible =
        board_has(CAP_CHARGE_CONTROL) ||
        (board_has(CAP_INPUT_CURRENT) && BOARD.power.input_current && BOARD.power.input_current->has_dac_no_charge);
    if (!dac_no_charge_possible || !board_has(CAP_USB_DAC) || board_impl_missing(CAP_USB_DAC))
        out->dac_no_charge_enabled = false;

    fclose(f);
    return true;
}

/* Flushes directory metadata changes so the rename survives an unclean
 * shutdown (atomic durable replace: write tmp -> fsync tmp -> rename -> fsync dir). */
static void fsync_settings_dir(void) {
    int dir_fd = open(MT_CONFIG_DIR, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) return;
    fsync(dir_fd);
    close(dir_fd);
}

/* A save that did not reach the card: the settings file there is left as it
 * was, the partial temp file goes, and the settings are kept in memory as
 * unsaved (written again with the next save, or when a card appears). */
static void settings_write_failed(bool remove_tmp) {
    fprintf(stderr, "settings: could not write %s: %s\n", SETTINGS_FILE_PATH, strerror(errno));
    if (remove_tmp) unlink(SETTINGS_TMP_FILE_PATH);
    mt_state_mark_unsaved(MT_PENDING_SETTINGS);
}

/* Set by settings_factory_reset(): no more saves before the reboot. */
static atomic_bool settings_frozen;

static void settings_write_file(const player_settings_t * settings) {
    if (atomic_load(&settings_frozen)) return;
    char subsonic_download_subfolder[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX] = "";
    (void) settings_validate_subsonic_download_subfolder(settings->subsonic_download_subfolder,
        subsonic_download_subfolder, sizeof(subsonic_download_subfolder));
    DBG_LOG("settings_save: called (idle_suspend_enabled=%d)\n", settings->idle_suspend_enabled ? 1 : 0);
    /* No writable card (or a card whose own settings this session must not
     * overwrite): keep the settings in memory, write them once a card is
     * back (storage_paths.c). */
    if (!mt_state_prepare_write(SETTINGS_FILE_PATH)) {
        mt_state_mark_unsaved(MT_PENDING_SETTINGS);
        return;
    }
    /* This file contains the RC PIN and Subsonic credentials. Create or
     * rewrite the atomic temp file as owner-only even when an older temp
     * file exists with broader permissions. */
    int settings_fd = open(SETTINGS_TMP_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (settings_fd < 0) {
        settings_write_failed(false);
        return;
    }
    if (fchmod(settings_fd, 0600) != 0) {
        close(settings_fd);
        settings_write_failed(true);
        return;
    }
    FILE * f = fdopen(settings_fd, "w");
    if (!f) {
        close(settings_fd);
        settings_write_failed(true);
        return;
    }

    fprintf(f, "volume=%.3f\n", (double) settings->volume);
    fprintf(f, "last_track=%s\n", settings->last_track);
    fprintf(f, "last_position=%.3f\n", settings->last_position);
    fprintf(f, "last_source_kind=%d\n", settings->last_source_kind);
    fprintf(f, "last_source_name=%s\n", settings->last_source_name);
    fprintf(f, "resume_mode=%d\n", settings->resume_mode);
    fprintf(f, "play_pause_button_mode=%d\n", settings->play_pause_button_mode);
    fprintf(f, "accent_color=%06X\n", (unsigned int) (settings->accent_color & 0xFFFFFF));
    fprintf(f, "accent_dynamic=%d\n", settings->accent_dynamic ? 1 : 0);
    fprintf(f, "crossfade=%d\n", settings->crossfade_enabled ? 1 : 0);
    fprintf(f, "gapless=%d\n", settings->gapless_enabled ? 1 : 0);
    fprintf(f, "replaygain_mode=%d\n", settings->replaygain_mode);
    fprintf(f, "animation_scale=%d\n", settings->animation_scale);
    fprintf(f, "inline_remote_enabled=%d\n", settings->inline_remote_enabled ? 1 : 0);
    fprintf(f, "lyrics_enabled=%d\n", settings->lyrics_enabled ? 1 : 0);
    fprintf(f, "subsonic_url=%s\n", settings->subsonic_url);
    fprintf(f, "subsonic_username=%s\n", settings->subsonic_username);
    fprintf(f, "subsonic_password=%s\n", settings->subsonic_password);
    fprintf(f, "subsonic_verify_tls=%d\n", settings->subsonic_verify_tls ? 1 : 0);
    fprintf(f, "subsonic_stream_quality=%d\n", settings->subsonic_stream_quality);
    fprintf(f, "subsonic_download_subfolder=%s\n", subsonic_download_subfolder);
    fprintf(f, "subsonic_download_layout=%d\n", settings->subsonic_download_layout == 1 ? 1 : 0);
    fprintf(f, "bt_volume_sync=%d\n", settings->bt_volume_sync_enabled ? 1 : 0);
    fprintf(f, "bt_speexrate=%d\n", settings->bt_speexrate_enabled ? 1 : 0);
    fprintf(f, "bt_sample_rate=%u\n", settings->bt_sample_rate);
    for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
        fprintf(f, "bt_device_rate=%s,%u\n", settings->bt_device_rates[i].mac,
                settings->bt_device_rates[i].rate);
    fprintf(f, "bt_codec=%s\n", settings->bt_codec);
    fprintf(f, "bt_ldac_quality=%s\n", settings->bt_ldac_quality);
    fprintf(f, "bt_last_output_mac=%s\n", settings->bt_last_output_mac);
    fprintf(f, "bt_hide_unnamed_devices=%d\n", settings->bt_hide_unnamed_devices ? 1 : 0);
    fprintf(f, "wifi_dac_mode=%d\n", settings->wifi_dac_mode_enabled ? 1 : 0);
    fprintf(f, "dlna_renderer_enabled=%d\n", settings->dlna_renderer_enabled ? 1 : 0);
    fprintf(f, "remote_control_enabled=%d\n", settings->remote_control_enabled ? 1 : 0);
    fprintf(f, "remote_control_pin=%s\n", settings->remote_control_pin);
    fprintf(f, "screen_timeout_enabled=%d\n", settings->screen_timeout_enabled ? 1 : 0);
    fprintf(f, "screen_timeout_seconds=%d\n", settings->screen_timeout_seconds);
    fprintf(f, "screen_dimming_enabled=%d\n", settings->screen_dimming_enabled ? 1 : 0);
    fprintf(f, "screen_dim_delay_seconds=%d\n", settings->screen_dim_delay_seconds);
    fprintf(f, "hide_player_topbar=%d\n", settings->hide_player_topbar ? 1 : 0);
    fprintf(f, "screen_upside_down=%d\n", settings->screen_upside_down ? 1 : 0);
    fprintf(f, "led_indicator_enabled=%d\n", settings->led_indicator_enabled ? 1 : 0);
    fprintf(f, "tap_to_wake_enabled=%d\n", settings->tap_to_wake_enabled ? 1 : 0);
    fprintf(f, "hw_gain_level=%d\n", settings->hw_gain_level);
    fprintf(f, "db_logging_enabled=%d\n", settings->db_logging_enabled ? 1 : 0);
    fprintf(f, "dev_bt_dac_all_codecs=%d\n", settings->dev_bt_dac_all_codecs ? 1 : 0);
    fprintf(f, "dev_covers_during_playback=%d\n", settings->dev_covers_during_playback ? 1 : 0);
    fprintf(f, "dev_allow_suspend=%d\n", settings->dev_allow_suspend ? 1 : 0);
    fprintf(f, "dev_allow_suspend_default_migrated=%d\n", settings->dev_allow_suspend_default_migrated ? 1 : 0);
    fprintf(f, "charge_limiter_enabled=%d\n", settings->charge_limiter_enabled ? 1 : 0);
    fprintf(f, "safe_charging_enabled=%d\n", settings->safe_charging_enabled ? 1 : 0);
    fprintf(f, "dac_no_charge_enabled=%d\n", settings->dac_no_charge_enabled ? 1 : 0);
    fprintf(f, "show_battery_percent=%d\n", settings->show_battery_percent ? 1 : 0);
    fprintf(f, "show_artist_images=%d\n", settings->show_artist_images ? 1 : 0);
    fprintf(f, "file_sort_mode=%d\n", settings->file_sort_mode == 1 ? 1 : 0);
    fprintf(f, "album_sort_mode=%d\n", settings->album_sort_mode >= 0 && settings->album_sort_mode <= 2 ? settings->album_sort_mode : 0);
    fprintf(f, "idle_shutdown_enabled=%d\n", settings->idle_shutdown_enabled ? 1 : 0);
    fprintf(f, "idle_shutdown_minutes=%d\n", settings->idle_shutdown_minutes);
    fprintf(f, "idle_suspend_enabled=%d\n", settings->idle_suspend_enabled ? 1 : 0);
    fprintf(f, "idle_suspend_default_migrated=%d\n", settings->idle_suspend_default_migrated ? 1 : 0);
    fprintf(f, "adb_enabled=%d\n", settings->adb_enabled ? 1 : 0);
    fprintf(f, "play_mode=%d\n", settings->play_mode);
    fprintf(f, "swipe_up_home_enabled=%d\n", settings->swipe_up_home_enabled ? 1 : 0);
    fprintf(f, "quick_drawer_volume_visible=%d\n", settings->quick_drawer_volume_visible ? 1 : 0);
    fprintf(f, "startup_volume_fixed_enabled=%d\n", settings->startup_volume_fixed_enabled ? 1 : 0);
    fprintf(f, "startup_volume_fixed_percent=%d\n", settings->startup_volume_fixed_percent);
    fprintf(f, "sleep_timer_minutes=%d\n", settings->sleep_timer_minutes);
    fprintf(f, "timezone=%s\n", settings->timezone);
    fprintf(f, "hostname=%s\n", settings->hostname);
    fprintf(f, "font_size_tier=%d\n", settings->font_size_tier);
    fprintf(f, "lyrics_font_size_tier=%d\n", settings->lyrics_font_size_tier);
    fprintf(f, "eq_preamp_mode=%d\n", settings->eq_preamp_mode);
    fprintf(f, "brightness_percent=%d\n", settings->brightness_percent);
    fprintf(f, "clock_24h=%d\n", settings->clock_24h ? 1 : 0);
    fprintf(f, "clock_automatic=%d\n", settings->clock_automatic ? 1 : 0);
    fprintf(f, "clock_manual_epoch=%lld\n", (long long) settings->clock_manual_epoch);
    fprintf(f, "clock_system_reference=%lld\n", (long long) settings->clock_system_reference);
    fprintf(f, "custom_font=%s\n", settings->custom_font);
    if (BOARD.ui.lyrics_small_default) fprintf(f, "lyrics_small_applied=1\n");
    fprintf(f, "language=%s\n", settings->language[0] ? settings->language : "en");
    fprintf(f, "setup_complete=%d\n", settings->setup_complete ? 1 : 0);
    fprintf(f, "setup_step=%d\n", settings->setup_step);
    fprintf(f, "setup_intro_played=%d\n", settings->setup_intro_played ? 1 : 0);

    /* Only a complete, synced file replaces the settings: a short write
     * (full card, I/O error) renamed over them would lose every setting
     * past the cut, and with it setup_complete and the credentials. */
    bool ok = !ferror(f);
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        settings_write_failed(true);
        return;
    }
    if (rename(SETTINGS_TMP_FILE_PATH, SETTINGS_FILE_PATH) != 0) {
        settings_write_failed(true);
        return;
    }
    chmod(SETTINGS_FILE_PATH, 0600);
    fsync_settings_dir();
}

/* Both synchronous callers and the coalescing worker share the same tmp
 * file, so writes must be serialized. The generation prevents an older
 * queued snapshot from landing after a newer synchronous save. */
static pthread_mutex_t settings_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t settings_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t settings_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t settings_worker_once = PTHREAD_ONCE_INIT;
static player_settings_t settings_queued_snapshot;
static uint64_t settings_save_generation = 0;
static uint64_t settings_queued_generation = 0;
static bool settings_save_pending = false;
static bool settings_worker_ready = false;

/* Rate remembered for one accessory, falling back to the global choice when
 * that accessory has never been given one. */
unsigned int settings_bt_rate_for(const player_settings_t * settings, const char * mac) {
    if (settings && mac && mac[0])
        for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
            if (strcasecmp(settings->bt_device_rates[i].mac, mac) == 0)
                return settings->bt_device_rates[i].rate;
    return settings ? settings->bt_sample_rate : 0;
}

void settings_bt_set_rate_for(player_settings_t * settings, const char * mac, unsigned int rate) {
    if (!settings || !mac || !mac[0]) return;
    for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
        if (strcasecmp(settings->bt_device_rates[i].mac, mac) == 0) {
            settings->bt_device_rates[i].rate = rate;
            return;
        }
    if (settings->bt_device_rate_count < BT_DEVICE_RATE_MAX) {
        int i = settings->bt_device_rate_count++;
        snprintf(settings->bt_device_rates[i].mac, sizeof(settings->bt_device_rates[i].mac), "%s", mac);
        settings->bt_device_rates[i].rate = rate;
        return;
    }
    /* Full: drop the oldest so a newly used accessory is still remembered. */
    memmove(&settings->bt_device_rates[0], &settings->bt_device_rates[1],
            sizeof(settings->bt_device_rates[0]) * (BT_DEVICE_RATE_MAX - 1));
    int last = BT_DEVICE_RATE_MAX - 1;
    snprintf(settings->bt_device_rates[last].mac, sizeof(settings->bt_device_rates[last].mac), "%s", mac);
    settings->bt_device_rates[last].rate = rate;
}

void settings_save(const player_settings_t * settings) {
    if (!settings) return;
    pthread_mutex_lock(&settings_write_mutex);
    pthread_mutex_lock(&settings_queue_mutex);
    settings_save_generation++;
    pthread_mutex_unlock(&settings_queue_mutex);
    settings_write_file(settings);
    pthread_mutex_unlock(&settings_write_mutex);
}

static void * settings_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&settings_queue_mutex);
        while (!settings_save_pending)
            pthread_cond_wait(&settings_queue_cond, &settings_queue_mutex);
        player_settings_t snapshot = settings_queued_snapshot;
        uint64_t generation = settings_queued_generation;
        settings_save_pending = false;
        pthread_mutex_unlock(&settings_queue_mutex);

        pthread_mutex_lock(&settings_write_mutex);
        pthread_mutex_lock(&settings_queue_mutex);
        bool current = generation == settings_save_generation;
        pthread_mutex_unlock(&settings_queue_mutex);
        if (current) settings_write_file(&snapshot);
        pthread_mutex_unlock(&settings_write_mutex);
    }
    return NULL;
}

static void settings_start_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, settings_worker_main, NULL) == 0) {
        pthread_detach(thread);
        settings_worker_ready = true;
    }
}

void settings_save_async(const player_settings_t * settings) {
    if (!settings) return;
    pthread_once(&settings_worker_once, settings_start_worker);
    if (!settings_worker_ready) {
        settings_save(settings);
        return;
    }

    pthread_mutex_lock(&settings_queue_mutex);
    settings_queued_snapshot = *settings;
    settings_queued_generation = ++settings_save_generation;
    settings_save_pending = true;
    pthread_cond_signal(&settings_queue_cond);
    pthread_mutex_unlock(&settings_queue_mutex);
}

void settings_factory_reset(void) {
    DBG_LOG("settings_factory_reset: called\n");
    /* Mellowtone reset: config/ moves to backup/reset-<date>/ on
     * the card; library, caches, logs and music stay, and nothing in flash
     * is touched. No writer may recreate config/ between the move and the
     * reboot: wait for a save in progress, then stop all saves. */
    pthread_mutex_lock(&settings_write_mutex);
    atomic_store(&settings_frozen, true);
    pthread_mutex_unlock(&settings_write_mutex);
    if (!mt_state_reset_config()) DBG_LOG("settings_factory_reset: could not move %s\n", MT_CONFIG_DIR);

#ifndef HOST_BUILD
    sync();
    if (BOARD.system.reboot_via_init) {
        /* Straight to busybox reboot (clean unmount): idle_shutdown_reboot_now()
         * would first flush the play queue. exec ends every thread, so no
         * queued save can land afterwards. */
        execl("/sbin/reboot", "reboot", (char *) NULL);
    }
    /* Otherwise not execl("/sbin/reboot", ...): on X1600 mellowtone-launcher
     * treats a clean (status 0) exit of the player as "power off", and
     * /sbin/reboot exits 0 once it hands off to init, so the power-off would
     * win over the real reboot. Calling reboot(2) directly restarts the
     * kernel immediately from within this process. */
    reboot(RB_AUTOBOOT);
    for (;;) pause();
#endif
}
