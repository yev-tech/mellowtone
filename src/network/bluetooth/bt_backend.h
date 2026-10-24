#ifndef MT_BT_BACKEND_H
#define MT_BT_BACKEND_H

/* Bluetooth back ends: three small interfaces behind the public
 * bluetooth_control.h API (bluetooth_control.c dispatches).
 *
 * 1. Stack (bt_stack_ops_t): BlueZ 4 or BlueZ 5, detected at run time over
 *    D-Bus (bt_stack.c). Power state, devices, connections.
 * 2. Platform (struct board_bt_platform_ops, from the board descriptor): who
 *    powers the radio and owns the codec policy. R3 Pro: HiBy's sys_server
 *    and bt_codec (bt_platform_hiby.c); X1600: the board scripts
 *    (bt_platform_scripts.c); host simulator: none (the stack's Powered).
 * 3. Audio transport (bt_audio_ops_t, detected at run time): bluealsa
 *    (bt_audio_bluealsa.c) or HiBy's ALSA plugin fed by our BlueZ 4
 *    (bt_audio_hiby.c).
 *
 * None of these decides whether a capability exists; that is the
 * descriptor's .caps. What differs between back ends (live rate change,
 * absolute volume, ...) is reported by bt_control_backend_features().
 *
 * Locking (as each side did before the split): bt_chip_mutex serialises
 * chip bring-up, power and device actions on the X1600 / generic paths --
 * bluetooth_control.c takes it around the stack's actions and the output
 * settings, bt_platform_scripts.c around its scripts. Stack ops never take
 * it. Platform ops that replace an action (the HiBy sys_server requests)
 * are called without it and serialise with their own mutex. */

#include "bluetooth_control.h"
#include "board.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

extern pthread_mutex_t bt_chip_mutex;

/* ---- 1. stack ---------------------------------------------------------- */

typedef enum {
    BT_STACK_UNKNOWN = 0, /* nothing answered yet (bluetoothd not running) */
    BT_STACK_BLUEZ4,
    BT_STACK_BLUEZ5,
} bt_stack_kind_t;

typedef enum {
    BT_POWER_OFF = 0,
    BT_POWER_ON,
    BT_POWER_NO_ADAPTER, /* the stack answers but has no adapter */
    BT_POWER_NO_ANSWER,  /* the stack did not answer (not running, wedged, timed out) */
} bt_power_state_t;

#define BT_A2DP_SINK_UUID "0000110b-0000-1000-8000-00805f9b34fb"

typedef struct {
    bt_device_t dev; /* mac, name, paired, connected */
    bool trusted;
    bool blocked;
    bool audio_sink; /* advertises the A2DP sink UUID (headphones, speaker, car) */
} bt_stack_device_t;

typedef struct bt_stack_ops {
    const char * name;
    bt_stack_kind_t kind;
    unsigned int features; /* BT_BACKEND_* bits this stack provides */
    bt_power_state_t (*power_state)(int timeout_ms);
    /* NULL where the platform alone switches the radio. */
    bool (*set_powered)(bool on);
    bool (*set_discoverable)(bool on);
    /* Known devices; -1 when the stack could not be read. */
    int (*list_devices)(bt_stack_device_t * out, int max_count, bool paired_only);
    bool (*device_info)(const char * mac, bt_stack_device_t * out, int timeout_ms);
    /* The device audio output goes to, if any. */
    bool (*connected_device)(bt_device_t * out);
    bool (*is_connected)(void);
    int (*any_paired_connected)(void); /* 1 / 0 / -1 (unknown) */
    /* Device actions over D-Bus; NULL where the platform owns them. */
    bool (*discover)(int seconds); /* blocks for the inquiry window */
    bool (*pair)(const char * mac, int timeout_ms);
    bool (*set_trusted)(const char * mac, bool trusted);
    bool (*connect)(const char * mac, int timeout_ms);
    bool (*disconnect)(const char * mac, int timeout_ms);
    bool (*remove)(const char * mac);
} bt_stack_ops_t;

extern const bt_stack_ops_t bt_stack_bluez4;
extern const bt_stack_ops_t bt_stack_bluez5;

/* The detected stack (probing D-Bus while none is known yet; a positive
 * answer is kept). Never NULL: before detection, an "unknown" table whose
 * reads report nothing and whose actions fail. */
const bt_stack_ops_t * bt_stack(void);
/* Probe now if not known yet; the kind found (or BT_STACK_UNKNOWN). */
bt_stack_kind_t bt_stack_detect(void);
/* Cached result only: never touches the bus. */
bt_stack_kind_t bt_stack_kind_cached(void);
/* Tests: forget the detected stack. */
void bt_stack_reset_detection(void);

/* Any hciN registered with the kernel (/sys/class/bluetooth; what plain
 * `hciconfig` listed). */
bool bt_hci_adapter_present(void);

/* "XX:XX:XX:XX:XX:XX" (any case). */
bool bt_valid_mac(const char * mac);

/* ---- 2. platform --------------------------------------------------------- */

/* Codec and transport-rate policy: the platform's (R3 Pro: bt_codec and the
 * rate bluetoothd is asked for) or else the audio transport's (bluealsa's
 * daemon arguments and PCM names). */
typedef struct {
    void (*restore_preference)(const char * codec); /* no I/O */
    bool (*set_codec)(const char * codec);
    bool (*codec_supported)(const char * codec);
    bool (*get_system_codec)(char * out, size_t out_size);
    void (*set_sample_rate)(unsigned int rate);
    void (*choose_sample_rate)(unsigned int rate);
    /* Per-device choice (bt_codec_choice.h); NULL where the codec is one
     * global preference (bluealsa). */
    void (*set_ldac_quality)(const char * quality); /* no I/O */
    bool (*device_view)(const char * mac, bt_codec_device_view_t * out);
    bool (*set_device_codec)(const char * mac, const char * choice);
    /* Configure the choice for mac (NULL: the connected device) before a
     * stream is opened. */
    bool (*apply)(const char * mac);
} bt_codec_policy_t;

struct board_bt_platform_ops {
    const char * name;
    /* Bring the chip up; true when an adapter exists afterwards. */
    bool (*init_chip)(void);
    /* NULL: the stack's Powered property. */
    void (*power)(bool on);
    void (*restore_boot_power)(void);       /* NULL: nothing kept across boots */
    void (*set_power_persist)(bool persist); /* NULL: nothing kept */
    /* Before streams are set up (connect, output settings, reconcile): the
     * R3 Pro tells bluetoothd its preferred rate (/tmp/bt_prefer_rate). */
    void (*prepare)(void);
    const bt_codec_policy_t * codec_policy; /* NULL: the audio transport's */
    /* Device actions owned by a platform service (the R3 Pro's sys_server,
     * which serialises them with its own requests); NULL: the stack's. */
    int (*scan)(int seconds, bt_device_t * out, int max_count);
    bool (*connect)(const char * mac);
    bool (*disconnect)(const char * mac);
    bool (*forget)(const char * mac);
    bool (*reconnect_paired)(const char * preferred_mac, bt_control_cancel_callback_t cancel_cb,
                             void * cancel_ctx);
    /* Restart a stack that stopped answering (bluetoothd wedged); false
     * when it did nothing (rate limit). NULL: no watchdog on this platform. */
    bool (*recover_stack)(void);
};

/* Descriptor values (board.h declares them for board.c). */
extern const struct board_bt_platform_ops bt_platform_hiby_ops;
extern const struct board_bt_platform_ops bt_platform_scripts_ops;
extern const struct board_bt_platform_ops bt_platform_none_ops;

/* The R3 Pro's requested transport rate (0 = bluetoothd's choice), read by
 * the HiBy audio transport for its PCM before the first stream. */
unsigned int bt_platform_hiby_requested_rate(void);

/* ---- 3. audio transport -------------------------------------------------- */

typedef enum {
    BT_AUDIO_BLUEALSA = 0,
    BT_AUDIO_HIBY_PLUGIN,
} bt_audio_kind_t;

typedef struct bt_audio_ops {
    const char * name;
    bt_audio_kind_t kind;
    unsigned int features; /* BT_BACKEND_* bits this transport provides */
    const bt_codec_policy_t * codec_policy;
    bool (*ensure_running)(void); /* after chip bring-up */
    bool (*reconcile)(void);
    bool (*apply_output_settings)(bool dac_mode_enabled, bool volume_sync_enabled);
    void (*after_stack_restart)(void);
    void (*get_dac_stream_info)(bt_dac_stream_info_t * out);
    bool (*get_a2dp_link)(bt_a2dp_link_t * out);
    bool (*is_source_connected)(void);
    bool (*get_connected_mac)(char * out, size_t out_size);
    bool (*get_connected_stream)(char * out, size_t out_size, unsigned int * out_sample_rate);
    /* Bounded output-link check for the reconnect worker: 1 / 0 / -1. */
    int (*output_connected)(int timeout_ms);
    void (*note_requested_disconnect)(const char * mac);
    void (*set_speexrate_enabled)(bool enabled);
    void (*set_dac_all_codecs)(bool enabled);
    bool (*reconnect_for_rate_change)(unsigned int rate);
    int (*get_available_rates)(unsigned int * out, int max_count);
    const char * (*get_playback_pcm)(void);
    bool (*prepare_playback_pcm)(char * out, size_t out_size);
    void (*volume_sync_start)(void);
    void (*volume_sync_stop)(void);
    bool (*volume_sync_is_running)(void);
    bool (*volume_sync_consume_percent)(int * out_percent);
    void (*watch_start)(void);
    void (*watch_stop)(void);
    bool (*watch_is_running)(void);
    bool (*watch_consume)(void);
} bt_audio_ops_t;

extern const bt_audio_ops_t bt_audio_bluealsa;
extern const bt_audio_ops_t bt_audio_hiby;

/* The detected transport (cached; file and bus checks, never blocking long). */
const bt_audio_ops_t * bt_audio(void);
/* Tests: forget it, or use a fake transport (NULL: back to detection). */
void bt_audio_reset_detection(void);
void bt_audio_set_override(const bt_audio_ops_t * ops);

#endif /* MT_BT_BACKEND_H */
