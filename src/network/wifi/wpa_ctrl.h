#ifndef MT_WPA_CTRL_H
#define MT_WPA_CTRL_H

/* wpa_supplicant control-interface client: the UNIX datagram
 * protocol wpa_cli speaks, without forking wpa_cli. One wpa_ctrl_t is one
 * client socket bound under client_dir and connected to the supplicant's
 * <ctrl_dir>/<iface> socket. Use one for request/reply and a second one,
 * ATTACHed, for unsolicited events ("<3>CTRL-EVENT-CONNECTED ..."), exactly
 * as wpa_cli and the stock sys_server do. Not thread-safe per handle. */

#include <stdbool.h>
#include <stddef.h>

typedef struct wpa_ctrl wpa_ctrl_t;

/* Error returns of wpa_ctrl_request() / wpa_ctrl_recv_event(). */
#define WPA_CTRL_ERR (-1)     /* socket error: the supplicant is gone */
#define WPA_CTRL_TIMEOUT (-2) /* no reply in time */

/* Connects to ctrl_path (e.g. /var/run/wpa_supplicant/wlan0). client_dir
 * holds this client's own socket (NULL: /tmp). NULL when the supplicant's
 * socket is missing or refuses (a stale file left by a killed supplicant):
 * that is what "Wi-Fi is off" looks like. */
wpa_ctrl_t * wpa_ctrl_open(const char * ctrl_path, const char * client_dir);
void wpa_ctrl_close(wpa_ctrl_t * ctrl);

/* Sends cmd and waits up to timeout_ms for its reply, which is copied to
 * reply (NUL-terminated, truncated to reply_size - 1). Unsolicited event
 * messages ("<N>...") and stale replies queued before the send are skipped.
 * Returns the reply length, WPA_CTRL_ERR or WPA_CTRL_TIMEOUT. */
int wpa_ctrl_request(wpa_ctrl_t * ctrl, const char * cmd, char * reply, size_t reply_size, int timeout_ms);

/* ATTACH: this handle then receives event messages. */
bool wpa_ctrl_attach(wpa_ctrl_t * ctrl, int timeout_ms);

/* Waits up to timeout_ms (-1: forever) for one event message. Returns its
 * length (> 0, NUL-terminated in buf), 0 on timeout, WPA_CTRL_ERR on error. */
int wpa_ctrl_recv_event(wpa_ctrl_t * ctrl, char * buf, size_t size, int timeout_ms);

/* The socket descriptor, for poll() next to other descriptors. */
int wpa_ctrl_fd(const wpa_ctrl_t * ctrl);

#endif /* MT_WPA_CTRL_H */
