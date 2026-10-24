/* Bluetooth back-end tests, on a private dbus-daemon:
 *
 * - stack detection: nothing on the bus, a fake BlueZ 4 (bluez4_mock.py, a
 *   python-dbusmock template with Manager.DefaultAdapter and no
 *   ObjectManager), python-dbusmock's own bluez5 template;
 * - bt_stack_bluez5 through the public API (no platform, as on the host):
 *   power on/off, device list with paired/connected flags, the cache kept
 *   current by InterfacesAdded/Removed and PropertiesChanged, connect
 *   (Pair + Trusted + Connect), disconnect, forget, the BR/EDR inquiry, the
 *   generic reconnect of a paired headset, a bluetoothd restart;
 * - bt_stack_bluez4 (the moved R3 Pro reads) against the fake BlueZ 4;
 * - the BlueZ 4 AVRCP player: RegisterPlayer's arguments as BlueZ 4.101's
 *   media.c parses them, the TrackChanged/PropertyChanged signals, Release
 *   and SetProperty, and the re-register triggers.
 *
 * The code under test reaches the bus through MT_BT_DBUS_ADDRESS (the only
 * user of that variable); the mocks through DBUS_SYSTEM_BUS_ADDRESS. */
#include "bt_avrcp_bluez4.h"
#include "bt_backend.h"
#include "bt_dbus.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- stubs for what the back ends call outside Bluetooth ------------------- */

float audio_get_volume(void);
void audio_set_volume(float volume);
void hiby_sys_server_report_volume(int percent);
void hiby_sys_server_command(const char * cmd);
bool hiby_sys_request(const char * cmd, const char * const * result_prefixes, int timeout_ms, char * out,
                      size_t out_size);
bool mt_state_prepare_write(const char * path);
bool mt_state_write_file(const char * path, const void * data, size_t len);
bool mt_state_available(void);

float audio_get_volume(void) { return 0.5f; }
void audio_set_volume(float volume) { (void) volume; }
void hiby_sys_server_report_volume(int percent) { (void) percent; }
void hiby_sys_server_command(const char * cmd) { (void) cmd; }
bool hiby_sys_request(const char * cmd, const char * const * result_prefixes, int timeout_ms, char * out,
                      size_t out_size) {
    (void) cmd;
    (void) result_prefixes;
    (void) timeout_ms;
    (void) out;
    (void) out_size;
    return false;
}
bool mt_state_prepare_write(const char * path) {
    (void) path;
    return false;
}
bool mt_state_write_file(const char * path, const void * data, size_t len) {
    (void) path;
    (void) data;
    (void) len;
    return false;
}
bool mt_state_available(void) {
    return false;
}

/* ---- checks ------------------------------------------------------------------ */

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            fprintf(stderr, __VA_ARGS__);                                                                          \
            fputc('\n', stderr);                                                                                   \
        }                                                                                                          \
    } while (0)

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* Retries `expr` for up to 3 s: signals reach the stack's connection
 * asynchronously. */
#define EVENTUALLY(expr)                                                                                           \
    ({                                                                                                             \
        bool ok_ = false;                                                                                          \
        for (int i_ = 0; i_ < 150 && !(ok_ = (expr)); i_++) sleep_ms(20);                                          \
        ok_;                                                                                                       \
    })

/* ---- the private bus and the mocks ------------------------------------------- */

static char tmpdir[256];
static char bus_address[512];
static pid_t daemon_pid = -1;
static DBusConnection * test_bus; /* the test's own connection (mock control) */

static pid_t spawn(char * const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setenv("DBUS_SYSTEM_BUS_ADDRESS", bus_address, 1);
        if (!getenv("MT_BT_TEST_VERBOSE") && !freopen("/dev/null", "w", stdout)) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void stop(pid_t * pid) {
    if (*pid <= 0) return;
    kill(*pid, SIGTERM);
    waitpid(*pid, NULL, 0);
    *pid = -1;
}

static bool name_owned(const char * name) {
    return bt_dbus_name_has_owner(test_bus, name);
}

static bool start_bus(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mt-bt-test-XXXXXX");
    if (!mkdtemp(tmpdir)) return false;
    char conf[300], socket_path[300];
    snprintf(conf, sizeof(conf), "%s/bus.conf", tmpdir);
    snprintf(socket_path, sizeof(socket_path), "%s/bus", tmpdir);
    snprintf(bus_address, sizeof(bus_address), "unix:path=%s", socket_path);
    FILE * f = fopen(conf, "w");
    if (!f) return false;
    fprintf(f,
            "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
            " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
            "<busconfig>\n"
            "  <type>system</type>\n"
            "  <listen>%s</listen>\n"
            "  <auth>EXTERNAL</auth>\n"
            "  <policy context=\"default\">\n"
            "    <allow user=\"*\"/>\n"
            "    <allow own=\"*\"/>\n"
            "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
            "    <allow receive_sender=\"*\"/>\n"
            "  </policy>\n"
            "</busconfig>\n",
            bus_address);
    fclose(f);
    char conf_arg[320];
    snprintf(conf_arg, sizeof(conf_arg), "--config-file=%s", conf);
    char * argv[] = { "dbus-daemon", conf_arg, "--nofork", "--nopidfile", NULL };
    daemon_pid = spawn(argv);
    for (int i = 0; i < 250 && access(socket_path, F_OK) != 0; i++) sleep_ms(20);
    setenv(BT_DBUS_ADDRESS_ENV, bus_address, 1);
    for (int i = 0; i < 100 && !test_bus; i++) {
        test_bus = bt_dbus_open();
        if (!test_bus) sleep_ms(20);
    }
    return test_bus != NULL;
}

static pid_t start_mock(const char * template_arg) {
    char * argv[] = { "python3", "-m", "dbusmock", "--system", "--template", (char *) template_arg, NULL };
    pid_t pid = spawn(argv);
    for (int i = 0; i < 500 && !name_owned("org.bluez"); i++) sleep_ms(20);
    return pid;
}

static void stop_mock(pid_t * pid) {
    stop(pid);
    for (int i = 0; i < 250 && name_owned("org.bluez"); i++) sleep_ms(20);
}

/* org.bluez.Mock / org.freedesktop.DBus.Mock calls with string arguments. */
static bool mock_call(const char * path, const char * iface, const char * method, int nargs, ...) {
    DBusMessage * m = dbus_message_new_method_call("org.bluez", path, iface, method);
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) {
        const char * a = va_arg(ap, const char *);
        dbus_message_append_args(m, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID);
    }
    va_end(ap);
    char err[128];
    DBusMessage * r = bt_dbus_call(test_bus, m, 10000, err, sizeof(err));
    if (!r) {
        fprintf(stderr, "mock %s.%s: %s\n", iface, method, err);
        return false;
    }
    dbus_message_unref(r);
    return true;
}

#define BLUEZ_MOCK "org.bluez.Mock"
#define DBUSMOCK "org.freedesktop.DBus.Mock"

/* UpdateProperties(org.bluez.Device1, {Trusted: true, UUIDs: [sink]}). */
static bool mock_make_headset(const char * dev_path) {
    DBusMessage * m = dbus_message_new_method_call("org.bluez", dev_path, DBUSMOCK, "UpdateProperties");
    DBusMessageIter it, dict, entry, var, arr;
    const char * iface = "org.bluez.Device1", * key = "UUIDs", * uuid = BT_A2DP_SINK_UUID;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    bt_dbus_dict_append_bool(&dict, "Trusted", true);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &uuid);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(&dict, &entry);
    dbus_message_iter_close_container(&it, &dict);
    DBusMessage * r = bt_dbus_call(test_bus, m, 10000, NULL, 0);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
}

/* GetMethodCalls(method) on a mock object: a(tav). Returns the number of
 * calls; *last (optional, caller unrefs) gets the reply positioned for
 * method_call_args() to read the last call's arguments. */
static int mock_calls(const char * path, const char * method, DBusMessage ** reply) {
    DBusMessage * m = dbus_message_new_method_call("org.bluez", path, DBUSMOCK, "GetMethodCalls");
    dbus_message_append_args(m, DBUS_TYPE_STRING, &method, DBUS_TYPE_INVALID);
    DBusMessage * r = bt_dbus_call(test_bus, m, 10000, NULL, 0);
    if (!r) return -1;
    DBusMessageIter it, arr;
    int n = 0;
    dbus_message_iter_init(r, &it);
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
        n++;
        dbus_message_iter_next(&arr);
    }
    if (reply) *reply = r;
    else dbus_message_unref(r);
    return n;
}

/* The av of call `index` from a mock_calls() reply. */
static bool call_args(DBusMessage * r, int index, DBusMessageIter * args) {
    DBusMessageIter it, arr, st;
    dbus_message_iter_init(r, &it);
    dbus_message_iter_recurse(&it, &arr);
    for (int i = 0; i < index; i++) dbus_message_iter_next(&arr);
    if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_STRUCT) return false;
    dbus_message_iter_recurse(&arr, &st);
    dbus_message_iter_next(&st); /* timestamp */
    if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY) return false;
    dbus_message_iter_recurse(&st, args);
    return true;
}

/* ---- dictionaries as test data ------------------------------------------------- */

typedef struct {
    char keys[16][32];
    char types[16]; /* D-Bus type code of each value */
    char strs[16][128];
    unsigned int nums[16];
    int n;
} dict_t;

static void dict_cb(const char * key, DBusMessageIter * v, void * ctx) {
    dict_t * d = ctx;
    if (d->n >= 16) return;
    snprintf(d->keys[d->n], sizeof(d->keys[0]), "%s", key);
    int t = dbus_message_iter_get_arg_type(v);
    d->types[d->n] = (char) t;
    d->strs[d->n][0] = '\0';
    d->nums[d->n] = 0;
    if (t == DBUS_TYPE_STRING) {
        snprintf(d->strs[d->n], sizeof(d->strs[0]), "%s", bt_dbus_iter_str(v));
    } else if (t == DBUS_TYPE_UINT32) {
        dbus_uint32_t u;
        dbus_message_iter_get_basic(v, &u);
        d->nums[d->n] = u;
    } else if (t == DBUS_TYPE_ARRAY && dbus_message_iter_get_element_type(v) == DBUS_TYPE_STRING) {
        DBusMessageIter a; /* first element: dbusmock may hand strings back as "as" */
        dbus_message_iter_recurse(v, &a);
        snprintf(d->strs[d->n], sizeof(d->strs[0]), "%s", bt_dbus_iter_str(&a));
    }
    d->n++;
}

/* Reads an a{sv}, also when wrapped in a variant (dbusmock's av). */
static bool read_dict(DBusMessageIter * v, dict_t * d) {
    memset(d, 0, sizeof(*d));
    DBusMessageIter inner;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(v, &inner);
        v = &inner;
    }
    return bt_dbus_foreach_prop(v, dict_cb, d);
}

static int dict_find(const dict_t * d, const char * key) {
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->keys[i], key) == 0) return i;
    return -1;
}

/* ---- tests ------------------------------------------------------------------------ */

static const char * template_bluez4;

static void test_detection_nothing(void) {
    bt_stack_reset_detection();
    CHECK(bt_stack_detect() == BT_STACK_UNKNOWN, "no org.bluez: unknown");
    CHECK(bt_stack()->kind == BT_STACK_UNKNOWN, "unknown table");
    CHECK(!bt_control_is_powered(), "unknown stack: not powered");
    CHECK(bt_control_any_paired_connected() == -1, "unknown stack: -1");
    bt_device_t d[4];
    CHECK(bt_control_list_paired_states(d, 4) == -1, "unknown stack: paired list -1");
    CHECK(!bt_control_connect("AA:BB:CC:DD:EE:01"), "unknown stack: no connect");
}

static void test_bluez4(void) {
    pid_t mock = start_mock(template_bluez4);
    CHECK(name_owned("org.bluez"), "BlueZ 4 mock up");
    bt_stack_reset_detection();
    CHECK(bt_stack_detect() == BT_STACK_BLUEZ4, "BlueZ 4 detected (got %d)", (int) bt_stack_kind_cached());

    /* The moved R3 Pro reads. */
    const bt_stack_ops_t * st = bt_stack();
    CHECK(st == &bt_stack_bluez4, "BlueZ 4 table");
    CHECK(st->power_state(1000) == BT_POWER_ON, "BlueZ 4 powered");
    bt_stack_device_t devs[8];
    int n = st->list_devices(devs, 8, false);
    CHECK(n == 2, "BlueZ 4 devices %d", n);
    int hp = n > 0 && strcmp(devs[0].dev.mac, "AA:BB:CC:DD:EE:02") == 0 ? 0 : 1;
    if (n == 2) {
        CHECK(strcmp(devs[hp].dev.name, "My Headphones") == 0, "alias wins: %s", devs[hp].dev.name);
        CHECK(devs[hp].dev.paired && devs[hp].audio_sink && devs[hp].trusted, "headphones paired sink");
        CHECK(devs[hp].dev.connected, "AudioSink State playing = connected");
        CHECK(!devs[1 - hp].dev.paired && !devs[1 - hp].audio_sink && !devs[1 - hp].dev.connected, "phone");
        CHECK(strcmp(devs[1 - hp].dev.name, "Phone") == 0, "name when no alias: %s", devs[1 - hp].dev.name);
    }
    CHECK(st->list_devices(devs, 8, true) == 1, "BlueZ 4 paired only");
    bt_device_t d;
    CHECK(st->connected_device(&d) && strcmp(d.mac, "AA:BB:CC:DD:EE:02") == 0, "connected device");
    CHECK(st->is_connected() && st->any_paired_connected() == 1, "connected");
    CHECK(!st->connect && !st->pair && !st->remove, "BlueZ 4 actions belong to the platform");
    CHECK(!(bt_control_backend_features() & (BT_BACKEND_RFCOMM_REMOTE)), "no RFCOMM profile on BlueZ 4");

    /* The AVRCP player (bt_avrcp_bluez4.c). */
    DBusConnection * player = bt_dbus_open();
    DBusConnection * listener = bt_dbus_open();
    dbus_bus_add_match(listener, "type='signal',interface='" BT_AVRCP4_PLAYER_INTERFACE "'", NULL);
    dbus_connection_flush(listener);
    char adapter[96];
    CHECK(bt_avrcp4_default_adapter(player, adapter, sizeof(adapter)) &&
              strcmp(adapter, "/org/bluez/1234/hci0") == 0,
          "default adapter %s", adapter);
    bt_avrcp4_state_t state = {
        .has_track = true, .playing = true, .title = "Title", .artist = "Artist", .album = "Album",
        .genre = "", .number = 7, .duration_ms = 215000, .position_ms = 12000,
    };
    const char * object = "/org/mellowtone/MediaPlayer";
    char err[128];
    CHECK(bt_avrcp4_register(player, adapter, object, &state, 3000, err, sizeof(err)), "RegisterPlayer: %s", err);
    DBusMessage * calls = NULL;
    CHECK(mock_calls(adapter, "RegisterPlayer", &calls) == 1, "one RegisterPlayer");
    DBusMessageIter args;
    if (calls && call_args(calls, 0, &args)) {
        DBusMessageIter v;
        dbus_message_iter_recurse(&args, &v);
        CHECK(strcmp(bt_dbus_iter_str(&v), object) == 0, "player path");
        dbus_message_iter_next(&args);
        dict_t props, meta;
        CHECK(read_dict(&args, &props), "properties dict");
        /* Only keys media.c's set_property() accepts, else InvalidArguments. */
        CHECK(props.n == 1 && dict_find(&props, "Status") == 0 && strcmp(props.strs[0], "playing") == 0 &&
                  props.types[0] == DBUS_TYPE_STRING,
              "properties = {Status: playing} (n=%d)", props.n);
        dbus_message_iter_next(&args);
        CHECK(read_dict(&args, &meta), "metadata dict");
        int t = dict_find(&meta, "Title"), a = dict_find(&meta, "Artist"), al = dict_find(&meta, "Album");
        int num = dict_find(&meta, "Number"), dur = dict_find(&meta, "Duration");
        CHECK(t >= 0 && strcmp(meta.strs[t], "Title") == 0 && meta.types[t] == DBUS_TYPE_STRING, "Title");
        CHECK(a >= 0 && strcmp(meta.strs[a], "Artist") == 0, "Artist");
        CHECK(al >= 0 && strcmp(meta.strs[al], "Album") == 0, "Album");
        CHECK(dict_find(&meta, "Genre") < 0, "empty Genre left out");
        CHECK(num >= 0 && meta.types[num] == DBUS_TYPE_UINT32 && meta.nums[num] == 7, "Number uint32 7");
        CHECK(dur >= 0 && meta.types[dur] == DBUS_TYPE_UINT32 && meta.nums[dur] == 215000, "Duration uint32 ms");
        CHECK(meta.n == 5, "nothing media.c would reject (n=%d)", meta.n);
    }
    if (calls) dbus_message_unref(calls);

    /* Position follows the registration (not in the properties dict). */
    bt_avrcp4_emit_track_changed(player, object, &state);
    state.playing = false;
    bt_avrcp4_emit_status(player, object, &state);
    dbus_connection_flush(player);
    int got_position = 0, got_track = 0, got_status = 0;
    for (int i = 0; i < 100 && !(got_position >= 2 && got_track && got_status); i++) {
        dbus_connection_read_write(listener, 20);
        DBusMessage * s;
        while ((s = dbus_connection_pop_message(listener))) {
            if (dbus_message_is_signal(s, BT_AVRCP4_PLAYER_INTERFACE, "PropertyChanged")) {
                const char * name = NULL;
                DBusMessageIter it, var;
                dbus_message_iter_init(s, &it);
                dbus_message_iter_get_basic(&it, &name);
                dbus_message_iter_next(&it);
                dbus_message_iter_recurse(&it, &var);
                if (strcmp(name, "Position") == 0) {
                    dbus_uint32_t pos = 0;
                    CHECK(dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_UINT32, "Position is uint32");
                    dbus_message_iter_get_basic(&var, &pos);
                    CHECK(pos == 12000, "Position ms %u", (unsigned) pos);
                    got_position++;
                } else if (strcmp(name, "Status") == 0) {
                    CHECK(strcmp(bt_dbus_iter_str(&var), "paused") == 0, "Status paused");
                    got_status++;
                }
                CHECK(strcmp(dbus_message_get_path(s), object) == 0, "signal from the player object");
            } else if (dbus_message_is_signal(s, BT_AVRCP4_PLAYER_INTERFACE, "TrackChanged")) {
                DBusMessageIter it;
                dict_t meta;
                dbus_message_iter_init(s, &it);
                CHECK(read_dict(&it, &meta) && dict_find(&meta, "Title") >= 0 && meta.n == 5, "TrackChanged dict");
                got_track++;
            }
            dbus_message_unref(s);
        }
    }
    CHECK(got_position == 2, "Position after RegisterPlayer and after TrackChanged (%d)", got_position);
    CHECK(got_track == 1 && got_status == 1, "TrackChanged %d, Status %d", got_track, got_status);

    /* No track: empty metadata, "stopped". */
    bt_avrcp4_state_t idle = { .has_track = false };
    CHECK(strcmp(bt_avrcp4_status(&idle), "stopped") == 0, "no track: stopped");
    CHECK(bt_avrcp4_register(player, adapter, "/org/mellowtone/Idle", &idle, 3000, NULL, 0), "idle register");
    calls = NULL;
    CHECK(mock_calls(adapter, "RegisterPlayer", &calls) == 2, "second RegisterPlayer");
    if (calls && call_args(calls, 1, &args)) {
        dict_t meta;
        dbus_message_iter_next(&args);
        dbus_message_iter_next(&args);
        CHECK(read_dict(&args, &meta) && meta.n == 0, "no track: empty metadata");
    }
    if (calls) dbus_message_unref(calls);

    /* bluetoothd calls Release / SetProperty on our object. */
    DBusMessage * rel = dbus_message_new_method_call(dbus_bus_get_unique_name(player), object,
                                                     BT_AVRCP4_PLAYER_INTERFACE, "Release");
    DBusMessage * set = dbus_message_new_method_call(dbus_bus_get_unique_name(player), object,
                                                     BT_AVRCP4_PLAYER_INTERFACE, "SetProperty");
    const char * prop = "Repeat";
    dbus_message_append_args(set, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusPendingCall * p_rel = NULL, * p_set = NULL;
    dbus_connection_send_with_reply(listener, rel, &p_rel, 3000);
    dbus_connection_send_with_reply(listener, set, &p_set, 3000);
    dbus_connection_flush(listener);
    dbus_message_unref(rel);
    dbus_message_unref(set);
    bool released = false;
    int handled = 0;
    for (int i = 0; i < 100 && handled < 2; i++) {
        dbus_connection_read_write(player, 20);
        DBusMessage * msg;
        while ((msg = dbus_connection_pop_message(player))) {
            if (bt_avrcp4_handle_message(player, msg, &released) == DBUS_HANDLER_RESULT_HANDLED) handled++;
            dbus_message_unref(msg);
        }
        dbus_connection_flush(player);
    }
    CHECK(handled == 2 && released, "Release handled (%d, %d)", handled, released);
    dbus_pending_call_block(p_rel);
    dbus_pending_call_block(p_set);
    DBusMessage * r_rel = dbus_pending_call_steal_reply(p_rel), * r_set = dbus_pending_call_steal_reply(p_set);
    CHECK(r_rel && dbus_message_get_type(r_rel) == DBUS_MESSAGE_TYPE_METHOD_RETURN, "Release answered");
    CHECK(r_set && dbus_message_get_type(r_set) == DBUS_MESSAGE_TYPE_ERROR &&
              strcmp(dbus_message_get_error_name(r_set), "org.bluez.Error.NotSupported") == 0,
          "SetProperty refused");
    if (r_rel) dbus_message_unref(r_rel);
    if (r_set) dbus_message_unref(r_set);
    dbus_pending_call_unref(p_rel);
    dbus_pending_call_unref(p_set);

    CHECK(bt_avrcp4_unregister(player, adapter, object, 3000), "UnregisterPlayer");
    CHECK(mock_calls(adapter, "UnregisterPlayer", NULL) == 1, "one UnregisterPlayer");

    /* Re-register triggers. */
    DBusMessage * sig = dbus_message_new_signal("/", "org.bluez.Manager", "AdapterAdded");
    CHECK(bt_avrcp4_needs_reregister(sig), "AdapterAdded");
    dbus_message_unref(sig);
    sig = dbus_message_new_signal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged");
    const char * nm = "org.bluez", * old_owner = ":1.1", * gone = "", * back = ":1.9";
    dbus_message_append_args(sig, DBUS_TYPE_STRING, &nm, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &gone,
                             DBUS_TYPE_INVALID);
    CHECK(!bt_avrcp4_needs_reregister(sig), "bluetoothd gone: nothing to register with");
    dbus_message_unref(sig);
    sig = dbus_message_new_signal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged");
    dbus_message_append_args(sig, DBUS_TYPE_STRING, &nm, DBUS_TYPE_STRING, &gone, DBUS_TYPE_STRING, &back,
                             DBUS_TYPE_INVALID);
    CHECK(bt_avrcp4_needs_reregister(sig), "bluetoothd back: register again");
    dbus_message_unref(sig);

    dbus_connection_close(player);
    dbus_connection_unref(player);
    dbus_connection_close(listener);
    dbus_connection_unref(listener);
    stop_mock(&mock);
}

/* A fake audio transport for the generic reconnect: what bluealsactl
 * would report. */
static int fake_output = 0;
static int fake_output_connected(int timeout_ms) {
    (void) timeout_ms;
    return fake_output;
}
static void fake_note(const char * mac) {
    (void) mac;
}
static bt_audio_ops_t fake_audio;

static bool find_dev(const char * mac, bt_device_t * out, bool paired_only) {
    bt_device_t devs[16];
    int n = paired_only ? bt_control_list_paired_states(devs, 16) : bt_control_list_devices(devs, 16);
    for (int i = 0; i < n; i++) {
        if (strcasecmp(devs[i].mac, mac) == 0) {
            if (out) *out = devs[i];
            return true;
        }
    }
    return false;
}

static bool connected(const char * mac) {
    bt_device_t d;
    return find_dev(mac, &d, false) && d.connected;
}

static bool paired(const char * mac) {
    bt_device_t d;
    return find_dev(mac, &d, true) && d.paired;
}

static bool cancel_never(void * ctx) {
    (void) ctx;
    return false;
}

static void test_bluez5(void) {
    pid_t mock = start_mock("bluez5");
    CHECK(name_owned("org.bluez"), "BlueZ 5 mock up");
    bt_stack_reset_detection();
    /* No adapter yet: ObjectManager answers with /org/bluez's managers. */
    CHECK(bt_stack_detect() == BT_STACK_BLUEZ5, "BlueZ 5 detected (got %d)", (int) bt_stack_kind_cached());
    CHECK(bt_control_backend_features() & BT_BACKEND_RFCOMM_REMOTE, "RFCOMM profile on BlueZ 5");
    CHECK(bt_stack()->power_state(1000) == BT_POWER_NO_ADAPTER, "no adapter yet");

    /* Adapter: InterfacesAdded reaches the cache. */
    CHECK(mock_call("/", BLUEZ_MOCK, "AddAdapter", 2, "hci0", "mellowtone"), "AddAdapter");
    CHECK(EVENTUALLY(bt_control_is_powered()), "adapter powered (template default)");
    bt_control_disable();
    CHECK(EVENTUALLY(!bt_control_is_powered()), "Powered off through the stack");
    bt_control_enable();
    CHECK(EVENTUALLY(bt_control_is_powered()), "Powered on through the stack");

    /* Devices appear, pair, connect -- by signals. */
    const char * hp = "AA:BB:CC:DD:EE:01", * spk = "AA:BB:CC:DD:EE:02";
    const char * hp_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01";
    CHECK(mock_call("/", BLUEZ_MOCK, "AddDevice", 3, "hci0", hp, "Headphones"), "AddDevice");
    CHECK(EVENTUALLY(find_dev(hp, NULL, false)), "new device listed");
    bt_device_t d;
    CHECK(find_dev(hp, &d, false) && !d.paired && !d.connected && strcmp(d.name, "Headphones") == 0,
          "unpaired, unconnected, named");
    CHECK(!find_dev(hp, NULL, true), "not in the paired list yet");
    CHECK(mock_call("/", BLUEZ_MOCK, "PairDevice", 2, "hci0", hp), "PairDevice");
    CHECK(EVENTUALLY(paired(hp)), "paired via PropertiesChanged");
    CHECK(!bt_control_is_connected() && bt_control_any_paired_connected() == 0, "nothing connected");
    CHECK(mock_call("/", BLUEZ_MOCK, "ConnectDevice", 2, "hci0", hp), "ConnectDevice");
    CHECK(EVENTUALLY(bt_control_is_connected()), "connected via PropertiesChanged");
    CHECK(bt_control_any_paired_connected() == 1, "any connected");
    bt_device_t paired_list[8];
    int n = bt_control_list_paired_states(paired_list, 8);
    CHECK(n == 1 && paired_list[0].paired && paired_list[0].connected, "paired list with flags (%d)", n);
    CHECK(mock_call("/", BLUEZ_MOCK, "DisconnectDevice", 2, "hci0", hp), "DisconnectDevice");
    CHECK(EVENTUALLY(!bt_control_is_connected()), "disconnected via PropertiesChanged");

    /* Connect from the player: Pair, Trusted, Connect. */
    CHECK(mock_call("/", BLUEZ_MOCK, "AddDevice", 3, "hci0", spk, "Speaker"), "AddDevice speaker");
    CHECK(EVENTUALLY(find_dev(spk, NULL, false)), "speaker listed");
    CHECK(bt_control_connect(spk), "bt_control_connect");
    CHECK(EVENTUALLY(paired(spk) && connected(spk)), "speaker paired and connected");
    bt_stack_device_t sd;
    CHECK(EVENTUALLY(bt_stack()->device_info(spk, &sd, 0) && sd.trusted), "speaker trusted");
    CHECK(mock_calls("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02", "Pair", NULL) == 1, "Pair called once");
    CHECK(bt_control_connect(spk), "connecting a connected device succeeds (AlreadyConnected)");
    CHECK(mock_calls("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02", "Pair", NULL) == 1, "no second Pair");
    CHECK(bt_control_disconnect(spk), "bt_control_disconnect");
    CHECK(EVENTUALLY(!connected(spk)), "speaker disconnected");
    CHECK(!bt_control_disconnect(spk), "disconnecting a disconnected device fails (NotConnected)");
    /* Malformed addresses never reach the bus (libdbus aborts on a bad
     * object path). */
    static const char * const bad_macs[] = { "", "AA:BB:CC:DD:EE", "AA:BB:CC:DD:EE:0G", "AA-BB-CC-DD-EE-02",
                                             "AA:BB:CC:DD:EE:02/x", NULL };
    for (int i = 0; bad_macs[i]; i++)
        CHECK(!bt_control_connect(bad_macs[i]) && !bt_control_disconnect(bad_macs[i]) &&
                  !bt_control_forget(bad_macs[i]),
              "malformed address '%s' refused", bad_macs[i]);
    CHECK(!bt_control_connect(NULL) && !bt_control_disconnect(NULL) && !bt_control_forget(NULL), "NULL address refused");

    /* Inquiry: classic only. */
    bt_device_t found[8];
    n = bt_control_scan(1, found, 8);
    CHECK(n == 2, "scan lists known devices (%d)", n);
    DBusMessage * calls = NULL;
    CHECK(mock_calls("/org/bluez/hci0", "StartDiscovery", NULL) == 1, "StartDiscovery");
    CHECK(mock_calls("/org/bluez/hci0", "StopDiscovery", NULL) == 1, "StopDiscovery");
    CHECK(mock_calls("/org/bluez/hci0", "SetDiscoveryFilter", &calls) == 1, "SetDiscoveryFilter");
    DBusMessageIter args;
    if (calls && call_args(calls, 0, &args)) {
        dict_t filter;
        int t;
        CHECK(read_dict(&args, &filter) && (t = dict_find(&filter, "Transport")) >= 0 &&
                  strcmp(filter.strs[t], "bredr") == 0,
              "Transport bredr");
    }
    if (calls) dbus_message_unref(calls);
    CHECK(bt_control_list_devices(found, 8) == 2, "list without inquiry");
    CHECK(mock_calls("/org/bluez/hci0", "StartDiscovery", NULL) == 1, "no inquiry for list_devices");

    /* Generic reconnect (no platform override), with a fake transport. */
    fake_audio = bt_audio_bluealsa;
    fake_audio.output_connected = fake_output_connected;
    fake_audio.note_requested_disconnect = fake_note;
    bt_audio_set_override(&fake_audio);
    CHECK(!bt_control_reconnect_paired(NULL, cancel_never, NULL), "no eligible headset (not trusted/sink)");
    CHECK(mock_make_headset(hp_path), "headphones: trusted A2DP sink");
    CHECK(EVENTUALLY(bt_stack()->device_info(hp, &sd, 0) && sd.audio_sink && sd.trusted), "headset flags");
    CHECK(bt_control_reconnect_paired(NULL, cancel_never, NULL), "one eligible headset reconnected");
    CHECK(EVENTUALLY(connected(hp)), "headphones connected by the reconnect");
    fake_output = 1;
    CHECK(bt_control_reconnect_paired(hp, cancel_never, NULL), "output already up: nothing to do");
    fake_output = -1;
    CHECK(!bt_control_reconnect_paired(hp, cancel_never, NULL), "output unknown: no guess");
    fake_output = 0;
    CHECK(!bt_control_reconnect_paired("bogus", cancel_never, NULL), "bad MAC refused");
    bt_audio_set_override(NULL);

    /* Forget: RemoveDevice -> InterfacesRemoved. */
    CHECK(bt_control_forget(spk), "bt_control_forget");
    CHECK(EVENTUALLY(!find_dev(spk, NULL, false)), "speaker gone from the cache");
    CHECK(mock_calls("/org/bluez/hci0", "RemoveDevice", NULL) == 1, "RemoveDevice");

    /* bluetoothd restart: a new owner of org.bluez drops the cache. */
    stop_mock(&mock);
    CHECK(!bt_control_is_powered(), "bluetoothd gone: not powered");
    CHECK(bt_control_any_paired_connected() == -1, "bluetoothd gone: unknown");
    mock = start_mock("bluez5");
    CHECK(mock_call("/", BLUEZ_MOCK, "AddAdapter", 2, "hci0", "mellowtone"), "AddAdapter again");
    CHECK(EVENTUALLY(bt_control_is_powered()), "powered after the restart");
    CHECK(bt_control_list_devices(found, 8) == 0, "fresh bluetoothd: no devices (old cache dropped)");
    CHECK(mock_call("/", BLUEZ_MOCK, "AddDevice", 3, "hci0", spk, "Speaker"), "AddDevice after restart");
    CHECK(EVENTUALLY(find_dev(spk, NULL, false)), "signals after the restart");

    /* Adapter removed. */
    CHECK(mock_call("/", BLUEZ_MOCK, "RemoveAdapterWithDevices", 1, "hci0"), "RemoveAdapter");
    CHECK(EVENTUALLY(bt_stack()->power_state(1000) == BT_POWER_NO_ADAPTER), "adapter gone");
    CHECK(bt_control_list_devices(found, 8) == 0, "no devices without an adapter");
    stop_mock(&mock);
}

int main(int argc, char ** argv) {
    template_bluez4 = argc > 1 ? argv[1] : "bluez4_mock.py";
    signal(SIGPIPE, SIG_IGN);
    if (!start_bus()) {
        fprintf(stderr, "could not start dbus-daemon\n");
        return 1;
    }
    test_detection_nothing();
    test_bluez4();
    test_bluez5();
    /* After BlueZ 5, BlueZ 4 again: detection is kept once positive. */
    CHECK(bt_stack_kind_cached() == BT_STACK_BLUEZ5, "kind kept");

    dbus_connection_close(test_bus);
    dbus_connection_unref(test_bus);
    stop(&daemon_pid);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", tmpdir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
