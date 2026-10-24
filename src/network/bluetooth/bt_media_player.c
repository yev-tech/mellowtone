#include "bt_media_player.h"
#include "bt_avrcp_bluez4.h"
#include "bt_backend.h"
#include "bt_dbus.h"
#include "branding.h"
#include "debug_log.h"
#include "hiby_sys_server.h"
#include "input_device_utils.h"
#include "loop_wake.h"
#include "utf8_util.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* AVRCP transport button support.
 * 1. Hosts a D-Bus player object to answer metadata and status queries from
 *    AVRCP controllers: on BlueZ 5 an org.mpris.MediaPlayer2.Player
 *    registered with Media1.RegisterPlayer, on BlueZ 4 an
 *    org.bluez.MediaPlayer registered with Media.RegisterPlayer
 *    (bluetooth/bt_avrcp_bluez4.c). The stack is detected at run time
 *    (bt_stack_detect()); nothing is registered until it is known.
 * 2. Reads AVRCP button events from BlueZ's virtual evdev keyboard device
 *    (BlueZ 5: "<name> (AVRCP)"; BlueZ 4: named after the remote address)
 *    to handle play/pause, next, and previous inputs. */

#define BT_MEDIA_PLAYER_OBJECT_PATH "/org/mellowtone/MediaPlayer"
#define BT_MEDIA_PLAYER_ADAPTER_PATH "/org/bluez/hci0"
/* Dispatch pacing while playing: the position is extrapolated between the
 * GUI's updates and a seek is spotted within this window. While paused the
 * loop sleeps until D-Bus traffic, a state change pushed by the GUI thread
 * (dispatch_wake) or the next retry or metadata-settle deadline. */
#define BT_MEDIA_PLAYER_POLL_TIMEOUT_MS 200
/* Let GUI labels and decoder duration settle before notifying AVRCP. */
#define BT_MEDIA_PLAYER_METADATA_SETTLE_MS 500

static DBusConnection * conn = NULL;
static pthread_t dispatch_thread;
static atomic_bool connect_thread_started = false;
static atomic_bool avrcp_input_thread_started = false;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool playing_state = false;    /* mirrors audio_is_playing(), for PlaybackStatus -- set via bt_media_player_notify_playback_state() */
static bool play_pause_requested = false;
static bool next_requested = false;
static bool prev_requested = false;

/* Self-pipe that ends the dispatch thread's wait when the GUI thread pushes
 * a playback or track change. */
static int dispatch_wake[2] = { -1, -1 };
static pthread_once_t dispatch_wake_once = PTHREAD_ONCE_INIT;

static void dispatch_wake_init(void) {
    int fds[2];
    if (pipe(fds) != 0) return;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    dispatch_wake[0] = fds[0];
    dispatch_wake[1] = fds[1];
}

static void dispatch_wake_signal(void) {
    pthread_once(&dispatch_wake_once, dispatch_wake_init);
    if (dispatch_wake[1] < 0) return;
    char byte = 1;
    (void) !write(dispatch_wake[1], &byte, 1); /* a full pipe already means "wake" */
}

/* Current AVRCP track data, copied by the GUI thread and read by dispatch. */
typedef struct {
    char title[256], artist[256], album[256], genre[256];
    int track_number;
    double position_seconds, duration_seconds;
    uint64_t generation;
} track_snapshot_t;
static track_snapshot_t track_state;

static void call_register_player(bool register_it);

/* The stack this player talks to: read by the dispatch thread (which sets
 * it) and the key thread. */
static _Atomic int stack_kind = BT_STACK_UNKNOWN;

/* BlueZ 4 registration state, dispatch thread only. */
static bool bz4_registered;
static bool bz4_register_due;

static void copy_utf8(char * dst, size_t size, const char * src) {
    if (!src) src = "";
    size_t len = strlen(src);
    utf8_truncate_safe_bounded(dst, size, src, len);
    utf8_sanitize(dst);
}

static track_snapshot_t snapshot_copy(void) {
    pthread_mutex_lock(&state_mutex);
    track_snapshot_t copy = track_state;
    pthread_mutex_unlock(&state_mutex);
    return copy;
}

static bool append_variant_bool(DBusMessageIter * iter, dbus_bool_t value) {
    DBusMessageIter variant;
    return dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &variant) &&
           dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value) &&
           dbus_message_iter_close_container(iter, &variant);
}

static bool append_variant_string(DBusMessageIter * iter, const char * value) {
    DBusMessageIter variant;
    return dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &variant) &&
           dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value) &&
           dbus_message_iter_close_container(iter, &variant);
}

static bool append_variant_int64(DBusMessageIter * iter, int64_t value) {
    DBusMessageIter variant;
    return dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "x", &variant) &&
           dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64, &value) &&
           dbus_message_iter_close_container(iter, &variant);
}

static int64_t seconds_to_microseconds(double seconds) {
    return seconds > 0 ? (int64_t) (seconds * 1000000.0) : 0;
}

static bool append_dict_string(DBusMessageIter * dict, const char * name,
                               const char * value) {
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!append_variant_string(&entry, value)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_dict_bool(DBusMessageIter * dict, const char * name,
                             dbus_bool_t value) {
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!append_variant_bool(&entry, value)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_dict_string_array(DBusMessageIter * dict, const char * name,
                                     const char * value) {
    DBusMessageIter entry, variant, array;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant)) return false;
    if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array)) return false;
    if (!dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &value)) return false;
    if (!dbus_message_iter_close_container(&variant, &array)) return false;
    if (!dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_dict_int32(DBusMessageIter * dict, const char * name, int32_t value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &variant)) return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value)) return false;
    if (!dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_dict_object_path(DBusMessageIter * dict, const char * name,
                                   const char * value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant)) return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &value)) return false;
    if (!dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_dict_int64(DBusMessageIter * dict, const char * name, int64_t value) {
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!append_variant_int64(&entry, value)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_metadata(DBusMessageIter * dict, const track_snapshot_t * s) {
    const char * track_id = "/org/mellowtone/track/current";
    if (!append_dict_string(dict, "xesam:title", s->title) ||
        !append_dict_string_array(dict, "xesam:artist", s->artist) ||
        !append_dict_string(dict, "xesam:album", s->album)) return false;
    if (s->genre[0] && !append_dict_string_array(dict, "xesam:genre", s->genre)) return false;
    if (s->track_number > 0 &&
        !append_dict_int32(dict, "xesam:trackNumber", s->track_number)) return false;
    if (s->duration_seconds > 0 &&
        !append_dict_int64(dict, "mpris:length", seconds_to_microseconds(s->duration_seconds))) return false;
    return append_dict_object_path(dict, "mpris:trackid", track_id);
}

static bool append_metadata_variant(DBusMessageIter * variant, const track_snapshot_t * s) {
    DBusMessageIter metadata;
    if (!dbus_message_iter_open_container(variant, DBUS_TYPE_ARRAY, "{sv}", &metadata)) return false;
    if (!append_metadata(&metadata, s)) return false;
    return dbus_message_iter_close_container(variant, &metadata);
}

static bool append_metadata_entry(DBusMessageIter * dict, const track_snapshot_t * s) {
    DBusMessageIter entry, variant;
    const char * name = "Metadata";
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name)) return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant)) return false;
    if (!append_metadata_variant(&variant, s)) return false;
    if (!dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_position_entry(DBusMessageIter * dict, double seconds) {
    return append_dict_int64(dict, "Position", seconds_to_microseconds(seconds));
}

/* Returns NULL (not an error) if name isn't one of ours -- callers fall
 * through to "no such property" in that case. */
static const char * playback_status_string(void) {
    pthread_mutex_lock(&state_mutex);
    bool p = playing_state;
    pthread_mutex_unlock(&state_mutex);
    return p ? "Playing" : "Paused";
}

/* Appends this property's value as a variant into iter (already
 * positioned correctly by the caller -- either directly for Get, or as a
 * dict-entry value for GetAll). Returns false if `property` isn't
 * recognized for `interface`. */
static bool append_property_value(DBusMessageIter * iter, const char * interface, const char * property) {
    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (strcmp(property, "PlaybackStatus") == 0)
            return append_variant_string(iter, playback_status_string());
        if (strcmp(property, "Metadata") == 0) {
            DBusMessageIter variant;
            track_snapshot_t s = snapshot_copy();
            if (!dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a{sv}", &variant)) return false;
            if (!append_metadata_variant(&variant, &s)) return false;
            return dbus_message_iter_close_container(iter, &variant);
        }
        if (strcmp(property, "Position") == 0) {
            track_snapshot_t s = snapshot_copy();
            return append_variant_int64(iter, seconds_to_microseconds(s.position_seconds));
        }
        if (strcmp(property, "CanControl") == 0) return append_variant_bool(iter, TRUE);
        if (strcmp(property, "CanPlay") == 0) return append_variant_bool(iter, TRUE);
        if (strcmp(property, "CanPause") == 0) return append_variant_bool(iter, TRUE);
        if (strcmp(property, "CanGoNext") == 0) return append_variant_bool(iter, TRUE);
        if (strcmp(property, "CanGoPrevious") == 0) return append_variant_bool(iter, TRUE);
        return false;
    }
    if (strcmp(interface, "org.mpris.MediaPlayer2") == 0) {
        if (strcmp(property, "Identity") == 0) return append_variant_string(iter, MT_MPRIS_IDENTITY);
        if (strcmp(property, "CanQuit") == 0) return append_variant_bool(iter, FALSE);
        if (strcmp(property, "CanRaise") == 0) return append_variant_bool(iter, FALSE);
        if (strcmp(property, "HasTrackList") == 0) return append_variant_bool(iter, FALSE);
        return false;
    }
    return false;
}

/* Every property name this app exposes for `interface`, in the same order
 * append_property_value() recognizes them -- used by GetAll. NULL-terminated. */
static const char * const player_properties[] = {
    "PlaybackStatus", "Metadata", "Position", "CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious", NULL
};
static const char * const root_properties[] = {
    "Identity", "CanQuit", "CanRaise", "HasTrackList", NULL
};

static const char * const * properties_for_interface(const char * interface) {
    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) return player_properties;
    if (strcmp(interface, "org.mpris.MediaPlayer2") == 0) return root_properties;
    return NULL;
}

static void send_empty_reply(DBusConnection * c, DBusMessage * msg) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void send_error_reply(DBusConnection * c, DBusMessage * msg, const char * error_name, const char * error_msg) {
    DBusMessage * reply = dbus_message_new_error(msg, error_name, error_msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static DBusHandlerResult handle_properties_get(DBusConnection * c, DBusMessage * msg) {
    const char * interface = "";
    const char * property = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        send_error_reply(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected (interface, property)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    if (!append_property_value(&iter, interface, property)) {
        dbus_message_unref(reply);
        send_error_reply(c, msg, "org.freedesktop.DBus.Error.UnknownProperty", property);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_properties_get_all(DBusConnection * c, DBusMessage * msg) {
    const char * interface = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        send_error_reply(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected (interface)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    const char * const * names = properties_for_interface(interface);

    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessageIter iter, dict_iter;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    for (int i = 0; names && names[i]; i++) {
        DBusMessageIter entry_iter;
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &names[i]);
        append_property_value(&entry_iter, interface, names[i]);
        dbus_message_iter_close_container(&dict_iter, &entry_iter);
    }
    dbus_message_iter_close_container(&iter, &dict_iter);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* MPRIS's Volume/LoopStatus/Rate are writable in the spec, but this app
 * doesn't use MPRIS for volume (see bt_control_source_volume_sync_start()
 * in bluetooth_control.c) and has no remotely-controllable loop-status/rate
 * concept. Accept and ignore rather than erroring, so a conformant MPRIS
 * client doesn't get an unexpected failure for a property it expects to be
 * settable. */
static DBusHandlerResult handle_properties_set(DBusConnection * c, DBusMessage * msg) {
    send_empty_reply(c, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult message_handler(DBusConnection * c, DBusMessage * msg, void * user_data) {
    (void) user_data;
    if (atomic_load(&stack_kind) == BT_STACK_BLUEZ4) {
        bool released = false;
        DBusHandlerResult r = bt_avrcp4_handle_message(c, msg, &released);
        if (released) bz4_registered = false; /* adapter gone: register again when one is back */
        if (r == DBUS_HANDLER_RESULT_HANDLED) return r;
    }
    const char * interface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!interface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(interface, "org.freedesktop.DBus.Properties") == 0) {
        if (strcmp(member, "Get") == 0) return handle_properties_get(c, msg);
        if (strcmp(member, "GetAll") == 0) return handle_properties_get_all(c, msg);
        if (strcmp(member, "Set") == 0) return handle_properties_set(c, msg);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(interface, "org.freedesktop.DBus.Introspectable") == 0 && strcmp(member, "Introspect") == 0) {
        static const char xml[] =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.mpris.MediaPlayer2\">\n"
            "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
            "  </interface>\n"
            "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
            "    <method name=\"Play\"/>\n"
            "    <method name=\"Pause\"/>\n"
            "    <method name=\"PlayPause\"/>\n"
            "    <method name=\"Stop\"/>\n"
            "    <method name=\"Next\"/>\n"
            "    <method name=\"Previous\"/>\n"
            "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
            "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
            "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
            "  </interface>\n"
            "</node>\n";
        DBusMessage * reply = dbus_message_new_method_return(msg);
        if (reply) {
            const char * p = xml;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
            dbus_connection_send(c, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        /* Play/Pause/PlayPause/Stop all reduce to this app's single
         * audio_toggle_pause() (see gui.c's toggle_play_pause()); there's
         * no separate "force play" vs "force pause" entry point, so
         * Play()/Stop() only request a toggle when that would actually
         * move toward the state they're asking for, using the cached
         * playing_state. */
        pthread_mutex_lock(&state_mutex);
        bool currently_playing = playing_state;
        if (strcmp(member, "PlayPause") == 0) {
            play_pause_requested = true;
        } else if (strcmp(member, "Play") == 0) {
            if (!currently_playing) play_pause_requested = true;
        } else if (strcmp(member, "Pause") == 0 || strcmp(member, "Stop") == 0) {
            if (currently_playing) play_pause_requested = true;
        } else if (strcmp(member, "Next") == 0) {
            next_requested = true;
        } else if (strcmp(member, "Previous") == 0) {
            prev_requested = true;
        } else {
            pthread_mutex_unlock(&state_mutex);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        pthread_mutex_unlock(&state_mutex);
        loop_wake_signal();
        send_empty_reply(c, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult vtable_message_handler(DBusConnection * c, DBusMessage * msg, void * user_data) {
    return message_handler(c, msg, user_data);
}

static DBusObjectPathVTable vtable = {
    NULL, /* unregister_function */
    vtable_message_handler,
    NULL, NULL, NULL, NULL
};

static DBusHandlerResult interfaces_added_filter(DBusConnection * c, DBusMessage * msg, void * data) {
    (void) c;
    (void) data;
    if (atomic_load(&stack_kind) == BT_STACK_BLUEZ4) {
        if (bt_avrcp4_needs_reregister(msg)) {
            bz4_registered = false;
            bz4_register_due = true;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (atomic_load(&stack_kind) != BT_STACK_BLUEZ5 ||
        !dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessageIter iter, interfaces, interface_entry;
    if (!dbus_message_iter_init(msg, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char * object_path = NULL;
    dbus_message_iter_get_basic(&iter, &object_path);
    if (!object_path || strcmp(object_path, BT_MEDIA_PLAYER_ADAPTER_PATH) != 0 ||
        !dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_message_iter_recurse(&iter, &interfaces);
    while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&interfaces, &interface_entry);
        if (dbus_message_iter_get_arg_type(&interface_entry) == DBUS_TYPE_STRING) {
            const char * interface_name = NULL;
            dbus_message_iter_get_basic(&interface_entry, &interface_name);
            if (interface_name && strcmp(interface_name, "org.bluez.Media1") == 0) {
                call_register_player(true);
                break;
            }
        }
        if (!dbus_message_iter_next(&interfaces)) break;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* Fire-and-forget: dbus_message_set_no_reply() tells BlueZ not to send a
 * reply, and dbus_connection_send() queues the call without blocking.
 * Neither RegisterPlayer's nor UnregisterPlayer's reply content is used
 * here beyond logging, and blocking on a reply would stall this thread's
 * own dispatch loop -- including incoming AVRCP button presses -- for up
 * to the reply timeout, which matters since this can fire during a
 * bluetoothd restart. */
static void call_register_player(bool register_it) {
    const char * member = register_it ? "RegisterPlayer" : "UnregisterPlayer";
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", BT_MEDIA_PLAYER_ADAPTER_PATH,
                                                       "org.bluez.Media1", member);
    if (!msg) return;

    const char * path = BT_MEDIA_PLAYER_OBJECT_PATH;
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path)) {
        dbus_message_unref(msg);
        return;
    }
    if (register_it) {
        /* BlueZ's RegisterPlayer handler reads CanPlay/CanPause/
         * CanControl/CanGoNext/CanGoPrevious directly out of this
         * properties dict argument to populate its internal capability
         * flags -- it never queries the registered object's own D-Bus
         * properties afterward. Leaving this dict empty leaves every flag
         * false, so bluetoothd silently no-ops button presses without
         * ever calling this app. This app always supports all five
         * (matching append_property_value()'s hardcoded TRUE for the same
         * properties). */
        DBusMessageIter dict_iter;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter)) {
            dbus_message_unref(msg);
            return;
        }
        static const char * const cap_names[] = {
            "CanPlay", "CanPause", "CanControl", "CanGoNext", "CanGoPrevious"
        };
        for (size_t i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
            if (!append_dict_bool(&dict_iter, cap_names[i], TRUE)) {
                dbus_message_unref(msg);
                return;
            }
        }
        track_snapshot_t s = snapshot_copy();
        if (!append_dict_string(&dict_iter, "PlaybackStatus", playback_status_string())) {
            dbus_message_unref(msg);
            return;
        }
        if (!append_metadata_entry(&dict_iter, &s)) {
            dbus_message_unref(msg);
            return;
        }
        if (!append_position_entry(&dict_iter, s.position_seconds)) {
            dbus_message_unref(msg);
            return;
        }
        if (!dbus_message_iter_close_container(&iter, &dict_iter)) {
            dbus_message_unref(msg);
            return;
        }
    }

    dbus_message_set_no_reply(msg, TRUE);
    if (!dbus_connection_send(conn, msg, NULL)) {
        DBG_LOG("bt_media_player: failed to queue %s\n", member);
    }
    dbus_message_unref(msg);
}

/* Emits PropertiesChanged for PlaybackStatus whenever playback state
 * actually changes. A physical play/pause button on a headset tracks its
 * own idea of "currently playing" and sends the opposite action next time
 * -- without this signal that idea never syncs with reality, so the first
 * button press after a state mismatch is a silent no-op (message_handler()
 * already ignores a request that doesn't match actual state) and only the
 * second press takes effect. */
static void send_playback_status_changed(void) {
    DBusMessage * signal = dbus_message_new_signal(BT_MEDIA_PLAYER_OBJECT_PATH,
                                                     "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!signal) return;

    DBusMessageIter iter, dict_iter, entry_iter, invalid_iter;
    dbus_message_iter_init_append(signal, &iter);
    const char * interface_name = "org.mpris.MediaPlayer2.Player";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface_name);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    const char * prop_name = "PlaybackStatus";
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &prop_name);
    append_variant_string(&entry_iter, playback_status_string());
    dbus_message_iter_close_container(&dict_iter, &entry_iter);
    dbus_message_iter_close_container(&iter, &dict_iter);

    /* Third PropertiesChanged argument: invalidated property names (none) -- still required by the signature. */
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalid_iter);
    dbus_message_iter_close_container(&iter, &invalid_iter);

    dbus_connection_send(conn, signal, NULL);
    dbus_message_unref(signal);
}

static bool send_track_changed(bool metadata_changed, bool position_changed) {
    DBusMessage * signal = dbus_message_new_signal(BT_MEDIA_PLAYER_OBJECT_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!signal) return false;
    DBusMessageIter iter, dict, invalid;
    dbus_message_iter_init_append(signal, &iter);
    const char * interface_name = "org.mpris.MediaPlayer2.Player";
    bool ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface_name);
    if (ok) ok = dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    track_snapshot_t s = snapshot_copy();
    if (ok && metadata_changed) ok = append_metadata_entry(&dict, &s);
    if (ok && position_changed) ok = append_position_entry(&dict, s.position_seconds);
    if (ok) ok = dbus_message_iter_close_container(&iter, &dict);
    if (ok) ok = dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalid);
    if (ok) ok = dbus_message_iter_close_container(&iter, &invalid);
    if (ok) ok = dbus_connection_send(conn, signal, NULL);
    dbus_message_unref(signal);
    return ok;
}

/* The BlueZ 4 view of a snapshot. */
static bt_avrcp4_state_t bz4_state(const track_snapshot_t * s, bool playing) {
    bt_avrcp4_state_t st = {
        .has_track = s->title[0] || s->artist[0] || s->album[0],
        .playing = playing,
        .title = s->title, .artist = s->artist, .album = s->album, .genre = s->genre,
        .number = s->track_number > 0 ? (unsigned int) s->track_number : 0,
        .duration_ms = s->duration_seconds > 0 ? (unsigned int) (s->duration_seconds * 1000.0) : 0,
        .position_ms = s->position_seconds > 0 ? (unsigned int) (s->position_seconds * 1000.0) : 0,
    };
    return st;
}

/* ---- HiBy bt-media feed (R3 Pro) -------------------------------------------
 * The stock bt_init starts HiBy's bt-media before any player. It registers
 * the first BlueZ 4 media player, and BlueZ 4 answers AVRCP from the first
 * one registered, so our own registration above is never asked. bt-media
 * takes the now-playing data on a unix socket, one command per connection,
 * as the stock player sent it: "TrackChanged Start", "TrackChanged Title=…"
 * (Artist, Album, Genre, Duration in ms), "TrackChanged End",
 * "PropertyChanged Status=playing|paused|stopped", "PropertyChanged
 * Position=<ms>". Without it, cars get no title or artist (and so find no
 * album art). */
#define HIBY_MEDIA_SERVER_PATH "/var/run/bt_media_server"
#define HIBY_MEDIA_RETRY_S 5

static bool hiby_media_send(const char * prefix, const char * value) {
    char msg[512];
    int n = snprintf(msg, sizeof(msg), "%s%s", prefix, value ? value : "");
    if (n < 0) return false;
    for (char * p = msg; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", HIBY_MEDIA_SERVER_PATH);
    bool ok = connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0 &&
              send(fd, msg, strlen(msg) + 1, MSG_NOSIGNAL) > 0;
    close(fd);
    return ok;
}

static bool hiby_media_send_track(const track_snapshot_t * s) {
    char field[256], duration[16];
    snprintf(duration, sizeof(duration), "%u",
             s->duration_seconds > 0 ? (unsigned int) (s->duration_seconds * 1000.0) : 0u);
    bool ok = hiby_media_send("TrackChanged Start", NULL);
    copy_utf8(field, sizeof(field), s->title);
    ok = ok && hiby_media_send("TrackChanged Title=", field);
    copy_utf8(field, sizeof(field), s->artist);
    ok = ok && hiby_media_send("TrackChanged Artist=", field);
    copy_utf8(field, sizeof(field), s->album);
    ok = ok && hiby_media_send("TrackChanged Album=", field);
    copy_utf8(field, sizeof(field), s->genre);
    ok = ok && hiby_media_send("TrackChanged Genre=", field);
    ok = ok && hiby_media_send("TrackChanged Duration=", duration);
    return ok && hiby_media_send("TrackChanged End", NULL);
}

static bool hiby_media_send_position(const track_snapshot_t * s) {
    char position[16];
    snprintf(position, sizeof(position), "%u",
             s->position_seconds > 0 ? (unsigned int) (s->position_seconds * 1000.0) : 0u);
    return hiby_media_send("PropertyChanged Position=", position);
}

static bool hiby_media_send_status(bool playing, const track_snapshot_t * s) {
    bool has_track = s->title[0] || s->artist[0] || s->album[0];
    return hiby_media_send("PropertyChanged Status=", playing ? "playing" : has_track ? "paused" : "stopped") &&
           hiby_media_send_position(s);
}

#define BZ4_REGISTER_RETRY_S 10
#define STACK_DETECT_RETRY_S 2
/* Failure-state retries (no stack, no adapter while the radio is off,
 * bt-media down) back off to this, so a long idle with Bluetooth off does not
 * keep waking bluetoothd and the bus. Adapter signals still trigger an
 * immediate BlueZ 4 registration. */
#define RETRY_BACKOFF_MAX_S 120

/* BlueZ 4: RegisterPlayer on the default adapter. Fails while bluetoothd
 * has no adapter (radio off); retried on the adapter signals and every
 * BZ4_REGISTER_RETRY_S seconds. */
static void bz4_register(void) {
    char adapter[96], err[128] = "";
    track_snapshot_t s = snapshot_copy();
    pthread_mutex_lock(&state_mutex);
    bool playing = playing_state;
    pthread_mutex_unlock(&state_mutex);
    bt_avrcp4_state_t st = bz4_state(&s, playing);
    bz4_register_due = false;
    if (!bt_avrcp4_default_adapter(conn, adapter, sizeof(adapter))) return;
    if (bt_avrcp4_register(conn, adapter, BT_MEDIA_PLAYER_OBJECT_PATH, &st, 3000, err, sizeof(err)) ||
        strcmp(err, "org.bluez.Error.AlreadyExists") == 0) {
        bz4_registered = true;
        DBG_LOG("bt_media_player: BlueZ 4 player registered on %s\n", adapter);
    } else {
        DBG_LOG("bt_media_player: BlueZ 4 RegisterPlayer on %s failed: %s\n", adapter, err);
    }
}

/* First sight of the stack: watch what it needs and register. */
static void stack_known(bt_stack_kind_t kind) {
    atomic_store(&stack_kind, kind);
    if (kind == BT_STACK_BLUEZ4) {
        for (int i = 0; bt_avrcp4_watch_rules[i]; i++) dbus_bus_add_match(conn, bt_avrcp4_watch_rules[i], NULL);
        bz4_register();
    } else {
        /* A later adapter InterfacesAdded signal re-registers the player
         * after BlueZ recreates the adapter. */
        call_register_player(true);
    }
}

static double seconds_between(const struct timespec * a, const struct timespec * b) {
    return (double) (b->tv_sec - a->tv_sec) + (double) (b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static double next_backoff(double current, double base) {
    if (current < base) return base;
    double next = current * 2;
    return next > RETRY_BACKOFF_MAX_S ? RETRY_BACKOFF_MAX_S : next;
}

/* Milliseconds until `interval_s` after `since`, for a poll() timeout. */
static int ms_until(const struct timespec * since, double interval_s, const struct timespec * now) {
    double left = interval_s - seconds_between(since, now);
    if (left <= 0) return 0;
    return (int) (left * 1000.0) + 1;
}

static int min_timeout(int a, int b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

/* Waits up to timeout_ms (-1: no limit) for bus traffic or a dispatch_wake,
 * then reads and dispatches everything that arrived. */
static void dispatch_wait(int timeout_ms) {
    if (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS) timeout_ms = 0;
    dbus_connection_flush(conn);
    int bus_fd = -1;
    pthread_once(&dispatch_wake_once, dispatch_wake_init);
    if (!dbus_connection_get_unix_fd(conn, &bus_fd) || bus_fd < 0 || dispatch_wake[0] < 0) {
        dbus_connection_read_write_dispatch(conn, timeout_ms < 0 ? BT_MEDIA_PLAYER_POLL_TIMEOUT_MS : timeout_ms);
        return;
    }
    struct pollfd pfd[2] = {
        { .fd = bus_fd, .events = POLLIN, .revents = 0 },
        { .fd = dispatch_wake[0], .events = POLLIN, .revents = 0 },
    };
    if (poll(pfd, 2, timeout_ms) > 0 && (pfd[1].revents & POLLIN)) {
        char buf[64];
        while (read(dispatch_wake[0], buf, sizeof(buf)) > 0) {
        }
    }
    dbus_connection_read_write(conn, 0);
    while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
}

static void * connect_thread_func(void * arg);

static void * dispatch_thread_func(void * arg) {
    (void) arg;

    /* Forces the first loop iteration to send an initial PropertiesChanged
     * once registered, regardless of playing_state's default -- a fresh
     * connection shouldn't start from BlueZ's own default assumption if
     * this app already knows the real state. */
    bool last_notified_playing = !playing_state;

    bt_stack_kind_t kind = bt_stack_detect();
    if (kind != BT_STACK_UNKNOWN) stack_known(kind);
    struct timespec last_attempt;
    clock_gettime(CLOCK_MONOTONIC, &last_attempt);
    uint64_t last_seen_generation = snapshot_copy().generation;
    uint64_t last_emitted_generation = last_seen_generation;
    double last_position = snapshot_copy().position_seconds;
    DBG_LOG("bt_media_player: dispatch started (stack %d)\n", (int) kind);

    struct timespec previous_iteration;
    clock_gettime(CLOCK_MONOTONIC, &previous_iteration);
    struct timespec metadata_change_time = previous_iteration;
    /* bt-media feed: the track and status still to deliver, and when. */
    bool hiby_dirty = true;
    struct timespec hiby_last_try = previous_iteration;
    double stack_retry_s = STACK_DETECT_RETRY_S;
    double bz4_retry_s = BZ4_REGISTER_RETRY_S;
    double hiby_retry_s = HIBY_MEDIA_RETRY_S;
    for (;;) {
        struct timespec iteration_time;
        clock_gettime(CLOCK_MONOTONIC, &iteration_time);
        double elapsed_seconds = seconds_between(&previous_iteration, &iteration_time);
        previous_iteration = iteration_time;
        kind = (bt_stack_kind_t) atomic_load(&stack_kind);
        if (kind == BT_STACK_UNKNOWN && seconds_between(&last_attempt, &iteration_time) >= stack_retry_s) {
            last_attempt = iteration_time;
            kind = bt_stack_detect();
            if (kind != BT_STACK_UNKNOWN) stack_known(kind);
            else stack_retry_s = next_backoff(stack_retry_s, STACK_DETECT_RETRY_S);
        } else if (kind == BT_STACK_BLUEZ4 && !bz4_registered &&
                   (bz4_register_due || seconds_between(&last_attempt, &iteration_time) >= bz4_retry_s)) {
            bool signalled = bz4_register_due;
            last_attempt = iteration_time;
            bz4_register();
            if (bz4_registered || signalled) bz4_retry_s = BZ4_REGISTER_RETRY_S;
            else bz4_retry_s = next_backoff(bz4_retry_s, BZ4_REGISTER_RETRY_S);
        }
        bool bz4 = kind == BT_STACK_BLUEZ4 && bz4_registered;
        pthread_mutex_lock(&state_mutex);
        bool current_playing = playing_state;
        pthread_mutex_unlock(&state_mutex);

        if (current_playing != last_notified_playing) {
            if (kind == BT_STACK_BLUEZ5) {
                send_playback_status_changed();
                send_track_changed(false, true);
            } else if (bz4) {
                track_snapshot_t s = snapshot_copy();
                bt_avrcp4_state_t st = bz4_state(&s, current_playing);
                bt_avrcp4_emit_status(conn, BT_MEDIA_PLAYER_OBJECT_PATH, &st);
                if (st.has_track) bt_avrcp4_emit_position(conn, BT_MEDIA_PLAYER_OBJECT_PATH, st.position_ms);
            }
            hiby_sys_server_report_playback_status(current_playing);
            if (kind == BT_STACK_BLUEZ4 && !hiby_dirty) {
                track_snapshot_t s = snapshot_copy();
                if (!hiby_media_send_status(current_playing, &s)) hiby_dirty = true;
            }
            last_notified_playing = current_playing;
        }
        track_snapshot_t current = snapshot_copy();
        if (current.generation != last_seen_generation) {
            last_seen_generation = current.generation;
            metadata_change_time = iteration_time;
        }
        double metadata_stable_seconds = seconds_between(&metadata_change_time, &iteration_time);
        bool metadata_pending = current.generation != last_emitted_generation;
        bool metadata_settled = metadata_stable_seconds >= (BT_MEDIA_PLAYER_METADATA_SETTLE_MS / 1000.0);
        bool emit_metadata = metadata_pending && metadata_settled;
        double elapsed_position = last_position + (current_playing ? elapsed_seconds : 0.0);
        bool seek_detected = (current.position_seconds - elapsed_position > 2.0) ||
                             (elapsed_position - current.position_seconds > 2.0);
        bool position_changed = emit_metadata || (!metadata_pending && seek_detected);
        if (emit_metadata || (!metadata_pending && seek_detected)) {
            if (kind == BT_STACK_BLUEZ5) {
                send_track_changed(emit_metadata, position_changed);
            } else if (bz4) {
                bt_avrcp4_state_t st = bz4_state(&current, current_playing);
                if (emit_metadata) bt_avrcp4_emit_track_changed(conn, BT_MEDIA_PLAYER_OBJECT_PATH, &st);
                else if (st.has_track) bt_avrcp4_emit_position(conn, BT_MEDIA_PLAYER_OBJECT_PATH, st.position_ms);
            }
            if (kind == BT_STACK_BLUEZ4 && !hiby_dirty) {
                bool ok = emit_metadata ? hiby_media_send_track(&current) && hiby_media_send_status(current_playing, &current)
                                        : hiby_media_send_position(&current);
                if (!ok) hiby_dirty = true;
            }
            if (emit_metadata) last_emitted_generation = current.generation;
        }
        /* A full resend after a failure: bt-media not up yet, or restarted. */
        if (kind == BT_STACK_BLUEZ4 && hiby_dirty && !metadata_pending &&
            seconds_between(&hiby_last_try, &iteration_time) >= hiby_retry_s) {
            hiby_last_try = iteration_time;
            hiby_dirty = !(hiby_media_send_track(&current) && hiby_media_send_status(current_playing, &current));
            hiby_retry_s = hiby_dirty ? next_backoff(hiby_retry_s, HIBY_MEDIA_RETRY_S) : HIBY_MEDIA_RETRY_S;
        }
        last_position = current.position_seconds;

        /* Only playback needs the fixed cadence; otherwise sleep until the
         * next thing that is actually due. */
        int timeout_ms = current_playing ? BT_MEDIA_PLAYER_POLL_TIMEOUT_MS : -1;
        if (metadata_pending && !emit_metadata)
            timeout_ms = min_timeout(timeout_ms, ms_until(&metadata_change_time,
                                                          BT_MEDIA_PLAYER_METADATA_SETTLE_MS / 1000.0, &iteration_time));
        if (kind == BT_STACK_UNKNOWN)
            timeout_ms = min_timeout(timeout_ms, ms_until(&last_attempt, stack_retry_s, &iteration_time));
        else if (kind == BT_STACK_BLUEZ4 && !bz4_registered)
            timeout_ms = min_timeout(timeout_ms, bz4_register_due ? 0 : ms_until(&last_attempt, bz4_retry_s, &iteration_time));
        if (kind == BT_STACK_BLUEZ4 && hiby_dirty)
            timeout_ms = min_timeout(timeout_ms, ms_until(&hiby_last_try, hiby_retry_s, &iteration_time));

        /* Processes any incoming method calls (dispatched to
         * message_handler() above) and blocks for up to this long if
         * there's nothing to do -- also what paces this loop's own
         * playing_state check above, so a play/pause is reflected within
         * one timeout window, not instantly, matching every other
         * ~poll-interval Bluetooth state sync in this codebase (e.g. the
         * GUI's own ~5s connection poll is far coarser than this). */
        dispatch_wait(timeout_ms);

        /* The bus went away (on the R3 Pro, bt_suspend stops dbus-daemon
         * before suspend-to-RAM and bt_resume starts a new one): drop the
         * connection and connect again, registering from scratch. */
        if (!dbus_connection_get_is_connected(conn)) {
            DBG_LOG("bt_media_player: system bus disconnected, reconnecting\n");
            dbus_connection_remove_filter(conn, interfaces_added_filter, NULL);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            conn = NULL;
            atomic_store(&stack_kind, BT_STACK_UNKNOWN);
            bz4_registered = false;
            bz4_register_due = false;
            return connect_thread_func(NULL);
        }
    }

    return NULL; /* unreached -- this thread runs for the app's whole lifetime, same as audio.c's playback thread */
}

/* Bounded retry for dbus_bus_get() to tolerate slow or in-progress D-Bus startup. */
#define DBUS_BUS_GET_RETRY_ATTEMPTS 10
#define DBUS_BUS_GET_RETRY_DELAY_US 300000

/* One connection attempt: get the bus, register our object path, start the
 * dispatch thread. Shared by the fast bounded retry in bt_media_player_
 * init() below and the slower background retry further down. */
static void drop_connection(void) {
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    conn = NULL;
}

static bool attempt_connect_once(void) {
    conn = bt_dbus_open(); /* the system bus (MT_BT_DBUS_ADDRESS in tests) */
    if (!conn) return false;

    if (!dbus_connection_register_object_path(conn, BT_MEDIA_PLAYER_OBJECT_PATH, &vtable, NULL)) {
        DBG_LOG("bt_media_player: failed to register object path\n");
        drop_connection();
        return false;
    }
    DBusError match_err;
    dbus_error_init(&match_err);
    dbus_bus_add_match(conn, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", &match_err);
    if (dbus_error_is_set(&match_err)) {
        DBG_LOG("bt_media_player: failed to watch BlueZ interfaces: %s\n", match_err.message);
        dbus_error_free(&match_err);
    }
    dbus_connection_add_filter(conn, interfaces_added_filter, NULL, NULL);
    DBG_LOG("bt_media_player: connected to system bus and registered %s\n", BT_MEDIA_PLAYER_OBJECT_PATH);

    if (pthread_create(&dispatch_thread, NULL, dispatch_thread_func, NULL) != 0) {
        DBG_LOG("bt_media_player: could not start the dispatch thread\n");
        dbus_connection_remove_filter(conn, interfaces_added_filter, NULL);
        dbus_connection_unregister_object_path(conn, BT_MEDIA_PLAYER_OBJECT_PATH);
        drop_connection();
        return false;
    }
    pthread_detach(dispatch_thread);
    return true;
}

/* Background retry delay when initial connection attempts fail. */
#define BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US (5 * 1000000)

/* Connects to D-Bus and registers the media player, retrying in the background
 * if the bus is not immediately available. */
static void * connect_thread_func(void * arg) {
    (void) arg;
    for (int attempt = 0; attempt < DBUS_BUS_GET_RETRY_ATTEMPTS; attempt++) {
        if (attempt_connect_once()) return NULL;
        if (attempt + 1 < DBUS_BUS_GET_RETRY_ATTEMPTS) usleep(DBUS_BUS_GET_RETRY_DELAY_US);
    }

    DBG_LOG("bt_media_player: exhausted fast retry, falling back to background retry\n");
    for (;;) {
        usleep(BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US);
        if (attempt_connect_once()) return NULL;
    }
}

/* See avrcp_input_thread_func()'s own comment (top of file) for why this
 * exists and why it's a separate device from hw_buttons.c's own two. */
#define BT_AVRCP_INPUT_NAME_MARKER "(AVRCP)"
/* Without a device the thread waits on inotify for a new /dev/input node
 * (BlueZ creates the uinput keyboard when a remote connects) and only falls
 * back to rescanning on this cadence if inotify is unavailable. A node can
 * show up a moment before it answers EVIOCGNAME, hence the settle rescans. */
#define BT_AVRCP_INPUT_RESCAN_DELAY_MS 3000
#define BT_AVRCP_INPUT_SETTLE_MS 500
#define BT_AVRCP_INPUT_SETTLE_TRIES 4
/* Safety net with the watch: the device's name only matches once the stack
 * is known, which can be after its node appeared. */
#define BT_AVRCP_INPUT_IDLE_RESCAN_MS 60000

/* Waits for /dev/input to change (or the fallback delay). */
static void avrcp_wait_for_input_node(int watch_fd, int timeout_ms) {
    if (watch_fd < 0) {
        usleep((useconds_t) timeout_ms * 1000);
        return;
    }
    struct pollfd pfd = { .fd = watch_fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, timeout_ms) > 0) {
        char buf[1024];
        while (read(watch_fd, buf, sizeof(buf)) > 0) {
        }
    }
}

static void * avrcp_input_thread_func(void * arg) {
    (void) arg;
    int fd = -1;
    int watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch_fd >= 0 && inotify_add_watch(watch_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(watch_fd);
        watch_fd = -1;
    }
    int settle_tries = 0;

    for (;;) {
        if (fd < 0) {
            char path[64];
            /* BlueZ 4.101's AVCTP names its uinput keyboard after the
             * remote address alone (audio/avctp.c init_uinput()). */
            bool found = atomic_load(&stack_kind) == BT_STACK_BLUEZ4
                             ? find_input_device_named_bdaddr(path, sizeof(path))
                             : find_input_device_containing(BT_AVRCP_INPUT_NAME_MARKER, path, sizeof(path));
            if (found) {
                fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    DBG_LOG("bt_media_player: AVRCP input device found at %s\n", path);
                } else {
                    DBG_LOG("bt_media_player: failed to open AVRCP input device %s\n", path);
                }
            }
            if (fd < 0) {
                if (settle_tries > 0) {
                    settle_tries--;
                    avrcp_wait_for_input_node(watch_fd, BT_AVRCP_INPUT_SETTLE_MS);
                } else {
                    avrcp_wait_for_input_node(watch_fd, watch_fd < 0 ? BT_AVRCP_INPUT_RESCAN_DELAY_MS
                                                                       : BT_AVRCP_INPUT_IDLE_RESCAN_MS);
                    settle_tries = BT_AVRCP_INPUT_SETTLE_TRIES;
                }
                continue;
            }
            settle_tries = 0;
        }

        /* No timeout: a removed device reports POLLHUP/POLLERR, then the
         * read below fails and the slot is closed. */
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (ret == 0) continue; /* timeout, nothing to read -- just re-poll */

        struct input_event ev;
        ssize_t n;
        while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t) sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) { /* press only -- matches hw_buttons.c's own next/prev/play-pause handling, no release-triggered action */
                pthread_mutex_lock(&state_mutex);
                /* PLAY and PAUSE are commands, not toggles, as the D-Bus
                 * Play()/Pause() above: a car stereo sends PLAY several
                 * times on connect, which toggled playback on and off. */
                switch (ev.code) {
                    case KEY_PLAYCD:       if (!playing_state) play_pause_requested = true; break;
                    case KEY_PAUSECD:
                    case KEY_STOPCD:       if (playing_state) play_pause_requested = true; break;
                    case KEY_NEXTSONG:     next_requested = true; break;
                    case KEY_PREVIOUSSONG: prev_requested = true; break;
                    default: break;
                }
                pthread_mutex_unlock(&state_mutex);
                loop_wake_signal();
            }
        }
        /* n == 0 (EOF) or a real error (not EAGAIN, which just means the
         * queue is empty for now) means the accessory disconnected and
         * BlueZ's input plugin destroyed this device -- close and go back
         * to rescanning rather than spinning on a dead fd. */
        if (n == 0 || (n < 0 && errno != EAGAIN)) {
            DBG_LOG("bt_media_player: AVRCP input device gone\n");
            close(fd);
            fd = -1;
        }
    }

    return NULL; /* unreached -- this thread runs for the app's whole lifetime, same as the D-Bus dispatch thread above */
}

void bt_media_player_init(void) {
    if (!atomic_exchange_explicit(&connect_thread_started, true, memory_order_acq_rel)) {
        pthread_t connect_thread;
        if (pthread_create(&connect_thread, NULL, connect_thread_func, NULL) == 0) {
            pthread_detach(connect_thread);
        } else {
            atomic_store_explicit(&connect_thread_started, false, memory_order_release);
        }
    }

    if (!atomic_exchange_explicit(&avrcp_input_thread_started, true, memory_order_acq_rel)) {
        pthread_t avrcp_input_thread;
        if (pthread_create(&avrcp_input_thread, NULL, avrcp_input_thread_func, NULL) == 0) {
            pthread_detach(avrcp_input_thread);
        } else {
            atomic_store_explicit(&avrcp_input_thread_started, false, memory_order_release);
        }
    }
}

void bt_media_player_notify_playback_state(bool playing) {
    pthread_mutex_lock(&state_mutex);
    bool changed = playing_state != playing;
    playing_state = playing;
    pthread_mutex_unlock(&state_mutex);
    if (changed) dispatch_wake_signal();
}

void bt_media_player_notify_track(const char * title, const char * artist, const char * album,
                                  const char * genre, int track_number, double position_seconds,
                                  double duration_seconds) {
    track_snapshot_t next = {0};
    copy_utf8(next.title, sizeof(next.title), title);
    copy_utf8(next.artist, sizeof(next.artist), artist);
    copy_utf8(next.album, sizeof(next.album), album);
    copy_utf8(next.genre, sizeof(next.genre), genre);
    next.track_number = track_number > 0 ? track_number : 0;
    next.position_seconds = position_seconds > 0 ? position_seconds : 0;
    next.duration_seconds = duration_seconds > 0 ? duration_seconds : 0;
    pthread_mutex_lock(&state_mutex);
    bool changed = strcmp(track_state.title, next.title) || strcmp(track_state.artist, next.artist) ||
                   strcmp(track_state.album, next.album) || strcmp(track_state.genre, next.genre) ||
                   track_state.track_number != next.track_number || track_state.duration_seconds != next.duration_seconds;
    next.generation = track_state.generation + (changed ? 1 : 0);
    /* Playback advances the position by the GUI tick (500 ms); a bigger
     * jump is a seek the paused dispatch loop must report. */
    double jump = next.position_seconds - track_state.position_seconds;
    bool seeked = jump > 1.0 || jump < -1.0;
    track_state = next;
    pthread_mutex_unlock(&state_mutex);
    if (changed || seeked) dispatch_wake_signal();
}

bool bt_media_player_consume_play_pause(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = play_pause_requested;
    play_pause_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool bt_media_player_consume_next(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = next_requested;
    next_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool bt_media_player_consume_prev(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = prev_requested;
    prev_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
