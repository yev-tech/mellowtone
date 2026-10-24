#ifndef MT_BT_AVRCP_BLUEZ4_H
#define MT_BT_AVRCP_BLUEZ4_H

/* AVRCP target player on BlueZ 4.101 (AVRCP is part of
 * CAP_BT_AUDIO_OUT on every board). bt_media_player.c uses this instead of
 * BlueZ 5's Media1/MPRIS registration when the stack is BlueZ 4.
 *
 * BlueZ 4.101 (doc/media-api.txt, audio/media.c):
 * - org.bluez.Media.RegisterPlayer(object player, dict properties,
 *   dict metadata) on the adapter path (Manager.DefaultAdapter; BlueZ 4
 *   paths are /org/bluez/<pid>/hciN). bluetoothd rejects the whole call
 *   with InvalidArguments for any key it does not know, so only these are
 *   sent: properties Status (string) -- Position is sent afterwards, since
 *   reading it before any metadata exists trips a GLib assertion in
 *   media.c -- and metadata Title, Artist, Album, Genre (strings), Number,
 *   Duration (uint32, ms).
 * - It then listens to our object for the signals
 *   org.bluez.MediaPlayer.TrackChanged(dict metadata) -- all metadata at
 *   once, one signal per track; it resets the position to 0 -- and
 *   PropertyChanged(string, variant): "Status" ("playing", "paused",
 *   "stopped") and "Position" (uint32 ms).
 * - It calls our SetProperty(string, variant) for the player-application
 *   settings (none are offered, so it is refused) and Release() when it
 *   unregisters the player (adapter gone): register again later.
 * - Keys (play/pause/next/previous) do not reach the player on 4.101: AVCTP
 *   turns passthrough commands into a uinput keyboard named after the
 *   remote address, read by bt_media_player.c's key thread.
 * Our bluetoothd advertises AVRCP target 1.3 (patch 0003): metadata, play
 * status and track/status notifications, no absolute volume (a known R3 Pro
 * limitation). */

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>

#define BT_AVRCP4_PLAYER_INTERFACE "org.bluez.MediaPlayer"

typedef struct {
    bool has_track;          /* false: no current track (empty metadata, "stopped") */
    bool playing;
    const char * title;      /* NULL = "" */
    const char * artist;
    const char * album;
    const char * genre;
    unsigned int number;      /* track number, 0 = unknown */
    unsigned int duration_ms; /* 0 = unknown */
    unsigned int position_ms;
} bt_avrcp4_state_t;

/* "playing", "paused" or "stopped". */
const char * bt_avrcp4_status(const bt_avrcp4_state_t * st);

/* Manager.DefaultAdapter. */
bool bt_avrcp4_default_adapter(DBusConnection * c, char * out, size_t out_size);

/* RegisterPlayer, then Position if it is not 0. Blocks up to timeout_ms.
 * err (optional) receives the D-Bus error name on failure. */
bool bt_avrcp4_register(DBusConnection * c, const char * adapter, const char * object_path,
                        const bt_avrcp4_state_t * st, int timeout_ms, char * err, size_t err_size);
bool bt_avrcp4_unregister(DBusConnection * c, const char * adapter, const char * object_path, int timeout_ms);

/* Signals from object_path (queued, never block). TrackChanged is followed
 * by Position when that is not 0 (bluetoothd resets it to 0). */
bool bt_avrcp4_emit_track_changed(DBusConnection * c, const char * object_path, const bt_avrcp4_state_t * st);
bool bt_avrcp4_emit_status(DBusConnection * c, const char * object_path, const bt_avrcp4_state_t * st);
bool bt_avrcp4_emit_position(DBusConnection * c, const char * object_path, unsigned int position_ms);

/* Calls on our object: org.bluez.MediaPlayer SetProperty / Release and
 * Introspect. *released is set when bluetoothd released the player. */
DBusHandlerResult bt_avrcp4_handle_message(DBusConnection * c, DBusMessage * msg, bool * released);

/* Match rules for the signals that mean "register again": an adapter
 * appeared or became the default, or org.bluez got a new owner. */
extern const char * const bt_avrcp4_watch_rules[];
bool bt_avrcp4_needs_reregister(DBusMessage * msg);

#endif /* MT_BT_AVRCP_BLUEZ4_H */
