/* Audio transport: HiBy's ALSA "bluetooth" plugin (the R3 Pro), fed by our
 * BlueZ 4 bluetoothd over its unix socket.
 *
 * audio_output.c pipes PCM into `aplay -D <pcm>`. The plugin does no rate
 * conversion, so the PCM is an ALSA "plug" pinned to the rate bluetoothd
 * negotiated (/tmp/bt_a2dp_stream, written by our bluetoothd). The plugin
 * has no volume of its own and BlueZ 4 has no AVRCP absolute volume, so the
 * player scales the samples (BT_BACKEND_PLAYER_VOLUME). Receiver (A2DP
 * sink / "DAC") mode is not supported on this stack; those entry points
 * report unavailable.
 *
 * Moved unchanged from the old bluetooth_bluez4.c. */
#include "bt_backend.h"
#include "board_config.h"
#include "debug_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STREAM_INFO_PATH "/tmp/bt_a2dp_stream"
#define BT_ALSA_CONFIG_DIR BOARD_DATA_ROOT "/alsa"
#define BT_ALSA_PCM_NAME "mellowtone_bt"

/* ---- negotiated stream (written by our bluetoothd) ------------------------ */

typedef struct {
    char addr[18];
    char codec[16];
    unsigned int rate;
    unsigned int channels;
} stream_info_t;

static bool read_stream_info(stream_info_t * s) {
    memset(s, 0, sizeof(*s));
    FILE * f = fopen(STREAM_INFO_PATH, "r");
    if (!f) return false;
    int n = fscanf(f, "addr=%17s codec=%15s rate=%u channels=%u", s->addr, s->codec, &s->rate, &s->channels);
    fclose(f);
    return n == 4 && s->rate > 0;
}

static bool connected_device(bt_device_t * out) {
    return bt_stack()->connected_device(out);
}

/* ---- link ------------------------------------------------------------------ */

/* "Source" = this player is the A2DP source, i.e. a sink (headphones,
 * speaker, car) is connected for output. */
static bool hiby_is_source_connected(void) {
    return connected_device(NULL);
}

static bool hiby_get_connected_mac(char * out, size_t out_size) {
    bt_device_t d;
    if (!connected_device(&d)) return false;
    snprintf(out, out_size, "%s", d.mac);
    return true;
}

static bool hiby_get_connected_stream(char * out, size_t out_size, unsigned int * out_sample_rate) {
    bt_device_t d;
    stream_info_t s;
    if (!connected_device(&d) || !read_stream_info(&s) || strcasecmp(s.addr, d.mac) != 0) return false;
    snprintf(out, out_size, "%s", s.codec);
    if (out_sample_rate) *out_sample_rate = s.rate;
    return true;
}

static bool hiby_get_a2dp_link(bt_a2dp_link_t * out) {
    memset(out, 0, sizeof(*out));
    bt_device_t d;
    if (!connected_device(&d)) return false;
    out->connected = true;
    snprintf(out->mac, sizeof(out->mac), "%s", d.mac);
    stream_info_t s;
    if (read_stream_info(&s) && strcasecmp(s.addr, d.mac) == 0) {
        snprintf(out->codec, sizeof(out->codec), "%s", s.codec);
        out->sample_rate = s.rate;
    }
    return true;
}

static int hiby_output_connected(int timeout_ms) {
    (void) timeout_ms;
    return connected_device(NULL) ? 1 : 0;
}

static void hiby_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (out) memset(out, 0, sizeof(*out)); /* receiver mode unavailable */
}

/* ---- settings ----------------------------------------------------------------- */

static bool hiby_true(void) {
    return true;
}

static bool hiby_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    (void) dac_mode_enabled;
    (void) volume_sync_enabled;
    return true; /* the platform's prepare() (/tmp/bt_prefer_rate) is all there is */
}

static void hiby_nothing(void) {}

static void hiby_note_requested_disconnect(const char * mac) {
    (void) mac;
}

static void hiby_set_flag(bool enabled) {
    (void) enabled; /* no speex rate plugin, no DAC mode on this firmware */
}

static bool hiby_reconnect_for_rate_change(unsigned int rate) {
    (void) rate;
    return false; /* takes effect on the next stream configuration */
}

static int hiby_get_available_rates(unsigned int * out, int max_count) {
    if (!out || max_count < 2) return 0;
    out[0] = 44100;
    out[1] = 48000;
    return 2;
}

/* ---- playback PCM ---------------------------------------------------------------- */

static const char * hiby_get_playback_pcm(void) {
    return BT_ALSA_PCM_NAME;
}

static bool hiby_prepare_playback_pcm(char * out, size_t out_size) {
    if (!out || out_size == 0) return false;
    bt_device_t d;
    if (!connected_device(&d)) return false;
    /* The stream is configured during the plugin's first open, so before
     * that use the rate we asked bluetoothd for; LDAC/AAC/aptX/SBC all offer
     * 44.1 kHz on common sinks. A later open picks up the real value. */
    stream_info_t s;
    unsigned int requested_rate = bt_platform_hiby_requested_rate();
    unsigned int rate = requested_rate ? requested_rate : 44100;
    unsigned int channels = 2;
    if (read_stream_info(&s) && strcasecmp(s.addr, d.mac) == 0) {
        rate = s.rate;
        if (s.channels) channels = s.channels;
    }
    if (mkdir(BT_ALSA_CONFIG_DIR, 0755) != 0 && errno != EEXIST) return false;
    char tmp_path[128], final_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.asoundrc.tmp", BT_ALSA_CONFIG_DIR);
    snprintf(final_path, sizeof(final_path), "%s/.asoundrc", BT_ALSA_CONFIG_DIR);
    FILE * f = fopen(tmp_path, "w");
    if (!f) return false;
    /* HiBy's plugin accepts any hw_params without converting, so pin the
     * slave to the negotiated rate and S16 and let plug convert. */
    fprintf(f,
            "pcm.%s {\n"
            "    type plug\n"
            "    slave {\n"
            "        pcm {\n"
            "            type bluetooth\n"
            "            bdaddr \"%s\"\n"
            "            profile \"a2dp\"\n"
            "        }\n"
            "        rate %u\n"
            "        channels %u\n"
            "        format S16_LE\n"
            "    }\n"
            "}\n",
            BT_ALSA_PCM_NAME, d.mac, rate, channels);
    bool ok = fclose(f) == 0 && rename(tmp_path, final_path) == 0;
    if (ok) snprintf(out, out_size, "%s", BT_ALSA_PCM_NAME);
    DBG_LOG("bt_hiby: playback pcm %s -> %s %u Hz\n", BT_ALSA_PCM_NAME, d.mac, rate);
    return ok;
}

/* ---- AVRCP volume sync (not on BlueZ 4.101) ---------------------------------------- */

static bool hiby_false(void) {
    return false;
}

static bool hiby_consume_percent(int * out_percent) {
    (void) out_percent;
    return false;
}

/* ---- output disconnect watch --------------------------------------------------------- */

static pthread_t watch_thread;
static bool watch_joinable;
static atomic_bool watch_active;
static atomic_bool watch_flag;
/* The watch checks once a second; stop wakes it at once (it used to sleep
 * in 100 ms slices just to notice a stop). Monotonic, like the rest. */
static pthread_mutex_t watch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t watch_cond;
static pthread_once_t watch_cond_once = PTHREAD_ONCE_INIT;
#define WATCH_INTERVAL_MS 1000

static void watch_cond_init(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&watch_cond, &attr);
    pthread_condattr_destroy(&attr);
}

static void watch_sleep(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += WATCH_INTERVAL_MS / 1000;
    pthread_mutex_lock(&watch_mutex);
    while (atomic_load(&watch_active) &&
           pthread_cond_timedwait(&watch_cond, &watch_mutex, &deadline) != ETIMEDOUT) {
    }
    pthread_mutex_unlock(&watch_mutex);
}

static void * watch_thread_func(void * arg) {
    (void) arg;
    bool was_connected = bt_control_is_connected();
    while (atomic_load(&watch_active)) {
        watch_sleep();
        if (!atomic_load(&watch_active)) break;
        bool now = bt_control_is_connected();
        if (was_connected && !now) atomic_store(&watch_flag, true);
        was_connected = now;
    }
    return NULL;
}

static void hiby_watch_start(void) {
    if (watch_joinable) {
        if (atomic_load(&watch_active)) return;
        pthread_join(watch_thread, NULL);
        watch_joinable = false;
    }
    pthread_once(&watch_cond_once, watch_cond_init);
    atomic_store(&watch_flag, false);
    atomic_store(&watch_active, true);
    if (pthread_create(&watch_thread, NULL, watch_thread_func, NULL) == 0) watch_joinable = true;
    else atomic_store(&watch_active, false);
}

static void hiby_watch_stop(void) {
    pthread_once(&watch_cond_once, watch_cond_init);
    pthread_mutex_lock(&watch_mutex);
    atomic_store(&watch_active, false);
    pthread_cond_broadcast(&watch_cond);
    pthread_mutex_unlock(&watch_mutex);
    if (watch_joinable) {
        pthread_join(watch_thread, NULL);
        watch_joinable = false;
    }
    atomic_store(&watch_flag, false);
}

static bool hiby_watch_is_running(void) {
    return atomic_load(&watch_active);
}

static bool hiby_watch_consume(void) {
    return atomic_exchange(&watch_flag, false);
}

const bt_audio_ops_t bt_audio_hiby = {
    .name = "hiby-alsa-plugin",
    .kind = BT_AUDIO_HIBY_PLUGIN,
    /* No speex plugin, no live rate change (the rate is asked for before the
     * stream is configured), and the player scales the volume itself. */
    .features = BT_BACKEND_PLAYER_VOLUME,
    .codec_policy = NULL, /* the platform's (bt_codec) */
    .ensure_running = hiby_true,
    .reconcile = hiby_true,
    .apply_output_settings = hiby_apply_output_settings,
    .after_stack_restart = hiby_nothing,
    .get_dac_stream_info = hiby_get_dac_stream_info,
    .get_a2dp_link = hiby_get_a2dp_link,
    .is_source_connected = hiby_is_source_connected,
    .get_connected_mac = hiby_get_connected_mac,
    .get_connected_stream = hiby_get_connected_stream,
    .output_connected = hiby_output_connected,
    .note_requested_disconnect = hiby_note_requested_disconnect,
    .set_speexrate_enabled = hiby_set_flag,
    .set_dac_all_codecs = hiby_set_flag,
    .reconnect_for_rate_change = hiby_reconnect_for_rate_change,
    .get_available_rates = hiby_get_available_rates,
    .get_playback_pcm = hiby_get_playback_pcm,
    .prepare_playback_pcm = hiby_prepare_playback_pcm,
    .volume_sync_start = hiby_nothing,
    .volume_sync_stop = hiby_nothing,
    .volume_sync_is_running = hiby_false,
    .volume_sync_consume_percent = hiby_consume_percent,
    .watch_start = hiby_watch_start,
    .watch_stop = hiby_watch_stop,
    .watch_is_running = hiby_watch_is_running,
    .watch_consume = hiby_watch_consume,
};
