#ifndef HIBY_SYS_SERVER_H
#define HIBY_SYS_SERVER_H

#include <stdbool.h>

/* Outbound playback status reporting to /var/run/sys_server.
 *
 * Reports playback status, metadata, position, and absolute volume to the
 * system service sys_server, which relays them as D-Bus PropertiesChanged
 * signals for AVRCP queries (e.g. car displays, headphones with screens).
 *
 * Target-only: /var/run/sys_server exists on real hardware. Failures to connect
 * or send are silently ignored. Safe to call from any thread. */

void hiby_sys_server_report_playback_status(bool playing);

/* Sends one raw command (e.g. "BT:ON"). BT commands are queued to
 * sys_server's worker thread, which answers "OK" at once; the outcome goes
 * to /var/run/sys_client, which nobody needs to read. */
void hiby_sys_server_command(const char * cmd);

/* Reports track metadata. length_ms may be 0 if unknown. */
void hiby_sys_server_report_metadata(const char * title, const char * artist,
                                      const char * album, const char * genre, long length_ms);

void hiby_sys_server_report_position(long position_ms);

/* Reports playback volume. percent: 0-100, scaled internally to 0-127.
 * No-ops if no Bluetooth accessory is currently connected. */
void hiby_sys_server_report_volume(int percent);

/* Tests only: send to this path instead of /var/run/sys_server. */
void hiby_sys_server_test_set_path(const char * path);

#endif /* HIBY_SYS_SERVER_H */
