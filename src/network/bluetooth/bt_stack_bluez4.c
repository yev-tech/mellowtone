/* BlueZ 4.101 stack (the R3 Pro): state over D-Bus from org.bluez
 * (Manager/Adapter/Device/AudioSink), so UI polling never forks.
 *
 * Moved unchanged from the old bluetooth_bluez4.c (the same calls, in the
 * same order, with the same timeouts). Actions (scan, pair, connect,
 * disconnect, unpair) are not here: on the R3 Pro they go through HiBy's
 * sys_server like the stock player's (bt_platform_hiby.c), so these
 * entries stay NULL. */
#include "bt_backend.h"
#include "bt_dbus.h"
#include "debug_log.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define BZ4_SERVICE "org.bluez"
#define BZ4_TIMEOUT_MS 3000

static pthread_mutex_t bus_mutex = PTHREAD_MUTEX_INITIALIZER;
static DBusConnection * bus;

/* ---- D-Bus helpers ------------------------------------------------------ */

static DBusConnection * get_bus(void) {
    if (bus && dbus_connection_get_is_connected(bus)) return bus;
    if (bus) {
        dbus_connection_unref(bus);
        bus = NULL;
    }
    bus = bt_dbus_open();
    return bus;
}

/* Calls path.iface.method with optional (string) argument; caller unrefs. */
static DBusMessage * call(const char * path, const char * iface, const char * method, const char * str_arg) {
    DBusConnection * c = get_bus();
    if (!c) return NULL;
    DBusMessage * m = dbus_message_new_method_call(BZ4_SERVICE, path, iface, method);
    if (!m) return NULL;
    if (str_arg) dbus_message_append_args(m, DBUS_TYPE_STRING, &str_arg, DBUS_TYPE_INVALID);
    return bt_dbus_call(c, m, BZ4_TIMEOUT_MS, NULL, 0);
}

static bool default_adapter(char * out, size_t out_size) {
    DBusMessage * r = call("/", "org.bluez.Manager", "DefaultAdapter", NULL);
    if (!r) return false;
    const char * p = NULL;
    bool ok = dbus_message_get_args(r, NULL, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID) && p;
    if (ok) snprintf(out, out_size, "%s", p);
    dbus_message_unref(r);
    return ok;
}

/* Visits each entry of the a{sv} returned by GetProperties. */
static bool get_properties(const char * path, const char * iface, bt_dbus_prop_cb_t cb, void * ctx) {
    DBusMessage * r = call(path, iface, "GetProperties", NULL);
    if (!r) return false;
    DBusMessageIter it;
    bool ok = dbus_message_iter_init(r, &it) && bt_dbus_foreach_prop(&it, cb, ctx);
    dbus_message_unref(r);
    return ok;
}

/* ---- adapter and device state ------------------------------------------- */

typedef struct {
    bool powered;
    char devices[32][96];
    int device_count;
} adapter_state_t;

static void adapter_prop(const char * key, DBusMessageIter * v, void * ctx) {
    adapter_state_t * a = ctx;
    if (strcmp(key, "Powered") == 0) {
        a->powered = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Devices") == 0 && dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        dbus_message_iter_recurse(v, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH && a->device_count < 32) {
            snprintf(a->devices[a->device_count++], sizeof(a->devices[0]), "%s", bt_dbus_iter_str(&arr));
            dbus_message_iter_next(&arr);
        }
    }
}

static bool read_adapter(adapter_state_t * a) {
    memset(a, 0, sizeof(*a));
    char path[96];
    pthread_mutex_lock(&bus_mutex);
    bool ok = default_adapter(path, sizeof(path)) && get_properties(path, "org.bluez.Adapter", adapter_prop, a);
    pthread_mutex_unlock(&bus_mutex);
    return ok;
}

static void device_prop(const char * key, DBusMessageIter * v, void * ctx) {
    bt_stack_device_t * d = ctx;
    if (strcmp(key, "Address") == 0) {
        snprintf(d->dev.mac, sizeof(d->dev.mac), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Alias") == 0 || (strcmp(key, "Name") == 0 && !d->dev.name[0])) {
        snprintf(d->dev.name, sizeof(d->dev.name), "%s", bt_dbus_iter_str(v));
    } else if (strcmp(key, "Paired") == 0) {
        d->dev.paired = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Trusted") == 0) {
        d->trusted = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "Blocked") == 0) {
        d->blocked = bt_dbus_iter_bool(v);
    } else if (strcmp(key, "UUIDs") == 0 && dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        dbus_message_iter_recurse(v, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            if (strcasecmp(bt_dbus_iter_str(&arr), BT_A2DP_SINK_UUID) == 0) d->audio_sink = true;
            dbus_message_iter_next(&arr);
        }
    }
}

static void sink_prop(const char * key, DBusMessageIter * v, void * ctx) {
    bool * connected = ctx;
    if (strcmp(key, "State") == 0) {
        const char * s = bt_dbus_iter_str(v);
        *connected = strcmp(s, "connected") == 0 || strcmp(s, "playing") == 0;
    }
}

/* "Connected" here means the A2DP sink link is up, not merely the ACL:
 * playback can only go to a device whose AudioSink is connected. */
static bool read_device(const char * path, bt_stack_device_t * d) {
    memset(d, 0, sizeof(*d));
    pthread_mutex_lock(&bus_mutex);
    bool ok = get_properties(path, "org.bluez.Device", device_prop, d);
    if (ok && d->audio_sink) get_properties(path, "org.bluez.AudioSink", sink_prop, &d->dev.connected);
    pthread_mutex_unlock(&bus_mutex);
    if (ok && !d->dev.name[0]) snprintf(d->dev.name, sizeof(d->dev.name), "%s", d->dev.mac);
    return ok && d->dev.mac[0];
}

/* Fills out[] with known devices; paired_only skips unpaired ones. */
static int collect_devices(bt_stack_device_t * out, int max_count, bool paired_only) {
    adapter_state_t a;
    if (!read_adapter(&a)) return -1;
    int count = 0;
    for (int i = 0; i < a.device_count && count < max_count; i++) {
        bt_stack_device_t d;
        if (!read_device(a.devices[i], &d)) continue;
        if (paired_only && !d.dev.paired) continue;
        out[count++] = d;
    }
    return count;
}

static bool connected_device(bt_device_t * out) {
    bt_stack_device_t devs[32];
    int n = collect_devices(devs, 32, false);
    for (int i = 0; i < n; i++) {
        if (devs[i].dev.connected) {
            if (out) *out = devs[i].dev;
            return true;
        }
    }
    return false;
}

/* ---- ops ------------------------------------------------------------------ */

static bt_power_state_t bz4_power_state(int timeout_ms) {
    (void) timeout_ms; /* BZ4_TIMEOUT_MS per call, as before */
    adapter_state_t a;
    if (!read_adapter(&a)) return BT_POWER_NO_ANSWER;
    return a.powered ? BT_POWER_ON : BT_POWER_OFF;
}

static int bz4_list_devices(bt_stack_device_t * out, int max_count, bool paired_only) {
    return collect_devices(out, max_count, paired_only);
}

static bool bz4_device_info(const char * mac, bt_stack_device_t * out, int timeout_ms) {
    (void) timeout_ms;
    bt_stack_device_t devs[32];
    int n = collect_devices(devs, 32, false);
    for (int i = 0; i < n; i++) {
        if (strcasecmp(devs[i].dev.mac, mac) == 0) {
            *out = devs[i];
            return true;
        }
    }
    return false;
}

static bool bz4_is_connected(void) {
    return connected_device(NULL);
}

static int bz4_any_paired_connected(void) {
    bt_stack_device_t devs[32];
    int n = collect_devices(devs, 32, true);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        if (devs[i].dev.connected) return 1;
    return 0;
}

/* AVRCP 1.3 (patch 0003): no absolute volume; no ProfileManager1. */
const bt_stack_ops_t bt_stack_bluez4 = {
    .name = "bluez4",
    .kind = BT_STACK_BLUEZ4,
    .features = 0,
    .power_state = bz4_power_state,
    .list_devices = bz4_list_devices,
    .device_info = bz4_device_info,
    .connected_device = connected_device,
    .is_connected = bz4_is_connected,
    .any_paired_connected = bz4_any_paired_connected,
};
