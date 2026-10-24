/* AVRCP target player on BlueZ 4.101: see bt_avrcp_bluez4.h. */
#include "bt_avrcp_bluez4.h"
#include "bt_dbus.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>

#define BZ4_SERVICE "org.bluez"

const char * bt_avrcp4_status(const bt_avrcp4_state_t * st) {
    if (st->playing) return "playing";
    return st->has_track ? "paused" : "stopped";
}

bool bt_avrcp4_default_adapter(DBusConnection * c, char * out, size_t out_size) {
    DBusMessage * m = dbus_message_new_method_call(BZ4_SERVICE, "/", "org.bluez.Manager", "DefaultAdapter");
    DBusMessage * r = bt_dbus_call(c, m, 3000, NULL, 0);
    if (!r) return false;
    const char * p = NULL;
    bool ok = dbus_message_get_args(r, NULL, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID) && p;
    if (ok) snprintf(out, out_size, "%s", p);
    dbus_message_unref(r);
    return ok;
}

/* The metadata dict: only keys media.c's metadata_to_val() knows. */
static bool append_metadata(DBusMessageIter * it, const bt_avrcp4_state_t * st) {
    DBusMessageIter dict;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict)) return false;
    bool ok = true;
    if (st->has_track) {
        ok = bt_dbus_dict_append_string(&dict, "Title", st->title ? st->title : "");
        if (ok && st->artist && st->artist[0]) ok = bt_dbus_dict_append_string(&dict, "Artist", st->artist);
        if (ok && st->album && st->album[0]) ok = bt_dbus_dict_append_string(&dict, "Album", st->album);
        if (ok && st->genre && st->genre[0]) ok = bt_dbus_dict_append_string(&dict, "Genre", st->genre);
        if (ok && st->number > 0) ok = bt_dbus_dict_append_uint32(&dict, "Number", st->number);
        if (ok && st->duration_ms > 0) ok = bt_dbus_dict_append_uint32(&dict, "Duration", st->duration_ms);
    }
    return dbus_message_iter_close_container(it, &dict) && ok;
}

bool bt_avrcp4_register(DBusConnection * c, const char * adapter, const char * object_path,
                        const bt_avrcp4_state_t * st, int timeout_ms, char * err, size_t err_size) {
    if (err && err_size) err[0] = '\0';
    DBusMessage * m = dbus_message_new_method_call(BZ4_SERVICE, adapter, "org.bluez.Media", "RegisterPlayer");
    if (!m) return false;
    DBusMessageIter it, props;
    dbus_message_iter_init_append(m, &it);
    bool ok = dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &object_path) &&
              dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &props) &&
              bt_dbus_dict_append_string(&props, "Status", bt_avrcp4_status(st)) &&
              dbus_message_iter_close_container(&it, &props) && append_metadata(&it, st);
    if (!ok) {
        dbus_message_unref(m);
        return false;
    }
    DBusMessage * r = bt_dbus_call(c, m, timeout_ms, err, err_size);
    if (!r) return false;
    dbus_message_unref(r);
    if (st->has_track && st->position_ms > 0) bt_avrcp4_emit_position(c, object_path, st->position_ms);
    return true;
}

bool bt_avrcp4_unregister(DBusConnection * c, const char * adapter, const char * object_path, int timeout_ms) {
    DBusMessage * m = dbus_message_new_method_call(BZ4_SERVICE, adapter, "org.bluez.Media", "UnregisterPlayer");
    if (!m) return false;
    if (!dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &object_path, DBUS_TYPE_INVALID)) {
        dbus_message_unref(m);
        return false;
    }
    DBusMessage * r = bt_dbus_call(c, m, timeout_ms, NULL, 0);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
}

static bool send_signal(DBusConnection * c, DBusMessage * s, bool ok) {
    if (!s) return false;
    ok = ok && dbus_connection_send(c, s, NULL);
    dbus_message_unref(s);
    return ok;
}

bool bt_avrcp4_emit_track_changed(DBusConnection * c, const char * object_path, const bt_avrcp4_state_t * st) {
    DBusMessage * s = dbus_message_new_signal(object_path, BT_AVRCP4_PLAYER_INTERFACE, "TrackChanged");
    if (!s) return false;
    DBusMessageIter it;
    dbus_message_iter_init_append(s, &it);
    bool ok = send_signal(c, s, append_metadata(&it, st));
    if (ok && st->has_track && st->position_ms > 0) ok = bt_avrcp4_emit_position(c, object_path, st->position_ms);
    return ok;
}

static bool emit_property(DBusConnection * c, const char * object_path, const char * name, int type,
                          const char * sig, const void * value) {
    DBusMessage * s = dbus_message_new_signal(object_path, BT_AVRCP4_PLAYER_INTERFACE, "PropertyChanged");
    if (!s) return false;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(s, &it);
    bool ok = dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name) &&
              dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var) &&
              dbus_message_iter_append_basic(&var, type, value) && dbus_message_iter_close_container(&it, &var);
    return send_signal(c, s, ok);
}

bool bt_avrcp4_emit_status(DBusConnection * c, const char * object_path, const bt_avrcp4_state_t * st) {
    const char * status = bt_avrcp4_status(st);
    return emit_property(c, object_path, "Status", DBUS_TYPE_STRING, "s", &status);
}

bool bt_avrcp4_emit_position(DBusConnection * c, const char * object_path, unsigned int position_ms) {
    dbus_uint32_t pos = position_ms;
    return emit_property(c, object_path, "Position", DBUS_TYPE_UINT32, "u", &pos);
}

static void reply(DBusConnection * c, DBusMessage * msg, DBusMessage * r) {
    if (!r) return;
    if (!dbus_message_get_no_reply(msg)) dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
}

DBusHandlerResult bt_avrcp4_handle_message(DBusConnection * c, DBusMessage * msg, bool * released) {
    if (dbus_message_is_method_call(msg, BT_AVRCP4_PLAYER_INTERFACE, "Release")) {
        DBG_LOG("bt_avrcp4: player released by bluetoothd\n");
        if (released) *released = true;
        reply(c, msg, dbus_message_new_method_return(msg));
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, BT_AVRCP4_PLAYER_INTERFACE, "SetProperty")) {
        /* Equalizer/Repeat/Shuffle/Scan: none registered, none settable. */
        reply(c, msg, dbus_message_new_error(msg, "org.bluez.Error.NotSupported", "No player settings"));
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        static const char xml[] =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.bluez.MediaPlayer\">\n"
            "    <method name=\"SetProperty\"><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"v\" direction=\"in\"/></method>\n"
            "    <method name=\"Release\"/>\n"
            "    <signal name=\"PropertyChanged\"><arg type=\"s\"/><arg type=\"v\"/></signal>\n"
            "    <signal name=\"TrackChanged\"><arg type=\"a{sv}\"/></signal>\n"
            "  </interface>\n"
            "</node>\n";
        DBusMessage * r = dbus_message_new_method_return(msg);
        const char * p = xml;
        if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
        reply(c, msg, r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

const char * const bt_avrcp4_watch_rules[] = {
    "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
    "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='DefaultAdapterChanged'",
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
    "arg0='org.bluez'",
    NULL,
};

bool bt_avrcp4_needs_reregister(DBusMessage * msg) {
    if (dbus_message_is_signal(msg, "org.bluez.Manager", "AdapterAdded") ||
        dbus_message_is_signal(msg, "org.bluez.Manager", "DefaultAdapterChanged"))
        return true;
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char * name = NULL, * old_owner = NULL, * new_owner = NULL;
        return dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                     DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
               strcmp(name, BZ4_SERVICE) == 0 && new_owner[0];
    }
    return false;
}
