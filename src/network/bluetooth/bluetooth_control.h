#ifndef BLUETOOTH_CONTROL_H
#define BLUETOOTH_CONTROL_H

/* Bluetooth on every board. The same API whatever runs
 * underneath: BlueZ 4 or BlueZ 5 (detected on D-Bus), the board's platform
 * glue (R3 Pro: HiBy's sys_server and bt_codec; X1600: the board scripts),
 * and bluealsa or HiBy's ALSA plugin for the audio. See
 * bluetooth/bt_backend.h. Unless noted, calls block (D-Bus round trips,
 * bluealsactl, scripts): call them off the UI thread. */

#include <stdbool.h>
#include <stddef.h>

typedef bool (*bt_control_cancel_callback_t)(void * ctx);

typedef struct {
    char mac[18]; /* "XX:XX:XX:XX:XX:XX" + NUL */
    char name[64];
    bool paired;
    bool connected;
} bt_device_t;

/* Negotiated Bluetooth transport/decoded PCM information. This describes
 * BlueALSA's incoming A2DP stream, not the original media file on the
 * phone/PC (which may already have been resampled by the host stack). */
typedef struct {
    bool available;
    bool running;
    char codec[32];
    char pcm_format[32];
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bit_depth; /* 0 when pcm_format cannot be mapped safely */
} bt_dac_stream_info_t;

/* Non-blocking, thread-safe cached snapshot. */
void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out);

bool bt_control_is_powered(void);

/* Brings up the Bluetooth chip if it isn't already active (the platform's:
 * X1600 /usr/bin/bt_resume when there is no hci0; R3 Pro waits for the
 * stock bt_init), then makes sure the audio transport's daemon runs.
 * Call off the UI thread as this takes several seconds. */
bool bt_control_init_chip(void);
/* Turns BT back on at startup if it was on when Mellowtone last ran (the
 * R3 Pro platform; BlueZ 4 does not keep adapter power across boots).
 * No-op on other platforms. */
void bt_control_restore_boot_power(void);
/* false while the idle radio suspend switches BT off (see wifi_control.h).
 * No-op on platforms that keep no saved power choice. */
void bt_control_set_power_persist(bool persist);

/* What the active back end can do (a back end detail,
 * never a capability). */
enum {
    BT_BACKEND_VOLUME_SYNC = 1u << 0,   /* AVRCP absolute volume (BlueZ 5 + bluealsa; not BlueZ 4's AVRCP 1.3) */
    BT_BACKEND_SPEEX_RATE = 1u << 1,    /* bluealsa's speex resampler plugin */
    BT_BACKEND_RFCOMM_REMOTE = 1u << 2, /* remote-control RFCOMM profile (BlueZ 5 ProfileManager1) */
    BT_BACKEND_LIVE_RATE_CHANGE = 1u << 3, /* a new transport rate applies to the connected
                                              accessory now (bluealsa); otherwise on the next
                                              connection (HiBy's BlueZ 4) */
    BT_BACKEND_PLAYER_VOLUME = 1u << 4, /* the transport has no volume of its own: the player
                                           scales Bluetooth output itself (HiBy's plugin) */
};
/* Non-blocking (cached detection), safe on the UI and audio threads. */
unsigned int bt_control_backend_features(void);
/* True when `codec` (a bt_control_set_codec() name) is offered by this back
 * end: BlueZ 5 + bluealsa has "auto" and "sbc_xq"; the stock BlueZ 4 codec
 * layer (bt_codec) has no Auto and one SBC profile, but LDAC Mobile and
 * Adaptive. */
bool bt_control_codec_supported(const char * codec);
/* Reconcile the persisted outgoing codec with any daemon started by the
 * stock boot scripts. Call from a worker after Bluetooth startup readiness. */
bool bt_control_reconcile_source_settings(void);

/* Each blocks for about a second (a script, sys_server, or the adapter's
 * Powered property) -- call off the UI thread, same as everything else. */
void bt_control_enable(void);
void bt_control_disable(void);

/* True if a device is connected: on BlueZ 5 any paired device with a
 * connection, on BlueZ 4 any device whose A2DP sink link is up. Answered
 * from the stack's cached state (BlueZ 5: kept current by D-Bus signals)
 * or a few D-Bus reads (BlueZ 4); no process is started. */
bool bt_control_is_connected(void);

/* For the status-bar icon: 1 when any device is connected (BlueZ 5: any
 * device, paired or not -- it can briefly disagree with
 * bt_control_is_connected() during a pairing handshake; BlueZ 4: a paired
 * device's A2DP sink), 0 when none, -1 when the stack could not be read
 * this cycle -- keep the last known state on -1. */
int bt_control_any_paired_connected(void);

/* Like bt_control_is_connected(), but returns the full per-device
 * paired/connected breakdown instead of collapsing it into a single bool --
 * see the .c file for why (avoids paying for the same per-device query loop
 * twice per poll cycle). Returns how many entries were written into out[]
 * (capped at max_count, same convention as bt_control_scan()), or -1 if the
 * underlying query itself failed -- see the .c file's own comment on why
 * that's kept distinct from a genuine empty list. Blocking; call off the
 * UI thread. */
int bt_control_list_paired_states(bt_device_t * out, int max_count);

/* True if a real A2DP-source PCM (this device -> a connected headphone/
 * speaker) is currently registered with bluealsa -- a stronger, audio-
 * specific signal than bt_control_is_connected()/bt_control_list_paired_states()
 * (those report ANY paired device with an active connection, not
 * necessarily one that actually supports/negotiated A2DP audio). Forks a
 * process (`bluealsactl list-pcms`); call off the UI thread. */
bool bt_control_is_a2dp_source_connected(void);

/* Writes the connected A2DP-source accessory's Bluetooth MAC address (e.g.
 * "AA:BB:CC:DD:EE:FF") into out, extracted from the same bluealsa PCM path
 * bt_control_is_a2dp_source_connected() already checks for
 * ("/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink" -- underscores
 * swapped back to colons). Returns false (out left untouched) if nothing's
 * connected. Same subprocess cost as the other bt_control_* calls here;
 * call off the UI thread. */
bool bt_control_get_connected_device_mac(char * out, size_t out_size);

/* Writes the ACTUAL negotiated A2DP codec the connected accessory is
 * currently streaming with (e.g. "AAC", "LDAC", "SBC" -- whatever
 * `bluealsactl info` reports as "Selected codec") into out. This is the
 * real, live-negotiated codec, NOT current_settings.bt_codec (this app's
 * own PREFERRED codec setting, Settings > Bluetooth > Codec) -- those can
 * differ if the accessory doesn't support the preferred one and bluealsa
 * fell back to something else. Same subprocess cost as the other
 * bt_control_* calls here; call off the UI thread. Returns false (out left
 * untouched) if nothing's connected. */
bool bt_control_get_connected_device_codec(char * out, size_t out_size);

/* As above, and also the negotiated sampling frequency in Hz -- the codec is
 * only half of what tells you which link you actually got. Pass NULL for
 * out_sample_rate to skip it. Same subprocess cost; call off the UI thread. */
bool bt_control_get_connected_device_stream(char * out, size_t out_size, unsigned int * out_sample_rate);

/* The output link in one pass over the device list -- what the three calls
 * above report separately, each with its own device-list query. */
typedef struct {
    bool connected;           /* an A2DP sink (headphones, speaker) is connected for output */
    char mac[18];             /* its address; "" when not connected or unknown */
    char codec[32];           /* negotiated codec; "" when unknown */
    unsigned int sample_rate; /* negotiated rate in Hz; 0 when unknown */
} bt_a2dp_link_t;

/* Fills *out (always cleared first). Returns out->connected. Blocking;
 * call off the UI thread. */
bool bt_control_get_a2dp_link(bt_a2dp_link_t * out);

/* Known devices only, no inquiry: safe to call while audio is streaming. */
int bt_control_list_devices(bt_device_t * out, int max_count);

/* Blocking: a classic (BR/EDR) inquiry for about `seconds` (BlueZ 5:
 * StartDiscovery/StopDiscovery; R3 Pro: sys_server's BT:SCAN), then the
 * combined paired+discovered list with Paired/Connected state. Returns how
 * many devices were written into out[] (capped at max_count). */
int bt_control_scan(int seconds, bt_device_t * out, int max_count);

/* Pair (if not already) + trust + connect, in that order -- trusting first
 * means a future reconnect (e.g. after the device is turned off and back
 * on) won't need to go through this flow again, matching normal phone/OS
 * Bluetooth UX. The already-running NoInputNoOutput agent auto-accepts
 * "Just Works" pairing with no PIN prompt needed from this app. Blocking;
 * call off the UI thread. */
bool bt_control_connect(const char * mac);

bool bt_control_disconnect(const char * mac);

/* Unpairs and removes the saved device entry (Adapter1.RemoveDevice) --
 * disconnects it first if currently connected, as part of the same
 * command. Blocking; call off the UI thread. */
bool bt_control_forget(const char * mac);

/* Bounded, cancellation-aware reconnect of an already-paired trusted A2DP
 * headset. Never pairs or trusts. Call only from the reconnect worker; all
 * Bluetooth chip/control operations are serialized with manual actions. */
bool bt_control_reconnect_paired(const char * preferred_mac,
                                bt_control_cancel_callback_t cancel_cb,
                                void * cancel_ctx);

/* Output settings use the firmware's BlueALSA 5 service, with an
 * a2dp-source profile for this device sending audio OUT to
 * headphones/speakers. Changing either setting kills and relaunches that
 * service with the full combination implied by both current values (callers
 * pass both, not just the one that changed). This is a real running-service
 * restart, briefly interrupting in-progress Bluetooth audio; blocking, call
 * off the UI thread.
 *
 * dac_mode_enabled adds the a2dp-sink profile (so another device can
 * stream audio TO this one, using it as an external DAC) and, when
 * turning it on, also starts bluealsa-aplay to actually route that
 * incoming audio to the hardware output and makes the adapter
 * discoverable+pairable so a phone can find and connect to it as a sink
 * target; turning it off stops bluealsa-aplay and discoverability again.
 * volume_sync_enabled selects whether the app mirrors volume through the
 * BlueALSA 5 control API. */
bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled);

/* Select the outgoing BlueALSA 5 encoder preference. `codec` is one of
 * "auto"/"ldac_hq"/"ldac_sq"/"aptx"/"aac"/"sbc"/"sbc_xq". The active
 * daemon is reconciled separately by bt_control_reconcile_source_settings().
 * This only updates the in-memory preference; settings persistence belongs to
 * the caller. */
bool bt_control_set_codec(const char * codec);
/* The codec the system will negotiate (stock audio.conf DefaultCodec), as a
 * bt_control_set_codec() name; false where the back end has no system-wide
 * codec setting (BlueZ 5: the preference is ours). */
bool bt_control_get_system_codec(char * out, size_t out_size);

/* Restore the saved outgoing encoder preference without doing I/O. */
void bt_control_restore_codec_preference(const char * codec);

/* Per-device codec (the R3 Pro: one system-wide DefaultCodec, set before each
 * stream). False where the codec is the one global preference above. */
bool bt_control_codec_per_device(void);
typedef struct {
    unsigned int codecs;  /* BT_CODEC_* bits the device has; 0 = not known yet */
    char choice[8];       /* "auto", "ldac", "aptx", "aac", "sbc" */
    char resolved[16];    /* what Auto or the choice configures ("ldac_hq", "aac", ...); "" if unknown */
} bt_codec_device_view_t;
/* The device's codecs and choice, from memory and /tmp (no D-Bus, no
 * process, no card write): safe on the UI thread. */
bool bt_control_codec_device_view(const char * mac, bt_codec_device_view_t * out);
/* Remember choice ("auto", "ldac", ...) for the device and configure it if
 * it is the connected one. Blocking (card write, bt_codec): a worker. */
bool bt_control_set_device_codec(const char * mac, const char * choice);
/* The LDAC mode used whenever LDAC is chosen: "ldac_hq", "ldac_sq",
 * "ldac_mq", "ldac_abr". No I/O; bt_control_apply_codec() configures it. */
void bt_control_set_ldac_quality(const char * quality);
/* Configure the connected device's choice now, if it differs from the
 * system setting. Blocking: a worker. */
bool bt_control_apply_codec(void);

/* Selects the speex resampler for Bluetooth output instead of alsa-lib's
 * built-in linear one. On by default; roughly a point of CPU more. */
void bt_control_set_speexrate_enabled(bool enabled);
/* Experimental: also offer LDAC in DAC mode (aptX and aptX-HD always are).
 * No effect on firmware without the rebuilt decoder, whose stock one
 * crashes. */
void bt_control_set_dac_all_codecs(bool enabled);

/* Requested A2DP transport rate in Hz, or 0 for BlueALSA's own choice
 * (highest up to 48 kHz). 44100 is applied as a daemon argument, so it needs
 * a Bluetooth off/on cycle; any other rate is selected per connection. */
void bt_control_set_sample_rate(unsigned int rate);
/* Same, for a rate the user just chose: also forgets a rate the connected
 * accessory refused before, which bt_control_set_sample_rate() keeps so a
 * reconnect does not retry it. */
void bt_control_choose_sample_rate(unsigned int rate);

/* Cycles the connected accessory's link so a changed transport rate is
 * negotiated. The radio stays on; only the device link drops. Blocks for
 * several seconds, so call it from a worker thread. False when nothing is
 * connected, in which case the new rate applies on the next connection. */
bool bt_control_reconnect_for_rate_change(unsigned int rate);

/* Rates the connected accessory supports for the codec currently in use,
 * newest query each call. Returns the count written, 0 when nothing is
 * connected or the rates could not be read. */
int bt_control_get_available_rates(unsigned int * out, int max_count);
/* ALSA PCM used for outgoing Bluetooth audio; an explicit preference selects
 * its BlueALSA CODEC parameter, while auto leaves negotiation unchanged. */
const char * bt_control_get_playback_pcm(void);

/* Prepares an ALSA PCM pinned to the rate the A2DP transport already
 * negotiated and reports its name, so playback never asks BlueALSA to change
 * the transport's rate (which recreates it and drops the accessory). False
 * when the transport cannot be read; open bt_control_get_playback_pcm()
 * directly in that case. */
bool bt_control_prepare_playback_pcm(char * out, size_t out_size);

/* Keeps this app's own playback volume and a connected a2dp-source
 * accessory's (headphones/speaker this device streams TO) AVRCP volume in
 * sync, in both directions -- the headphones' own volume buttons update
 * this app's volume, and this app's own volume slider/hardware buttons
 * update whatever level the headphones show. See the .c file's own comment
 * above bt_control_source_volume_sync_start() for why this doesn't
 * introduce a second, compounding gain stage. Call start whenever
 * Bluetooth output is actually in use (mirror audio_set_bt_output()'s own
 * gating -- gui.c calls both together) and stop when it isn't; both are
 * idempotent. Serialize start/stop calls on one lifecycle worker; stop can
 * wait for child cleanup and must not run on the LVGL thread. */
void bt_control_source_volume_sync_start(void);
void bt_control_source_volume_sync_stop(void);
/* Nonblocking status snapshots, safe from the UI thread. */
bool bt_control_source_volume_sync_is_running(void);
bool bt_control_source_volume_sync_consume_percent(int * out_percent);

/* Fast disconnect detection for a2dp-source output PCM via `bluealsactl monitor`.
 * Confirms absence after a short grace interval: sample-rate/codec changes can
 * remove and re-create the same PCM without disconnecting the headphones.
 *
 * Same start/stop lifecycle convention as bt_control_source_volume_sync_start()/
 * _stop() just above -- start whenever Bluetooth output is actually in use,
 * stop when it isn't, both idempotent. bt_control_output_disconnect_consume()
 * is edge-triggered: true (and clears itself) the first poll after a real
 * removal remained absent through the grace interval, false otherwise.
 * A matching addition cancels a pending or unconsumed removal. EOF does not
 * confirm absence; periodic polling remains the fallback for a lost monitor.
 * The consumer performs no subprocess calls or waits for this grace period. */
#define BT_OUTPUT_RECONFIGURE_GRACE_MS 750
void bt_control_output_disconnect_watch_start(void);
void bt_control_output_disconnect_watch_stop(void);
bool bt_control_output_disconnect_watch_is_running(void);
bool bt_control_output_disconnect_consume(void);

#endif /* BLUETOOTH_CONTROL_H */
