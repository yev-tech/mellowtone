/* R3 Pro platform: HiBy's sys_server powers the radio and runs
 * pairing, connections and scans, /usr/bin/bt_codec owns the codec, and our
 * bluetoothd reads the preferred transport rate from /tmp/bt_prefer_rate.
 * The codec is chosen per device (bt_codec_choice.h) from the codecs our
 * bluetoothd reports in /tmp/bt_a2dp_remote.
 *
 * Moved unchanged from the old bluetooth_bluez4.c: the same sys_server
 * requests, files, timeouts and serialisation. State reads go to the
 * BlueZ 4 stack (bt_stack_bluez4.c) through bt_stack(). */
#include "bt_backend.h"
#include "bt_codec_choice.h"
#include "board_config.h"
#include "cJSON.h"
#include "debug_log.h"
#include "hiby_sys_client.h"
#include "hiby_sys_server.h"
#include "storage_paths.h"
#include "subprocess.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PREFER_RATE_PATH "/tmp/bt_prefer_rate"

/* One command at a time against the controller, like sys_server's loop. */
static pthread_mutex_t bt_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;

static char codec_preference[16] = "auto";
static unsigned int requested_rate;

unsigned int bt_platform_hiby_requested_rate(void) {
    return requested_rate;
}

/* ---- commands ------------------------------------------------------------ */

static bool run(int timeout_ms, const char * a0, const char * a1, const char * a2, const char * a3) {
    char * argv[] = { (char *) a0, (char *) a1, (char *) a2, (char *) a3, NULL };
    int exit_code = -1;
    bool ran = subprocess_run_checked(argv, NULL, 0, timeout_ms, &exit_code);
    DBG_LOG("bt_hiby: %s %s %s %s -> ran=%d exit=%d\n", a0, a1 ? a1 : "", a2 ? a2 : "", a3 ? a3 : "",
            ran, exit_code);
    return ran && exit_code == 0;
}

static void write_prefer_rate(void) {
    /* Ask bluetoothd for a transport rate we can feed without resampling;
     * only read by it while this player runs (see bluetoothd hiby_preferred_rate). */
    unsigned int rate = requested_rate ? requested_rate : 44100;
    /* "<rate> <pid>": bluetoothd ignores the file once this process is gone,
     * so a crash can never leave the stock player with a 44.1 kHz LDAC link. */
    char tmp[] = PREFER_RATE_PATH ".tmp";
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u %ld\n", rate, (long) getpid());
    if (fclose(f) != 0 || rename(tmp, PREFER_RATE_PATH) != 0) unlink(tmp);
}

/* ---- power ----------------------------------------------------------------- */

static bool hiby_init_chip(void) {
    /* bt_init (stock S40) brings the chip up and creates this marker. */
    for (int i = 0; i < 100 && access("/tmp/bt_init_ok", F_OK) != 0; i++) usleep(100000);
    write_prefer_rate();
    return access("/tmp/bt_init_ok", F_OK) == 0;
}

/* Power goes through sys_server, as the stock player does: its BT:ON also
 * starts bt_deamon on the first enable after boot and reconnects the most
 * recently used paired device, and its on/off state stays consistent for
 * the stock player after a switch. The last choice is kept in
 * BT_POWER_FILE ("1"/"0", missing = on) and restored at startup. */
/* config/ on the card. */
#define BT_POWER_FILE MT_BT_POWER_FILE
#define BT_POWER_WAIT_MS 20000

static volatile bool persist_power_choice = true;

static void hiby_set_power_persist(bool persist) {
    persist_power_choice = persist;
}

static void save_power_choice(bool on) {
    if (!persist_power_choice || !mt_state_prepare_write(BT_POWER_FILE)) return;
    FILE * f = fopen(BT_POWER_FILE, "w");
    if (!f) return;
    fputs(on ? "1\n" : "0\n", f);
    fclose(f);
}

static bool saved_power_choice(void) {
    FILE * f = fopen(BT_POWER_FILE, "r");
    if (!f) return true;
    int c = fgetc(f);
    fclose(f);
    return c != '0';
}

static void wait_powered(bool on) {
    for (int waited = 0; waited < BT_POWER_WAIT_MS; waited += 250) {
        if (bt_control_is_powered() == on) return;
        usleep(250000);
    }
    DBG_LOG("bt_hiby: adapter did not become %s within %d ms\n", on ? "powered" : "unpowered", BT_POWER_WAIT_MS);
}

static void hiby_power(bool on) {
    pthread_mutex_lock(&bt_cmd_mutex);
    if (on) write_prefer_rate();
    save_power_choice(on);
    hiby_sys_server_command(on ? "BT:ON" : "BT:OFF");
    wait_powered(on);
    pthread_mutex_unlock(&bt_cmd_mutex);
}

static void * restore_power_thread(void * arg) {
    (void) arg;
    sleep(3);                                   /* let the UI come up first */
    /* The stock bt_init can still be flashing the chip this early in boot. */
    for (int i = 0; i < 60 && access("/tmp/bt_init_ok", F_OK) != 0; i++) sleep(1);
    if (!bt_control_init_chip()) return NULL;
    if (!bt_control_is_powered()) bt_control_enable();
    return NULL;
}

static void hiby_restore_boot_power(void) {
    if (!saved_power_choice()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, restore_power_thread, NULL) == 0) pthread_detach(t);
}

/* ---- devices ------------------------------------------------------------------- */

/* bluez-tools writes {"DEVICE":[{"MAC":..,"Name":..,"Paired":0|1,"Connected":0|1,..}]} */
static int parse_device_json(const char * path, bt_device_t * out, int max_count) {
    FILE * f = fopen(path, "r");
    if (!f) return -1;
    char * buf = malloc(65536);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    cJSON * root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;
    int count = 0;
    cJSON * arr = cJSON_GetObjectItemCaseSensitive(root, "DEVICE");
    cJSON * it;
    cJSON_ArrayForEach(it, arr) {
        if (count >= max_count) break;
        cJSON * mac = cJSON_GetObjectItemCaseSensitive(it, "MAC");
        cJSON * name = cJSON_GetObjectItemCaseSensitive(it, "Alias");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(it, "Name");
        cJSON * paired = cJSON_GetObjectItemCaseSensitive(it, "Paired");
        cJSON * conn = cJSON_GetObjectItemCaseSensitive(it, "Connected");
        if (!cJSON_IsString(mac) || !bt_valid_mac(mac->valuestring)) continue;
        bt_device_t * d = &out[count++];
        memset(d, 0, sizeof(*d));
        snprintf(d->mac, sizeof(d->mac), "%s", mac->valuestring);
        snprintf(d->name, sizeof(d->name), "%s", cJSON_IsString(name) ? name->valuestring : mac->valuestring);
        d->paired = cJSON_IsNumber(paired) && paired->valueint;
        d->connected = cJSON_IsNumber(conn) && conn->valueint;
    }
    cJSON_Delete(root);
    return count;
}

/* Pairing, connections and scans go through the stock sys_server, as the
 * stock player does: its worker serialises them with
 * the stock player's own requests and runs the same bluez-tools. */
static bool sys_bt(const char * verb, const char * mac, int timeout_ms) {
    char cmd[64], ok[32], fail[32];
    snprintf(cmd, sizeof(cmd), mac ? "BT:%s:%s" : "BT:%s", verb, mac ? mac : "");
    snprintf(ok, sizeof(ok), "BT:%s:OK", verb);
    snprintf(fail, sizeof(fail), "BT:%s:FAIL", verb);
    const char * prefixes[] = { ok, fail, NULL };
    char result[64];
    if (!hiby_sys_request(cmd, prefixes, timeout_ms, result, sizeof(result))) return false;
    return strcmp(result, ok) == 0;
}

/* sys_server BT:SCAN: DiscoverType BR_EDR, then one bt-adapter -d inquiry
 * (about 10 s) written as JSON to /data/bt_scan.txt. */
#define STOCK_SCAN_RESULT_PATH "/data/bt_scan.txt"

/* The scan file holds only devices found by this inquiry, while callers
 * expect paired and discovered devices in one list (gui_network splits them
 * by ->paired): paired devices BlueZ already knows are added from the stack,
 * so they show even when they are not discoverable right now. */
static int hiby_scan(int seconds, bt_device_t * out, int max_count) {
    pthread_mutex_lock(&bt_cmd_mutex);
    int timeout_ms = (seconds > 0 ? seconds : 10) * 1000 + 20000;
    int count = 0;
    if (sys_bt("SCAN", NULL, timeout_ms)) count = parse_device_json(STOCK_SCAN_RESULT_PATH, out, max_count);
    pthread_mutex_unlock(&bt_cmd_mutex);
    if (count < 0) count = 0;

    bt_stack_device_t known[32];
    int n = bt_stack()->list_devices(known, 32, true);
    for (int i = 0; i < n && count < max_count; i++) {
        bool seen = false;
        for (int j = 0; j < count && !seen; j++) {
            if (strcasecmp(out[j].mac, known[i].dev.mac) == 0) {
                out[j].paired = true;
                seen = true;
            }
        }
        if (!seen) out[count++] = known[i].dev;
    }
    return count;
}

static bool is_paired(const char * mac) {
    bt_stack_device_t devs[32];
    int n = bt_stack()->list_devices(devs, 32, true);
    for (int i = 0; i < n; i++)
        if (strcasecmp(devs[i].dev.mac, mac) == 0) return true;
    return false;
}

static bool apply_device_codec(const char * mac);

static bool hiby_connect(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    pthread_mutex_lock(&bt_cmd_mutex);
    write_prefer_rate();
    bool ok = true;
    /* BT:PAIR pairs through bt-device's agent and cleans up a half-created
     * device on failure. */
    if (!is_paired(mac)) ok = sys_bt("PAIR", mac, 70000);
    if (ok) ok = sys_bt("CONNECT", mac, 40000);
    pthread_mutex_unlock(&bt_cmd_mutex);
    if (ok) (void) apply_device_codec(mac);
    return ok;
}

static bool hiby_disconnect(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    pthread_mutex_lock(&bt_cmd_mutex);
    bool ok = sys_bt("DISCONNECT", mac, 30000);
    pthread_mutex_unlock(&bt_cmd_mutex);
    return ok;
}

static bool hiby_forget(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    pthread_mutex_lock(&bt_cmd_mutex);
    bool ok = sys_bt("UNPAIR", mac, 30000);
    pthread_mutex_unlock(&bt_cmd_mutex);
    return ok;
}

/* Never pairs: only devices the stack reports as paired A2DP sinks are
 * asked to connect, the preferred one first, and the cancel callback is
 * checked before every request so a shutdown waits for one at most. */
static bool reconnect_one(const char * mac, bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    pthread_mutex_lock(&bt_cmd_mutex);
    bool ok = false;
    if (!(cancel_cb && cancel_cb(cancel_ctx))) {
        write_prefer_rate();
        ok = sys_bt("CONNECT", mac, 40000);
    }
    pthread_mutex_unlock(&bt_cmd_mutex);
    if (ok) (void) apply_device_codec(mac);
    return ok;
}

static bool hiby_reconnect_paired(const char * preferred_mac, bt_control_cancel_callback_t cancel_cb,
                                  void * cancel_ctx) {
    if (bt_control_is_connected()) return true;
    if (cancel_cb && cancel_cb(cancel_ctx)) return false;
    bt_stack_device_t devs[32];
    int n = bt_stack()->list_devices(devs, 32, false);
    if (n <= 0) return false;
    bool have_preferred = preferred_mac && bt_valid_mac(preferred_mac);
    for (int pass = have_preferred ? 0 : 1; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const bt_stack_device_t * d = &devs[i];
            if (!d->dev.paired || !d->audio_sink || !bt_valid_mac(d->dev.mac)) continue;
            bool preferred = have_preferred && strcasecmp(d->dev.mac, preferred_mac) == 0;
            if (preferred != (pass == 0)) continue;
            if (cancel_cb && cancel_cb(cancel_ctx)) return false;
            if (reconnect_one(d->dev.mac, cancel_cb, cancel_ctx)) return true;
        }
    }
    return false;
}

/* ---- codec and rate --------------------------------------------------------------- */

/* Mellowtone preference names -> stock bt_codec / audio.conf DefaultCodec. */
static const char * stock_codec_name(const char * codec) {
    if (!codec) return NULL;
    if (strcmp(codec, "ldac_hq") == 0) return "LDAC_HQ";
    if (strcmp(codec, "ldac_sq") == 0 || strcmp(codec, "ldac") == 0) return "LDAC_SQ";
    if (strcmp(codec, "ldac_mq") == 0) return "LDAC_MQ";
    if (strcmp(codec, "ldac_abr") == 0) return "LDAC_ABR";
    if (strcmp(codec, "aptx") == 0) return "APTX";
    if (strcmp(codec, "aac") == 0) return "AAC";
    if (strcmp(codec, "sbc") == 0 || strcmp(codec, "sbc_xq") == 0) return "SBC";
    return NULL; /* "auto": leave the stock default */
}

static bool hiby_codec_supported(const char * codec) {
    static const char * const codecs[] = { "ldac_hq", "ldac_sq", "ldac_mq", "ldac_abr", "aptx", "aac", "sbc" };
    for (size_t i = 0; codec && i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (strcmp(codec, codecs[i]) == 0) return true;
    return false;
}

static bool hiby_get_system_codec(char * out, size_t out_size) {
    /* DefaultCodec in audio.conf, the stock player's own setting (bt_codec). */
    if (!out || !out_size) return false;
    out[0] = '\0';
    FILE * f = fopen("/etc/bluetooth/audio.conf", "r");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "DefaultCodec=", 13) != 0) continue;
        const char * v = line + 13;
        static const struct { const char * stock, * ours; } map[] = {
            { "LDAC_HQ", "ldac_hq" }, { "LDAC_SQ", "ldac_sq" }, { "LDAC_MQ", "ldac_mq" }, { "LDAC_ABR", "ldac_abr" },
            { "APTX", "aptx" }, { "AAC", "aac" }, { "SBC", "sbc" },
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (strncmp(v, map[i].stock, strlen(map[i].stock)) == 0 &&
                (v[strlen(map[i].stock)] == '\n' || v[strlen(map[i].stock)] == '\r' || !v[strlen(map[i].stock)])) {
                snprintf(out, out_size, "%s", map[i].ours);
                break;
            }
        break;
    }
    fclose(f);
    return out[0] != '\0';
}

static void hiby_restore_codec_preference(const char * codec) {
    snprintf(codec_preference, sizeof(codec_preference), "%s", codec ? codec : "auto");
}

/* bt_codec only rewrites a file (no controller command), so it has its own
 * lock: the codec set before a stream must not wait for a scan. */
static pthread_mutex_t codec_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool run_bt_codec(const char * stock) {
    /* bt_codec rewrites DefaultCodec in audio.conf; the plugin reads it on
     * every open and bluetoothd reconfigures the stream in place. */
    pthread_mutex_lock(&codec_mutex);
    bool ok = run(10000, "/usr/bin/bt_codec", stock, NULL, NULL);
    pthread_mutex_unlock(&codec_mutex);
    return ok;
}

static bool hiby_set_codec(const char * codec) {
    if (!codec) return false;
    hiby_restore_codec_preference(codec);
    const char * stock = stock_codec_name(codec);
    if (!stock) return strcmp(codec, "auto") == 0;
    return run_bt_codec(stock);
}

/* ---- per-device choice ------------------------------------------------------------ */

#define REMOTE_INFO_PATH "/tmp/bt_a2dp_remote"
/* config/ on the card. */
#define BT_CODECS_FILE MT_BT_CODECS_FILE

/* Guards everything below; never held across bt_codec or a card write. */
static pthread_mutex_t choice_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_codec_store_t codec_store;
static bool codec_store_loaded;
static char ldac_quality[16] = BT_CODEC_LDAC_QUALITY_DEFAULT;

/* Caller holds choice_mutex. A missing file on a mounted card is an empty
 * table; without a card it is read again next time, and nothing is saved
 * until then (a save would drop the card's entries). */
static void load_store_locked(void) {
    if (codec_store_loaded) return;
    char buf[BT_CODEC_DEVICES_MAX * 64 + 64];
    size_t n = 0;
    FILE * f = fopen(BT_CODECS_FILE, "r");
    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    bt_codec_store_parse(&codec_store, buf);
    codec_store_loaded = f || mt_state_available();
}

/* One writer at a time (they share the temporary file). */
static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;

static void save_store(void) {
    char buf[BT_CODEC_DEVICES_MAX * 64 + 64];
    pthread_mutex_lock(&save_mutex);
    pthread_mutex_lock(&choice_mutex);
    size_t n = codec_store_loaded ? bt_codec_store_format(&codec_store, buf, sizeof(buf)) : 0;
    pthread_mutex_unlock(&choice_mutex);
    if (n > 0 && mt_state_prepare_write(BT_CODECS_FILE) && !mt_state_write_file(BT_CODECS_FILE, buf, n))
        DBG_LOG("bt_hiby: could not save %s\n", BT_CODECS_FILE);
    pthread_mutex_unlock(&save_mutex);
}

/* What our bluetoothd last reported for mac (0 when the report is for
 * another device or missing). */
static unsigned int read_remote_codecs(const char * mac, bool * complete) {
    *complete = false;
    char line[160], addr[18];
    unsigned int codecs = 0;
    FILE * f = fopen(REMOTE_INFO_PATH, "r");
    if (!f) return 0;
    bool got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got || !bt_codec_parse_remote(line, addr, &codecs, complete) || strcasecmp(addr, mac) != 0) return 0;
    return codecs;
}

/* Fills the view from the table merged with the latest report; true when
 * the table itself changed (the caller saves it if it may write). */
static bool view_locked(const char * mac, bt_codec_device_view_t * out) {
    load_store_locked();
    bool complete;
    unsigned int reported = read_remote_codecs(mac, &complete);
    bool changed = bt_codec_store_note_remote(&codec_store, mac, reported, complete);
    const bt_codec_device_t * d = bt_codec_store_find(&codec_store, mac);
    memset(out, 0, sizeof(*out));
    out->codecs = d ? d->codecs : 0;
    snprintf(out->choice, sizeof(out->choice), "%s", d ? d->choice : "auto");
    const char * resolved = bt_codec_resolve(out->codecs, out->choice, ldac_quality);
    snprintf(out->resolved, sizeof(out->resolved), "%s", resolved ? resolved : "");
    return changed;
}

static bool hiby_device_view(const char * mac, bt_codec_device_view_t * out) {
    if (!bt_valid_mac(mac) || !out) return false;
    pthread_mutex_lock(&choice_mutex);
    /* No card write on the UI thread: the table is left as it was, and the
     * next apply merges the same report again and saves it. */
    load_store_locked();
    bt_codec_store_t saved = codec_store;
    (void) view_locked(mac, out);
    codec_store = saved;
    pthread_mutex_unlock(&choice_mutex);
    return true;
}

static bool apply_device_codec(const char * mac) {
    if (!bt_valid_mac(mac)) return false;
    bt_codec_device_view_t view;
    pthread_mutex_lock(&choice_mutex);
    bool changed = view_locked(mac, &view);
    pthread_mutex_unlock(&choice_mutex);
    if (changed) save_store();
    if (!view.resolved[0]) return true; /* nothing known about it: leave the setting */
    /* bt_codec rewrites a file in /etc (flash): only when it changes. */
    char current[16];
    if (hiby_get_system_codec(current, sizeof(current)) && strcmp(current, view.resolved) == 0) return true;
    DBG_LOG("bt_hiby: codec for %s: %s (codecs 0x%x, choice %s, was %s)\n", mac, view.resolved, view.codecs,
            view.choice, current[0] ? current : "unset");
    return run_bt_codec(stock_codec_name(view.resolved));
}

static bool hiby_apply(const char * mac) {
    char connected[18];
    if (!mac) {
        bt_device_t d;
        if (!bt_stack()->connected_device(&d)) return true;
        snprintf(connected, sizeof(connected), "%s", d.mac);
        mac = connected;
    }
    return apply_device_codec(mac);
}

static bool hiby_set_device_codec(const char * mac, const char * choice) {
    if (!bt_valid_mac(mac) || !bt_codec_is_choice(choice)) return false;
    pthread_mutex_lock(&choice_mutex);
    load_store_locked();
    bool changed = bt_codec_store_set_choice(&codec_store, mac, choice);
    pthread_mutex_unlock(&choice_mutex);
    if (changed) save_store();
    bt_device_t d;
    if (!bt_stack()->connected_device(&d) || strcasecmp(d.mac, mac) != 0) return true;
    return apply_device_codec(mac);
}

static void hiby_set_ldac_quality(const char * quality) {
    if (!bt_codec_is_ldac_quality(quality)) return;
    pthread_mutex_lock(&choice_mutex);
    snprintf(ldac_quality, sizeof(ldac_quality), "%s", quality);
    pthread_mutex_unlock(&choice_mutex);
}

static void hiby_set_sample_rate(unsigned int rate) {
    requested_rate = (rate == 44100 || rate == 48000) ? rate : 0;
    write_prefer_rate();
}

static const bt_codec_policy_t hiby_codec_policy = {
    .restore_preference = hiby_restore_codec_preference,
    .set_codec = hiby_set_codec,
    .codec_supported = hiby_codec_supported,
    .get_system_codec = hiby_get_system_codec,
    .set_sample_rate = hiby_set_sample_rate,
    .choose_sample_rate = hiby_set_sample_rate,
    .set_ldac_quality = hiby_set_ldac_quality,
    .device_view = hiby_device_view,
    .set_device_codec = hiby_set_device_codec,
    .apply = hiby_apply,
};

const struct board_bt_platform_ops bt_platform_hiby_ops = {
    .name = "hiby-sys_server",
    .init_chip = hiby_init_chip,
    .power = hiby_power,
    .restore_boot_power = hiby_restore_boot_power,
    .set_power_persist = hiby_set_power_persist,
    .prepare = write_prefer_rate,
    .codec_policy = &hiby_codec_policy,
    .scan = hiby_scan,
    .connect = hiby_connect,
    .disconnect = hiby_disconnect,
    .forget = hiby_forget,
    .reconnect_paired = hiby_reconnect_paired,
    .recover_stack = NULL, /* sys_server and the stock scripts own bluetoothd */
};
