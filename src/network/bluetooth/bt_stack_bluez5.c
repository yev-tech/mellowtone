/* BlueZ 5 stack (the X1600 boards, a Linux PC): org.bluez over D-Bus
 * directly, replacing the bluetoothctl and hciconfig subprocesses.
 *
 * A cache of the adapter and its devices is built from one
 * ObjectManager.GetManagedObjects and then kept current by the
 * InterfacesAdded / InterfacesRemoved / PropertiesChanged signals, read
 * without blocking at the start of every call (no thread of its own). So
 * the UI's polls (paired list, "anything connected?") cost a mutex and a
 * non-blocking socket read instead of one fork per device. A new owner of
 * org.bluez (bluetoothd restarted) drops the cache; the next call
 * rebuilds it.
 *
 * Blocking calls (Pair, Connect, ...) run on a reference to the connection
 * without holding the cache mutex, so polls never wait behind them.
 * power_state() is a real round trip (Properties.Get Powered): it is also
 * the liveness check the wedge watchdog in bluetooth_control.c counts on. */
#include "bt_backend.h"
#include "bt_dbus.h"
#include "debug_log.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BZ5_SERVICE "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define SYNC_TIMEOUT_MS 3000
#define CALL_TIMEOUT_MS 5000
#define MAX_DEVICES 64

typedef struct {
    char path[128];
    char adapter[64];
    char mac[18];
    char alias[64];
    char name[64];
    bool paired, trusted, blocked, connected, audio_sink;
} bz5_device_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    DBusConnection * conn;
    bool synced;
    char adapter[64]; /* "" = no Adapter1 */
    bool powered;
    bz5_device_t devs[MAX_DEVICES];
    int ndev;
} S;

/* ---- cache updates (s_mutex held: they run from pump_locked()) ----------------- */

static bz5_device_t * find_dev_path(const char * path) {
    for (int i = 0; i < S.ndev; i++)
        if (strcmp(S.devs[i].path, path) == 0) return &S.devs[i];
    return NULL;
}

static bz5_device_t * find_dev_mac(const char * mac) {
    for (int i = 0; i < S.ndev; i++)
        if (strcasecmp(S.devs[i].mac, mac) == 0 && (!S.adapter[0] || strcmp(S.devs[i].adapter, S.adapter) == 0))
            return &S.devs[i];
    return NULL;
}

static void remove_dev_path(const char * path) {
    for (int i = 0; i < S.ndev; i++) {
        if (strcmp(S.devs[i].path, path) == 0) {
            S.devs[i] = S.devs[--S.ndev];
            return;
        }
    }
}

static void device_prop(const char * key, DBusMessageIter * v, void * ctx) {
    bz5_device_t * d = ctx;
    if (strcmp(key, "Address") == 0) {
        snprintf(d->mac, sizeof(d->mac), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Alias") == 0) {
        snprintf(d->alias, sizeof(d->alias), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Name") == 0) {
        snprintf(d->name, sizeof(d->name), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Adapter") == 0) {
        snprintf(d->adapter, sizeof(d->adapter), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Paired") == 0) {
        d->paired = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Trusted") == 0) {
        d->trusted = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Blocked") == 0) {
        d->blocked = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Connected") == 0) {
        d->connected = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "UUIDs") == 0 && dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        d->audio_sink = false;
        dbus_message_iter_recurse(v, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            if (strcasecmp(bt_dbus_iter_str(&arr), BT_A2DP_SINK_UUID) == 0) d->audio_sink = true;
            dbus_message_iter_next(&arr);
        }
    }
}

static void adapter_prop(const char * key, DBusMessageIter * v, void * ctx) {
    (void) ctx;
    if (strcmp(key, "Powered") == 0) S.powered = bt_dbus_iter_bool(v);
}

/* The first adapter seen is used; hci0 wins over others. */
static void adopt_adapter(const char * path) {
    if (!S.adapter[0] || (strstr(path, "/hci0") && !strstr(S.adapter, "/hci0")))
        snprintf(S.adapter, sizeof(S.adapter), "%s", path);
}

/* One object's a{sa{sv}} (InterfacesAdded, GetManagedObjects). */
static void add_interfaces(const char * path, DBusMessageIter * ifaces_array) {
    DBusMessageIter ifaces;
    dbus_message_iter_recurse(ifaces_array, &ifaces);
    while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&ifaces, &entry);
        const char * iface = bt_dbus_iter_str(&entry);
        dbus_message_iter_next(&entry);
        if (strcmp(iface, ADAPTER_IFACE) == 0) {
            adopt_adapter(path);
            if (strcmp(S.adapter, path) == 0) bt_dbus_foreach_prop(&entry, adapter_prop, NULL);
        } else if (strcmp(iface, DEVICE_IFACE) == 0) {
            bz5_device_t * d = find_dev_path(path);
            if (!d && S.ndev < MAX_DEVICES) {
                d = &S.devs[S.ndev++];
                memset(d, 0, sizeof(*d));
                snprintf(d->path, sizeof(d->path), "%s", path);
            }
            if (d) bt_dbus_foreach_prop(&entry, device_prop, d);
        }
        dbus_message_iter_next(&ifaces);
    }
}

static void clear_cache(void) {
    S.synced = false;
    S.adapter[0] = '\0';
    S.powered = false;
    S.ndev = 0;
}

static void on_interfaces_removed(DBusMessage * msg) {
    DBusMessageIter it, arr;
    if (!dbus_message_iter_init(msg, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) return;
    const char * path = bt_dbus_iter_str(&it);
    if (!dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char * iface = bt_dbus_iter_str(&arr);
        if (strcmp(iface, DEVICE_IFACE) == 0) {
            remove_dev_path(path);
        } else if (strcmp(iface, ADAPTER_IFACE) == 0 && strcmp(S.adapter, path) == 0) {
            /* bluetoothd does not always remove the devices first. */
            size_t len = strlen(path);
            for (int i = S.ndev - 1; i >= 0; i--)
                if (strncmp(S.devs[i].path, path, len) == 0 && S.devs[i].path[len] == '/') S.devs[i] = S.devs[--S.ndev];
            S.adapter[0] = '\0';
            S.powered = false;
        }
        dbus_message_iter_next(&arr);
    }
}

static void on_properties_changed(DBusMessage * msg) {
    const char * path = dbus_message_get_path(msg);
    DBusMessageIter it;
    if (!path || !dbus_message_iter_init(msg, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return;
    const char * iface = bt_dbus_iter_str(&it);
    if (!dbus_message_iter_next(&it)) return;
    if (strcmp(iface, ADAPTER_IFACE) == 0) {
        if (strcmp(S.adapter, path) == 0) bt_dbus_foreach_prop(&it, adapter_prop, NULL);
    } else if (strcmp(iface, DEVICE_IFACE) == 0) {
        bz5_device_t * d = find_dev_path(path);
        if (d) bt_dbus_foreach_prop(&it, device_prop, d);
    }
}

static DBusHandlerResult filter(DBusConnection * c, DBusMessage * msg, void * data) {
    (void) c;
    (void) data;
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char * name = NULL, * old_owner = NULL, * new_owner = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
            strcmp(name, BZ5_SERVICE) == 0) {
            DBG_LOG("bt_bluez5: org.bluez owner '%s' -> '%s'\n", old_owner, new_owner);
            clear_cache(); /* resynced on the next call */
        }
    } else if (!S.synced) {
        /* Deltas before the snapshot are covered by it. */
    } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
        DBusMessageIter it;
        if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            char path[128];
            snprintf(path, sizeof(path), "%s", bt_dbus_iter_str(&it));
            if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
                add_interfaces(path, &it);
        }
    } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
        on_interfaces_removed(msg);
    } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        on_properties_changed(msg);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- connection and sync -------------------------------------------------------- */

static void drop_conn_locked(void) {
    if (S.conn) {
        dbus_connection_remove_filter(S.conn, filter, NULL);
        dbus_connection_close(S.conn);
        dbus_connection_unref(S.conn);
        S.conn = NULL;
    }
    clear_cache();
}

static bool open_conn_locked(void) {
    if (S.conn && dbus_connection_get_is_connected(S.conn)) return true;
    drop_conn_locked();
    DBusConnection * c = bt_dbus_open();
    if (!c) return false;
    static const char * const rules[] = {
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='org.bluez'",
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_add_match(c, rules[i], &err);
        if (dbus_error_is_set(&err)) {
            DBG_LOG("bt_bluez5: match %s: %s\n", rules[i], err.message);
            dbus_error_free(&err);
        }
    }
    dbus_connection_add_filter(c, filter, NULL, NULL);
    S.conn = c;
    return true;
}

/* Applies every signal already received, without blocking. */
static void pump_locked(void) {
    if (!S.conn) return;
    dbus_connection_read_write(S.conn, 0);
    while (dbus_connection_dispatch(S.conn) == DBUS_DISPATCH_DATA_REMAINS) {}
}

static bool sync_locked(void) {
    DBusMessage * m = dbus_message_new_method_call(BZ5_SERVICE, "/", "org.freedesktop.DBus.ObjectManager",
                                                   "GetManagedObjects");
    DBusMessage * r = bt_dbus_call(S.conn, m, SYNC_TIMEOUT_MS, NULL, 0);
    if (!r) return false;
    clear_cache();
    DBusMessageIter it, objects;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &objects);
        while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&objects, &entry);
            char path[128];
            snprintf(path, sizeof(path), "%s", bt_dbus_iter_str(&entry));
            if (dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY)
                add_interfaces(path, &entry);
            dbus_message_iter_next(&objects);
        }
    }
    dbus_message_unref(r);
    S.synced = true;
    return true;
}

/* Connection up, pending signals applied, cache built. */
static bool ensure_locked(void) {
    if (!open_conn_locked()) return false;
    pump_locked();
    if (!S.synced) {
        /* Signals received before this snapshot are older than it: drop them. */
        if (!sync_locked()) return false;
    }
    pump_locked();
    return S.synced;
}

/* A reference to the connection for a blocking call outside s_mutex. */
static DBusConnection * conn_ref(void) {
    pthread_mutex_lock(&s_mutex);
    DBusConnection * c = open_conn_locked() ? dbus_connection_ref(S.conn) : NULL;
    pthread_mutex_unlock(&s_mutex);
    return c;
}

static void conn_unref(DBusConnection * c) {
    if (c) dbus_connection_unref(c);
}

static void to_stack_device(const bz5_device_t * d, bt_stack_device_t * out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->dev.mac, sizeof(out->dev.mac), "%s", d->mac);
    /* Alias is the name, or the address with dashes when the device sent
     * none (what the UI's "hide unnamed" check recognises). */
    snprintf(out->dev.name, sizeof(out->dev.name), "%s", d->alias[0] ? d->alias : d->name[0] ? d->name : d->mac);
    out->dev.paired = d->paired;
    out->dev.connected = d->connected;
    out->trusted = d->trusted;
    out->blocked = d->blocked;
    out->audio_sink = d->audio_sink;
}

static bool on_adapter(const bz5_device_t * d) {
    if (!S.adapter[0]) return false;
    if (d->adapter[0]) return strcmp(d->adapter, S.adapter) == 0;
    size_t len = strlen(S.adapter);
    return strncmp(d->path, S.adapter, len) == 0 && d->path[len] == '/';
}

/* Object path of a device on the current adapter; false when unknown. */
static bool device_path(const char * mac, char * out, size_t out_size) {
    pthread_mutex_lock(&s_mutex);
    bool ok = false;
    if (ensure_locked() && S.adapter[0]) {
        bz5_device_t * d = find_dev_mac(mac);
        if (d) {
            snprintf(out, out_size, "%s", d->path);
        } else {
            snprintf(out, out_size, "%s/dev_%s", S.adapter, mac);
            for (char * p = out + strlen(S.adapter); *p; p++) {
                if (*p == ':') *p = '_';
                else if (*p >= 'a' && *p <= 'f') *p = (char) (*p - 'a' + 'A');
            }
        }
        ok = true;
    }
    pthread_mutex_unlock(&s_mutex);
    return ok;
}

static bool adapter_path(char * out, size_t out_size) {
    pthread_mutex_lock(&s_mutex);
    bool ok = ensure_locked() && S.adapter[0];
    if (ok) snprintf(out, out_size, "%s", S.adapter);
    pthread_mutex_unlock(&s_mutex);
    return ok;
}

/* Calls a no-argument method; true on success or when the error is one of
 * the names in ok_errors (NULL-terminated). */
static bool simple_call(const char * path, const char * iface, const char * method, int timeout_ms,
                        const char * const * ok_errors) {
    DBusConnection * c = conn_ref();
    if (!c) return false;
    char err[128];
    DBusMessage * r = bt_dbus_call(c, dbus_message_new_method_call(BZ5_SERVICE, path, iface, method), timeout_ms,
                                   err, sizeof(err));
    conn_unref(c);
    if (r) {
        dbus_message_unref(r);
        return true;
    }
    for (int i = 0; ok_errors && ok_errors[i]; i++)
        if (strcmp(err, ok_errors[i]) == 0) return true;
    return false;
}

/* ---- ops ------------------------------------------------------------------------- */

static bt_power_state_t bz5_power_state(int timeout_ms) {
    char path[64];
    pthread_mutex_lock(&s_mutex);
    bool synced = ensure_locked();
    bool has_adapter = synced && S.adapter[0];
    if (has_adapter) snprintf(path, sizeof(path), "%s", S.adapter);
    pthread_mutex_unlock(&s_mutex);
    if (!synced) return BT_POWER_NO_ANSWER;
    if (!has_adapter) return BT_POWER_NO_ADAPTER;

    DBusConnection * c = conn_ref();
    if (!c) return BT_POWER_NO_ANSWER;
    DBusMessage * m = dbus_message_new_method_call(BZ5_SERVICE, path, "org.freedesktop.DBus.Properties", "Get");
    const char * iface = ADAPTER_IFACE, * prop = "Powered";
    if (m) dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    char err[128];
    DBusMessage * r = bt_dbus_call(c, m, timeout_ms > 0 ? timeout_ms : CALL_TIMEOUT_MS, err, sizeof(err));
    conn_unref(c);
    if (!r) {
        /* An error reply is an answer (the adapter went away); silence is not. */
        bool answered = err[0] && strcmp(err, DBUS_ERROR_NO_REPLY) != 0 && strcmp(err, DBUS_ERROR_TIMEOUT) != 0 &&
                        strcmp(err, DBUS_ERROR_SERVICE_UNKNOWN) != 0 && strcmp(err, DBUS_ERROR_NAME_HAS_NO_OWNER) != 0 &&
                        strcmp(err, DBUS_ERROR_DISCONNECTED) != 0;
        return answered ? BT_POWER_NO_ADAPTER : BT_POWER_NO_ANSWER;
    }
    DBusMessageIter it, var;
    bool powered = false;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        powered = bt_dbus_iter_bool(&var);
    }
    dbus_message_unref(r);
    pthread_mutex_lock(&s_mutex);
    if (strcmp(S.adapter, path) == 0) S.powered = powered;
    pthread_mutex_unlock(&s_mutex);
    return powered ? BT_POWER_ON : BT_POWER_OFF;
}

static bool set_adapter_bool(const char * prop, bool on) {
    char path[64];
    if (!adapter_path(path, sizeof(path))) return false;
    DBusConnection * c = conn_ref();
    bool ok = c && bt_dbus_set_bool(c, BZ5_SERVICE, path, ADAPTER_IFACE, prop, on, CALL_TIMEOUT_MS);
    conn_unref(c);
    return ok;
}

static bool bz5_set_powered(bool on) {
    return set_adapter_bool("Powered", on);
}

static bool bz5_set_discoverable(bool on) {
    return set_adapter_bool("Discoverable", on);
}

/* list_devices() fails with -1 for the paired list (callers keep their last
 * state) and 0 for the full list, as the bluetoothctl versions did. */
static int bz5_list_devices(bt_stack_device_t * out, int max_count, bool paired_only) {
    pthread_mutex_lock(&s_mutex);
    int count = -1;
    if (ensure_locked()) {
        count = 0;
        for (int i = 0; i < S.ndev && count < max_count; i++) {
            if (!on_adapter(&S.devs[i]) || !S.devs[i].mac[0]) continue;
            if (paired_only && !S.devs[i].paired) continue;
            to_stack_device(&S.devs[i], &out[count++]);
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return count < 0 && !paired_only ? 0 : count;
}

static bool bz5_device_info(const char * mac, bt_stack_device_t * out, int timeout_ms) {
    (void) timeout_ms; /* the cache answers without a round trip */
    pthread_mutex_lock(&s_mutex);
    bool ok = false;
    if (ensure_locked()) {
        bz5_device_t * d = find_dev_mac(mac);
        if (d && on_adapter(d)) {
            to_stack_device(d, out);
            ok = true;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return ok;
}

static bool bz5_connected_device(bt_device_t * out) {
    pthread_mutex_lock(&s_mutex);
    bool found = false;
    if (ensure_locked()) {
        for (int i = 0; i < S.ndev && !found; i++) {
            if (!on_adapter(&S.devs[i]) || !S.devs[i].connected || !S.devs[i].paired) continue;
            if (out) {
                bt_stack_device_t d;
                to_stack_device(&S.devs[i], &d);
                *out = d.dev;
            }
            found = true;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return found;
}

/* Any paired device with a connection (the old per-device `info` loop). */
static bool bz5_is_connected(void) {
    return bz5_connected_device(NULL);
}

/* Any device with a connection (the old `devices Connected`); -1 when the
 * stack could not be read. */
static int bz5_any_paired_connected(void) {
    pthread_mutex_lock(&s_mutex);
    int result = -1;
    if (ensure_locked()) {
        result = 0;
        for (int i = 0; i < S.ndev; i++)
            if (on_adapter(&S.devs[i]) && S.devs[i].connected) result = 1;
    }
    pthread_mutex_unlock(&s_mutex);
    return result;
}

/* Classic (BR/EDR) inquiry only: dual-mode devices found over BR/EDR
 * reliably offer A2DP/AVRCP (the old `scan.transport bredr`). */
static bool bz5_discover(int seconds) {
    char path[64];
    if (!adapter_path(path, sizeof(path))) return false;
    DBusConnection * c = conn_ref();
    if (!c) return false;
    DBusMessage * m = dbus_message_new_method_call(BZ5_SERVICE, path, ADAPTER_IFACE, "SetDiscoveryFilter");
    if (m) {
        DBusMessageIter it, dict;
        dbus_message_iter_init_append(m, &it);
        if (!dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict) ||
            !bt_dbus_dict_append_string(&dict, "Transport", "bredr") ||
            !dbus_message_iter_close_container(&it, &dict)) {
            dbus_message_unref(m);
            m = NULL;
        }
    }
    DBusMessage * r = bt_dbus_call(c, m, CALL_TIMEOUT_MS, NULL, 0);
    if (r) dbus_message_unref(r);
    static const char * const in_progress[] = { "org.bluez.Error.InProgress", NULL };
    bool started = simple_call(path, ADAPTER_IFACE, "StartDiscovery", CALL_TIMEOUT_MS, in_progress);
    if (started) {
        sleep(seconds > 0 ? (unsigned int) seconds : 1);
        static const char * const not_running[] = { "org.bluez.Error.Failed", NULL };
        (void) simple_call(path, ADAPTER_IFACE, "StopDiscovery", CALL_TIMEOUT_MS, not_running);
    }
    conn_unref(c);
    return started;
}

static bool bz5_pair(const char * mac, int timeout_ms) {
    char path[128];
    if (!device_path(mac, path, sizeof(path))) return false;
    static const char * const already[] = { "org.bluez.Error.AlreadyExists", NULL };
    return simple_call(path, DEVICE_IFACE, "Pair", timeout_ms, already);
}

static bool bz5_set_trusted(const char * mac, bool trusted) {
    char path[128];
    if (!device_path(mac, path, sizeof(path))) return false;
    DBusConnection * c = conn_ref();
    bool ok = c && bt_dbus_set_bool(c, BZ5_SERVICE, path, DEVICE_IFACE, "Trusted", trusted, CALL_TIMEOUT_MS);
    conn_unref(c);
    return ok;
}

static bool bz5_connect(const char * mac, int timeout_ms) {
    char path[128];
    if (!device_path(mac, path, sizeof(path))) return false;
    static const char * const already[] = { "org.bluez.Error.AlreadyConnected", NULL };
    return simple_call(path, DEVICE_IFACE, "Connect", timeout_ms, already);
}

static bool bz5_disconnect(const char * mac, int timeout_ms) {
    char path[128];
    if (!device_path(mac, path, sizeof(path))) return false;
    return simple_call(path, DEVICE_IFACE, "Disconnect", timeout_ms, NULL);
}

/* Unpairs and removes the device (disconnecting it first, inside BlueZ). */
static bool bz5_remove(const char * mac) {
    char adapter[64], path[128];
    if (!adapter_path(adapter, sizeof(adapter)) || !device_path(mac, path, sizeof(path))) return false;
    DBusConnection * c = conn_ref();
    if (!c) return false;
    DBusMessage * m = dbus_message_new_method_call(BZ5_SERVICE, adapter, ADAPTER_IFACE, "RemoveDevice");
    const char * p = path;
    if (m) dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
    DBusMessage * r = bt_dbus_call(c, m, CALL_TIMEOUT_MS, NULL, 0);
    conn_unref(c);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
}

/* AVRCP 1.5+ (absolute volume, with bluealsa) and ProfileManager1 (the
 * remote-control RFCOMM profile). */
const bt_stack_ops_t bt_stack_bluez5 = {
    .name = "bluez5",
    .kind = BT_STACK_BLUEZ5,
    .features = BT_BACKEND_VOLUME_SYNC | BT_BACKEND_RFCOMM_REMOTE,
    .power_state = bz5_power_state,
    .set_powered = bz5_set_powered,
    .set_discoverable = bz5_set_discoverable,
    .list_devices = bz5_list_devices,
    .device_info = bz5_device_info,
    .connected_device = bz5_connected_device,
    .is_connected = bz5_is_connected,
    .any_paired_connected = bz5_any_paired_connected,
    .discover = bz5_discover,
    .pair = bz5_pair,
    .set_trusted = bz5_set_trusted,
    .connect = bz5_connect,
    .disconnect = bz5_disconnect,
    .remove = bz5_remove,
};
