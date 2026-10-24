/* bluetooth_control.h on every board: the public API over the
 * three back ends of bt_backend.h -- the stack detected on D-Bus, the
 * platform from the board descriptor, and the detected audio transport.
 *
 * Everything that differs between boards lives in a back end; what is
 * here is ordering and locking, plus two generic algorithms that used to
 * run bluetoothctl: connect (pair, trust, connect) and the bounded
 * reconnect of a paired headset. The R3 Pro's platform replaces both with
 * its sys_server requests, as before. */
#include "bt_backend.h"
#include "bt_dbus.h"
#include "board.h"
#include "debug_log.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- back-end choice --------------------------------------------------------- */

static const struct board_bt_platform_ops * platform(void) {
#ifdef HOST_BUILD
    /* The simulator models a board but runs on a PC: no board scripts. */
    return &bt_platform_none_ops;
#else
    return BOARD.bt_platform ? BOARD.bt_platform : &bt_platform_none_ops;
#endif
}

/* bluealsa when its service is on the bus or its ALSA plugin is installed;
 * HiBy's plugin when that is installed instead; bluealsa otherwise (what
 * every BlueZ 5 system uses). Files and the bus do not change while the
 * player runs, so the answer is kept. */
static const char * const alsa_lib_dirs[] = {
    "/usr/lib/alsa-lib", "/usr/lib/x86_64-linux-gnu/alsa-lib", "/usr/lib/aarch64-linux-gnu/alsa-lib",
    "/usr/lib64/alsa-lib", NULL,
};

static bool alsa_plugin_installed(const char * name) {
    for (int i = 0; alsa_lib_dirs[i]; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/libasound_module_pcm_%s.so", alsa_lib_dirs[i], name);
        if (access(path, F_OK) == 0) return true;
    }
    return false;
}

static atomic_int audio_kind = -1;
static const bt_audio_ops_t * audio_override;

void bt_audio_set_override(const bt_audio_ops_t * ops) {
    audio_override = ops;
}

/* The answer from the plugin files alone (no bus): never blocks. */
static const bt_audio_ops_t * guess_audio_from_files(void) {
    if (!alsa_plugin_installed("bluealsa") && alsa_plugin_installed("bluetooth")) return &bt_audio_hiby;
    return &bt_audio_bluealsa;
}

static const bt_audio_ops_t * detect_audio(void) {
    const bt_audio_ops_t * ops = guess_audio_from_files();
    if (ops == &bt_audio_hiby) {
        /* Only ask the bus when the files point at HiBy's plugin. */
        bool bluealsa = false;
        DBusConnection * c = bt_dbus_open();
        if (c) {
            bluealsa = bt_dbus_name_has_owner(c, "org.bluealsa");
            dbus_connection_close(c);
            dbus_connection_unref(c);
        }
        if (bluealsa) ops = &bt_audio_bluealsa;
    }
    return ops;
}

const bt_audio_ops_t * bt_audio(void) {
    if (audio_override) return audio_override;
    int kind = atomic_load(&audio_kind);
    if (kind < 0) {
        const bt_audio_ops_t * ops = detect_audio();
        DBG_LOG("bt_control: audio transport %s\n", ops->name);
        atomic_store(&audio_kind, (int) ops->kind);
        return ops;
    }
    return kind == BT_AUDIO_HIBY_PLUGIN ? &bt_audio_hiby : &bt_audio_bluealsa;
}

static atomic_bool audio_detect_running = false;

static void * detect_audio_thread(void * arg) {
    (void) arg;
    (void) bt_audio();
    atomic_store(&audio_detect_running, false);
    return NULL;
}

/* The transport without blocking: the detected one, or a guess from the
 * plugin files while the full detection runs on a worker. */
static const bt_audio_ops_t * bt_audio_nonblocking(void) {
    if (audio_override) return audio_override;
    int kind = atomic_load(&audio_kind);
    if (kind >= 0) return kind == BT_AUDIO_HIBY_PLUGIN ? &bt_audio_hiby : &bt_audio_bluealsa;
    if (!atomic_exchange(&audio_detect_running, true)) {
        pthread_t t;
        if (pthread_create(&t, NULL, detect_audio_thread, NULL) == 0) pthread_detach(t);
        else atomic_store(&audio_detect_running, false);
    }
    return guess_audio_from_files();
}

void bt_audio_reset_detection(void) {
    atomic_store(&audio_kind, -1);
}

static const bt_codec_policy_t * codec_policy(void) {
    const bt_codec_policy_t * p = platform()->codec_policy;
    return p ? p : bt_audio()->codec_policy;
}

static void prepare(void) {
    if (platform()->prepare) platform()->prepare();
}

/* Never blocks: the stack bits come from the stack found so far. Before
 * bluetoothd has answered, the stack that goes with the audio transport is
 * assumed (HiBy's plugin only exists next to BlueZ 4). */
unsigned int bt_control_backend_features(void) {
    const bt_audio_ops_t * audio = bt_audio_nonblocking();
    bt_stack_kind_t kind = bt_stack_kind_cached();
    if (kind == BT_STACK_UNKNOWN) kind = audio->kind == BT_AUDIO_HIBY_PLUGIN ? BT_STACK_BLUEZ4 : BT_STACK_BLUEZ5;
    unsigned int stack = kind == BT_STACK_BLUEZ5 ? bt_stack_bluez5.features : bt_stack_bluez4.features;
    /* Volume sync needs both: AVRCP absolute volume and a transport volume. */
    unsigned int both = BT_BACKEND_VOLUME_SYNC;
    return ((stack | audio->features) & ~both) | (stack & audio->features & both);
}

/* ---- power --------------------------------------------------------------------- */

/* The wedge watchdog (platforms that can restart bluetoothd): one missed
 * answer is not proof of a wedge -- bluealsa can legitimately keep
 * bluetoothd busy for ~15 s while a phone opens a transport and never
 * starts it -- so several consecutive ones are needed (polled every ~5 s).
 * No adapter while hci0 is up is a clean, unambiguous answer and recovers
 * at once, after a grace period for boot-time bring-up. */
#define TIMEOUT_RECOVERY_THRESHOLD 4
#define BT_BOOT_RACE_GRACE_MS 15000
#define POWER_QUERY_TIMEOUT_MS 15000

static pthread_mutex_t bt_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void recover_stack(void) {
    if (platform()->recover_stack && platform()->recover_stack()) {
        /* bluealsa comes back in its default profile: restore DAC mode etc. */
        bt_audio()->after_stack_restart();
    }
}

static bool is_powered_watched(void) {
    static int consecutive_timeouts = 0;
    static bool first_poll_seen = false;
    static uint32_t first_poll_tick = 0;

    if (!first_poll_seen) {
        first_poll_seen = true;
        first_poll_tick = monotonic_ms();
    }
    if (!bt_hci_adapter_present()) {
        DBG_LOG("bt_control: bt_control_is_powered: no adapter present\n");
        return false;
    }
    bt_power_state_t state = bt_stack()->power_state(POWER_QUERY_TIMEOUT_MS);
    if (state == BT_POWER_NO_ANSWER) {
        consecutive_timeouts++;
        DBG_LOG("bt_control: bt_control_is_powered: no answer from the stack (%d/%d)\n", consecutive_timeouts,
                TIMEOUT_RECOVERY_THRESHOLD);
        if (consecutive_timeouts >= TIMEOUT_RECOVERY_THRESHOLD) {
            recover_stack();
            consecutive_timeouts = 0;
        }
        return false;
    }
    consecutive_timeouts = 0; /* any actual reply, even "not powered", means it isn't stuck */
    if (state == BT_POWER_ON) return true;
    if (state == BT_POWER_NO_ADAPTER) {
        uint32_t since_first_poll = monotonic_ms() - first_poll_tick;
        if (since_first_poll < BT_BOOT_RACE_GRACE_MS) {
            DBG_LOG("bt_control: bt_control_is_powered: no adapter %ums after first poll -- "
                    "within boot grace window, treating as still starting up, not a wedge\n",
                    (unsigned) since_first_poll);
        } else {
            recover_stack();
        }
    }
    return false;
}

bool bt_control_is_powered(void) {
    pthread_mutex_lock(&bt_status_mutex);
    bool result = platform()->recover_stack ? is_powered_watched()
                                            : bt_stack()->power_state(POWER_QUERY_TIMEOUT_MS) == BT_POWER_ON;
    pthread_mutex_unlock(&bt_status_mutex);
    return result;
}

bool bt_control_init_chip(void) {
    bool ok = platform()->init_chip();
    (void) bt_audio()->ensure_running();
    return ok;
}

void bt_control_restore_boot_power(void) {
    if (platform()->restore_boot_power) platform()->restore_boot_power();
}

void bt_control_set_power_persist(bool persist) {
    if (platform()->set_power_persist) platform()->set_power_persist(persist);
}

static void set_power(bool on) {
    if (platform()->power) {
        platform()->power(on);
        return;
    }
    pthread_mutex_lock(&bt_chip_mutex);
    const bt_stack_ops_t * stack = bt_stack();
    if (!stack->set_powered || !stack->set_powered(on))
        DBG_LOG("bt_control: could not switch the adapter %s\n", on ? "on" : "off");
    pthread_mutex_unlock(&bt_chip_mutex);
}

void bt_control_enable(void) {
    set_power(true);
}

void bt_control_disable(void) {
    set_power(false);
}

bool bt_control_reconcile_source_settings(void) {
    prepare();
    return bt_audio()->reconcile();
}

/* ---- devices ------------------------------------------------------------------------ */

bool bt_control_is_connected(void) {
    return bt_stack()->is_connected();
}

int bt_control_any_paired_connected(void) {
    return bt_stack()->any_paired_connected();
}

static int list_devices(bt_device_t * out, int max_count, bool paired_only) {
    if (max_count <= 0) return 0;
    bt_stack_device_t devs[64];
    int n = bt_stack()->list_devices(devs, max_count < 64 ? max_count : 64, paired_only);
    for (int i = 0; i < n; i++) out[i] = devs[i].dev;
    return n;
}

int bt_control_list_paired_states(bt_device_t * out, int max_count) {
    return list_devices(out, max_count, true);
}

/* Devices the stack already knows, no inquiry: an inquiry makes the
 * controller interleave discovery with a live A2DP link (audible), so the
 * caller decides when that is worth it. */
int bt_control_list_devices(bt_device_t * out, int max_count) {
    return list_devices(out, max_count, false);
}

int bt_control_scan(int seconds, bt_device_t * out, int max_count) {
    if (platform()->scan) return platform()->scan(seconds, out, max_count);
    const bt_stack_ops_t * stack = bt_stack();
    if (seconds > 0 && stack->discover) (void) stack->discover(seconds); /* blocks for `seconds` */
    int n = list_devices(out, max_count, false);
    return n < 0 ? 0 : n;
}

#define PAIR_TIMEOUT_MS 20000
#define CONNECT_TIMEOUT_MS 15000
#define DISCONNECT_TIMEOUT_MS 15000

/* Pair (if not yet), trust, connect: trusting first means a later
 * reconnect needs none of this. A pairing failure is not fatal (the
 * agent may still accept the connect), as with bluetoothctl. */
bool bt_control_connect(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    if (platform()->connect) return platform()->connect(mac);
    pthread_mutex_lock(&bt_chip_mutex);
    prepare();
    const bt_stack_ops_t * stack = bt_stack();
    bt_stack_device_t d;
    bool paired = stack->device_info(mac, &d, CONNECT_TIMEOUT_MS) && d.dev.paired;
    if (!paired && stack->pair) (void) stack->pair(mac, PAIR_TIMEOUT_MS);
    if (stack->set_trusted) (void) stack->set_trusted(mac, true);
    bool ok = stack->connect && stack->connect(mac, CONNECT_TIMEOUT_MS);
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_disconnect(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    bt_audio()->note_requested_disconnect(mac);
    if (platform()->disconnect) return platform()->disconnect(mac);
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_stack()->disconnect && bt_stack()->disconnect(mac, DISCONNECT_TIMEOUT_MS);
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_forget(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    if (platform()->forget) return platform()->forget(mac);
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_stack()->remove && bt_stack()->remove(mac);
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

/* ---- bounded reconnect of a paired headset -------------------------------------------- */

#define BT_RECONNECT_QUERY_TIMEOUT_MS 1500
#define BT_RECONNECT_CONNECT_TIMEOUT_MS 6500

static bool cancelled(bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    return cancel_cb && cancel_cb(cancel_ctx);
}

/* Paired, trusted, not blocked, and an A2DP sink. */
static bool reconnect_eligible(const bt_stack_device_t * d) {
    return d->dev.paired && d->trusted && !d->blocked && d->audio_sink;
}

/* Never pairs or trusts. Without a preferred device exactly one eligible
 * paired device must exist (no guessing between two headsets). Nothing is
 * done while the radio is off or an output is already connected, and a
 * connection that completes after a cancel is dropped again. */
static bool generic_reconnect_paired(const char * preferred_mac, bt_control_cancel_callback_t cancel_cb,
                                     void * cancel_ctx) {
    if (preferred_mac && (!bt_valid_mac(preferred_mac) || cancelled(cancel_cb, cancel_ctx))) return false;
    const bt_stack_ops_t * stack = bt_stack();
    const bt_audio_ops_t * audio = bt_audio();
    if (stack->power_state(BT_RECONNECT_QUERY_TIMEOUT_MS) != BT_POWER_ON || cancelled(cancel_cb, cancel_ctx))
        return false;
    int out = audio->output_connected(BT_RECONNECT_QUERY_TIMEOUT_MS);
    if (out != 0) return out > 0;
    if (cancelled(cancel_cb, cancel_ctx)) return false;

    char target[18] = {0};
    bt_stack_device_t info;
    if (preferred_mac) {
        snprintf(target, sizeof(target), "%s", preferred_mac);
        if (!stack->device_info(target, &info, BT_RECONNECT_QUERY_TIMEOUT_MS) || !reconnect_eligible(&info))
            return false;
    } else {
        bt_stack_device_t devs[64];
        int n = stack->list_devices(devs, 64, true);
        if (n <= 0 || cancelled(cancel_cb, cancel_ctx)) return false;
        int eligible = 0;
        for (int i = 0; i < n; i++) {
            if (!bt_valid_mac(devs[i].dev.mac) || !reconnect_eligible(&devs[i])) continue;
            if (++eligible > 1) return false;
            info = devs[i];
            memcpy(target, info.dev.mac, sizeof(target));
            target[sizeof(target) - 1] = '\0';
        }
        if (eligible != 1) return false;
    }
    if (info.dev.connected) return true;
    if (cancelled(cancel_cb, cancel_ctx)) return false;

    pthread_mutex_lock(&bt_chip_mutex);
    bool result = false;
    if (!cancelled(cancel_cb, cancel_ctx) && stack->power_state(BT_RECONNECT_QUERY_TIMEOUT_MS) == BT_POWER_ON &&
        audio->output_connected(BT_RECONNECT_QUERY_TIMEOUT_MS) == 0) {
        /* Refresh under the chip lock so a connection made by another path
         * before the lock was taken counts as already there. */
        bt_stack_device_t locked;
        if (!stack->device_info(target, &locked, BT_RECONNECT_QUERY_TIMEOUT_MS) || !reconnect_eligible(&locked)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        if (locked.dev.connected) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return true;
        }
        if (cancelled(cancel_cb, cancel_ctx)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        bool connect_ok = stack->connect && stack->connect(target, BT_RECONNECT_CONNECT_TIMEOUT_MS);
        /* Verify, and drop a connection that raced a cancel (the callback
         * is deliberately not used for this cleanup read). */
        bool connected_after = stack->device_info(target, &locked, BT_RECONNECT_QUERY_TIMEOUT_MS) &&
                               reconnect_eligible(&locked) && locked.dev.connected;
        if (cancelled(cancel_cb, cancel_ctx)) {
            if (stack->disconnect) (void) stack->disconnect(target, BT_RECONNECT_QUERY_TIMEOUT_MS);
        } else {
            result = connect_ok && connected_after;
        }
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

bool bt_control_reconnect_paired(const char * preferred_mac, bt_control_cancel_callback_t cancel_cb,
                                 void * cancel_ctx) {
    if (platform()->reconnect_paired) return platform()->reconnect_paired(preferred_mac, cancel_cb, cancel_ctx);
    return generic_reconnect_paired(preferred_mac, cancel_cb, cancel_ctx);
}

/* ---- the output link (audio transport) --------------------------------------------- */

bool bt_control_is_a2dp_source_connected(void) {
    return bt_audio()->is_source_connected();
}

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    return bt_audio()->get_connected_mac(out, out_size);
}

bool bt_control_get_connected_device_stream(char * out, size_t out_size, unsigned int * out_sample_rate) {
    return bt_audio()->get_connected_stream(out, out_size, out_sample_rate);
}

bool bt_control_get_connected_device_codec(char * out, size_t out_size) {
    return bt_control_get_connected_device_stream(out, out_size, NULL);
}

bool bt_control_get_a2dp_link(bt_a2dp_link_t * out) {
    return bt_audio()->get_a2dp_link(out);
}

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    bt_audio()->get_dac_stream_info(out);
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    prepare();
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_audio()->apply_output_settings(dac_mode_enabled, volume_sync_enabled);
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

/* ---- codec and rate ---------------------------------------------------------------- */

bool bt_control_codec_supported(const char * codec) {
    const bt_codec_policy_t * p = codec_policy();
    return p && p->codec_supported(codec);
}

bool bt_control_set_codec(const char * codec) {
    const bt_codec_policy_t * p = codec_policy();
    return p && p->set_codec(codec);
}

bool bt_control_get_system_codec(char * out, size_t out_size) {
    const bt_codec_policy_t * p = codec_policy();
    if (p) return p->get_system_codec(out, out_size);
    if (out && out_size) out[0] = '\0';
    return false;
}

void bt_control_restore_codec_preference(const char * codec) {
    const bt_codec_policy_t * p = codec_policy();
    if (p) p->restore_preference(codec);
}

bool bt_control_codec_per_device(void) {
    const bt_codec_policy_t * p = codec_policy();
    return p && p->apply;
}

bool bt_control_codec_device_view(const char * mac, bt_codec_device_view_t * out) {
    const bt_codec_policy_t * p = codec_policy();
    if (out) memset(out, 0, sizeof(*out));
    return p && p->device_view && out && p->device_view(mac, out);
}

bool bt_control_set_device_codec(const char * mac, const char * choice) {
    const bt_codec_policy_t * p = codec_policy();
    return p && p->set_device_codec && p->set_device_codec(mac, choice);
}

void bt_control_set_ldac_quality(const char * quality) {
    const bt_codec_policy_t * p = codec_policy();
    if (p && p->set_ldac_quality) p->set_ldac_quality(quality);
}

bool bt_control_apply_codec(void) {
    const bt_codec_policy_t * p = codec_policy();
    return p && p->apply && p->apply(NULL);
}

void bt_control_set_sample_rate(unsigned int rate) {
    const bt_codec_policy_t * p = codec_policy();
    if (p) p->set_sample_rate(rate);
}

void bt_control_choose_sample_rate(unsigned int rate) {
    const bt_codec_policy_t * p = codec_policy();
    if (p) p->choose_sample_rate(rate);
}

void bt_control_set_speexrate_enabled(bool enabled) {
    bt_audio()->set_speexrate_enabled(enabled);
}

void bt_control_set_dac_all_codecs(bool enabled) {
    bt_audio()->set_dac_all_codecs(enabled);
}

bool bt_control_reconnect_for_rate_change(unsigned int rate) {
    return bt_audio()->reconnect_for_rate_change(rate);
}

int bt_control_get_available_rates(unsigned int * out, int max_count) {
    return bt_audio()->get_available_rates(out, max_count);
}

const char * bt_control_get_playback_pcm(void) {
    return bt_audio()->get_playback_pcm();
}

bool bt_control_prepare_playback_pcm(char * out, size_t out_size) {
    /* The plugin reads the system codec when the stream opens: set the
     * device's choice first (a remote that connected on its own has not
     * been through the connect path). */
    const bt_codec_policy_t * p = codec_policy();
    if (p && p->apply) (void) p->apply(NULL);
    return bt_audio()->prepare_playback_pcm(out, out_size);
}

/* ---- volume sync and the output watch ---------------------------------------------------- */

void bt_control_source_volume_sync_start(void) {
    bt_audio()->volume_sync_start();
}

void bt_control_source_volume_sync_stop(void) {
    bt_audio()->volume_sync_stop();
}

bool bt_control_source_volume_sync_is_running(void) {
    return bt_audio()->volume_sync_is_running();
}

bool bt_control_source_volume_sync_consume_percent(int * out_percent) {
    return bt_audio()->volume_sync_consume_percent(out_percent);
}

void bt_control_output_disconnect_watch_start(void) {
    bt_audio()->watch_start();
}

void bt_control_output_disconnect_watch_stop(void) {
    bt_audio()->watch_stop();
}

bool bt_control_output_disconnect_watch_is_running(void) {
    return bt_audio()->watch_is_running();
}

bool bt_control_output_disconnect_consume(void) {
    return bt_audio()->watch_consume();
}
