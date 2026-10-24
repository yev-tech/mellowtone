/* libdbus helpers for the Bluetooth back ends (bt_dbus.h). */
#include "bt_dbus.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DBusConnection * bt_dbus_open(void) {
    DBusError err;
    dbus_error_init(&err);
    const char * address = getenv(BT_DBUS_ADDRESS_ENV);
    DBusConnection * c;
    if (address && address[0]) {
        c = dbus_connection_open_private(address, &err);
        if (c && !dbus_bus_register(c, &err)) {
            dbus_connection_close(c);
            dbus_connection_unref(c);
            c = NULL;
        }
    } else {
        c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    }
    if (!c) {
        DBG_LOG("bt_dbus: bus: %s\n", err.message ? err.message : "?");
        dbus_error_free(&err);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    return c;
}

DBusMessage * bt_dbus_call(DBusConnection * c, DBusMessage * m, int timeout_ms, char * error_name,
                           size_t error_name_size) {
    if (error_name && error_name_size) error_name[0] = '\0';
    if (!m) return NULL;
    if (!c) {
        dbus_message_unref(m);
        return NULL;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage * r = dbus_connection_send_with_reply_and_block(c, m, timeout_ms, &err);
    if (!r) {
        DBG_LOG("bt_dbus: %s.%s(%s): %s\n", dbus_message_get_interface(m) ? dbus_message_get_interface(m) : "",
                dbus_message_get_member(m) ? dbus_message_get_member(m) : "",
                dbus_message_get_path(m) ? dbus_message_get_path(m) : "", err.message ? err.message : "?");
        if (error_name && error_name_size && err.name) snprintf(error_name, error_name_size, "%s", err.name);
        dbus_error_free(&err);
    }
    dbus_message_unref(m);
    return r;
}

bool bt_dbus_name_has_owner(DBusConnection * c, const char * name) {
    DBusMessage * m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus", "NameHasOwner");
    if (!m) return false;
    if (!dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
        dbus_message_unref(m);
        return false;
    }
    DBusMessage * r = bt_dbus_call(c, m, 2000, NULL, 0);
    if (!r) return false;
    dbus_bool_t owned = FALSE;
    if (!dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &owned, DBUS_TYPE_INVALID)) owned = FALSE;
    dbus_message_unref(r);
    return owned;
}

bool bt_dbus_set_bool(DBusConnection * c, const char * dest, const char * path, const char * iface,
                      const char * prop, bool value, int timeout_ms) {
    DBusMessage * m = dbus_message_new_method_call(dest, path, "org.freedesktop.DBus.Properties", "Set");
    if (!m) return false;
    DBusMessageIter it, var;
    dbus_bool_t b = value ? TRUE : FALSE;
    dbus_message_iter_init_append(m, &it);
    if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface) ||
        !dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop) ||
        !dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var) ||
        !dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b) ||
        !dbus_message_iter_close_container(&it, &var)) {
        dbus_message_unref(m);
        return false;
    }
    DBusMessage * r = bt_dbus_call(c, m, timeout_ms, NULL, 0);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
}

bool bt_dbus_iter_bool(DBusMessageIter * v) {
    dbus_bool_t b = FALSE;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(v, &b);
    return b;
}

const char * bt_dbus_iter_str(DBusMessageIter * v) {
    const char * s = NULL;
    int t = dbus_message_iter_get_arg_type(v);
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) dbus_message_iter_get_basic(v, &s);
    return s ? s : "";
}

bool bt_dbus_foreach_prop(DBusMessageIter * dict, bt_dbus_prop_cb_t cb, void * ctx) {
    if (dbus_message_iter_get_arg_type(dict) != DBUS_TYPE_ARRAY) return false;
    DBusMessageIter entries;
    dbus_message_iter_recurse(dict, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        const char * key = NULL;
        dbus_message_iter_recurse(&entries, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &var);
                if (key) cb(key, &var, ctx);
            }
        }
        dbus_message_iter_next(&entries);
    }
    return true;
}

static bool dict_append(DBusMessageIter * dict, const char * key, int type, const char * sig, const void * value) {
    DBusMessageIter entry, var;
    return dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry) &&
           dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
           dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &var) &&
           dbus_message_iter_append_basic(&var, type, value) &&
           dbus_message_iter_close_container(&entry, &var) &&
           dbus_message_iter_close_container(dict, &entry);
}

bool bt_dbus_dict_append_string(DBusMessageIter * dict, const char * key, const char * value) {
    const char * v = value ? value : "";
    return dict_append(dict, key, DBUS_TYPE_STRING, "s", &v);
}

bool bt_dbus_dict_append_uint32(DBusMessageIter * dict, const char * key, unsigned int value) {
    dbus_uint32_t v = value;
    return dict_append(dict, key, DBUS_TYPE_UINT32, "u", &v);
}

bool bt_dbus_dict_append_bool(DBusMessageIter * dict, const char * key, bool value) {
    dbus_bool_t v = value ? TRUE : FALSE;
    return dict_append(dict, key, DBUS_TYPE_BOOLEAN, "b", &v);
}
