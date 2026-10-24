#ifndef MT_BT_DBUS_H
#define MT_BT_DBUS_H

/* Small libdbus helpers shared by the Bluetooth back ends: the
 * bus connection, method calls with a timeout, and a{sv} walking.
 *
 * Which bus: the system bus, or MT_BT_DBUS_ADDRESS when that is set. Only
 * the tests set it (tests/bluetooth: a private dbus-daemon with a fake
 * BlueZ); nothing on a device does. */

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>

#define BT_DBUS_ADDRESS_ENV "MT_BT_DBUS_ADDRESS"

/* A private connection (the caller owns it: dbus_connection_close() +
 * dbus_connection_unref()), registered on the bus, exit-on-disconnect off.
 * NULL (and the reason logged) when the bus is not there. */
DBusConnection * bt_dbus_open(void);

/* Sends `m` (consumed) and waits up to timeout_ms. Returns the reply, or
 * NULL with *error_name set to the D-Bus error name ("" on a local
 * failure) when error_name is given. */
DBusMessage * bt_dbus_call(DBusConnection * c, DBusMessage * m, int timeout_ms, char * error_name,
                           size_t error_name_size);

/* org.freedesktop.DBus.NameHasOwner; false on any failure. */
bool bt_dbus_name_has_owner(DBusConnection * c, const char * name);

/* Properties.Set(iface, prop, <bool>) on `dest`/`path`. */
bool bt_dbus_set_bool(DBusConnection * c, const char * dest, const char * path, const char * iface,
                      const char * prop, bool value, int timeout_ms);

/* Variant readers: the value or a default when the type differs. */
bool bt_dbus_iter_bool(DBusMessageIter * v);
const char * bt_dbus_iter_str(DBusMessageIter * v); /* string or object path; "" otherwise */

/* Visits each entry of the a{sv} at *dict (positioned on the array). */
typedef void (*bt_dbus_prop_cb_t)(const char * key, DBusMessageIter * value, void * ctx);
bool bt_dbus_foreach_prop(DBusMessageIter * dict, bt_dbus_prop_cb_t cb, void * ctx);

/* Appends {key: variant} entries to an open a{sv} container. */
bool bt_dbus_dict_append_string(DBusMessageIter * dict, const char * key, const char * value);
bool bt_dbus_dict_append_uint32(DBusMessageIter * dict, const char * key, unsigned int value);
bool bt_dbus_dict_append_bool(DBusMessageIter * dict, const char * key, bool value);

#endif /* MT_BT_DBUS_H */
