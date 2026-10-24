/* Stack detection: which BlueZ answers on the bus.
 *
 * - BlueZ 5: org.freedesktop.DBus.ObjectManager.GetManagedObjects on
 *   org.bluez "/" answers with org.bluez.* interfaces (Adapter1, and the
 *   AgentManager1/ProfileManager1 at /org/bluez that BlueZ 5 always has).
 * - BlueZ 4: no ObjectManager; org.bluez.Manager.DefaultAdapter answers
 *   (a path, or org.bluez.Error.NoSuchAdapter while no adapter is up).
 *
 * Nothing owns org.bluez until bluetoothd runs (on the R3 Pro sys_server's
 * first BT:ON starts it), so an unknown result is not kept: the next call
 * probes again (a NameHasOwner round trip to the bus daemon only). A
 * positive answer is kept for the life of the process. */
#include "bt_backend.h"
#include "bt_dbus.h"
#include "debug_log.h"

#include <ctype.h>
#include <stdatomic.h>
#include <string.h>

#define PROBE_TIMEOUT_MS 3000

static atomic_int detected = BT_STACK_UNKNOWN;
static pthread_mutex_t probe_mutex = PTHREAD_MUTEX_INITIALIZER;

bool bt_valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!isxdigit((unsigned char) mac[i])) {
            return false;
        }
    }
    return true;
}

/* Any org.bluez.* interface in a GetManagedObjects reply. */
static bool reply_has_bluez_interface(DBusMessage * r) {
    DBusMessageIter it, objects;
    if (!dbus_message_iter_init(r, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    dbus_message_iter_recurse(&it, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, ifaces;
        dbus_message_iter_recurse(&objects, &entry);
        dbus_message_iter_next(&entry); /* object path -> a{sa{sv}} */
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&entry, &ifaces);
            while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter iface;
                dbus_message_iter_recurse(&ifaces, &iface);
                const char * name = bt_dbus_iter_str(&iface);
                if (strncmp(name, "org.bluez.", 10) == 0) return true;
                dbus_message_iter_next(&ifaces);
            }
        }
        dbus_message_iter_next(&objects);
    }
    return false;
}

/* One connection for the probes, kept (under probe_mutex). */
static DBusConnection * probe_bus;

static bt_stack_kind_t probe(void) {
    if (probe_bus && !dbus_connection_get_is_connected(probe_bus)) {
        dbus_connection_unref(probe_bus);
        probe_bus = NULL;
    }
    if (!probe_bus) probe_bus = bt_dbus_open();
    DBusConnection * c = probe_bus;
    if (!c) return BT_STACK_UNKNOWN;
    bt_stack_kind_t kind = BT_STACK_UNKNOWN;
    if (bt_dbus_name_has_owner(c, "org.bluez")) {
        DBusMessage * m = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                                       "GetManagedObjects");
        DBusMessage * r = bt_dbus_call(c, m, PROBE_TIMEOUT_MS, NULL, 0);
        if (r) {
            if (reply_has_bluez_interface(r)) kind = BT_STACK_BLUEZ5;
            dbus_message_unref(r);
        }
        if (kind == BT_STACK_UNKNOWN) {
            char err[128];
            m = dbus_message_new_method_call("org.bluez", "/", "org.bluez.Manager", "DefaultAdapter");
            r = bt_dbus_call(c, m, PROBE_TIMEOUT_MS, err, sizeof(err));
            if (r) {
                kind = BT_STACK_BLUEZ4;
                dbus_message_unref(r);
            } else if (strncmp(err, "org.bluez.Error.", 16) == 0) {
                kind = BT_STACK_BLUEZ4; /* answered: NoSuchAdapter while no adapter is up */
            }
        }
    }
    if (kind != BT_STACK_UNKNOWN) { /* not needed any more */
        dbus_connection_close(c);
        dbus_connection_unref(c);
        probe_bus = NULL;
    }
    return kind;
}

bt_stack_kind_t bt_stack_detect(void) {
    bt_stack_kind_t kind = (bt_stack_kind_t) atomic_load(&detected);
    if (kind != BT_STACK_UNKNOWN) return kind;
    pthread_mutex_lock(&probe_mutex);
    kind = (bt_stack_kind_t) atomic_load(&detected);
    if (kind == BT_STACK_UNKNOWN) {
        kind = probe();
        if (kind != BT_STACK_UNKNOWN) {
            DBG_LOG("bt_stack: %s\n", kind == BT_STACK_BLUEZ5 ? "BlueZ 5" : "BlueZ 4");
            atomic_store(&detected, kind);
        }
    }
    pthread_mutex_unlock(&probe_mutex);
    return kind;
}

bt_stack_kind_t bt_stack_kind_cached(void) {
    return (bt_stack_kind_t) atomic_load(&detected);
}

void bt_stack_reset_detection(void) {
    pthread_mutex_lock(&probe_mutex);
    if (probe_bus) {
        dbus_connection_close(probe_bus);
        dbus_connection_unref(probe_bus);
        probe_bus = NULL;
    }
    atomic_store(&detected, BT_STACK_UNKNOWN);
    pthread_mutex_unlock(&probe_mutex);
}

/* ---- before detection: nothing known, nothing possible ---------------------- */

static bt_power_state_t none_power_state(int timeout_ms) {
    (void) timeout_ms;
    return BT_POWER_NO_ANSWER;
}
static int none_list(bt_stack_device_t * out, int max_count, bool paired_only) {
    (void) out;
    (void) max_count;
    (void) paired_only;
    return -1;
}
static bool none_info(const char * mac, bt_stack_device_t * out, int timeout_ms) {
    (void) mac;
    (void) out;
    (void) timeout_ms;
    return false;
}
static bool none_connected_device(bt_device_t * out) {
    (void) out;
    return false;
}
static bool none_is_connected(void) {
    return false;
}
static int none_any(void) {
    return -1;
}

static const bt_stack_ops_t stack_unknown = {
    .name = "unknown",
    .kind = BT_STACK_UNKNOWN,
    .power_state = none_power_state,
    .list_devices = none_list,
    .device_info = none_info,
    .connected_device = none_connected_device,
    .is_connected = none_is_connected,
    .any_paired_connected = none_any,
};

const bt_stack_ops_t * bt_stack(void) {
    switch (bt_stack_detect()) {
    case BT_STACK_BLUEZ4: return &bt_stack_bluez4;
    case BT_STACK_BLUEZ5: return &bt_stack_bluez5;
    default: return &stack_unknown;
    }
}
