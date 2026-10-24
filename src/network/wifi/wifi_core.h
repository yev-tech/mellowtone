#ifndef MT_WIFI_CORE_H
#define MT_WIFI_CORE_H

/* Shared Wi-Fi core: one worker thread keeps an ATTACHed
 * wpa_supplicant control socket and a cached view of the link, so every
 * read (status bar, screens, plugins) is a mutex-protected copy instead of
 * a wpa_cli fork. Back ends (wifi_backend.h) only add power and policy.
 *
 * "Enabled" means the supplicant answers on its control socket (PING): a
 * stale socket file after wifi_off.sh is refused at connect() and reads as
 * off. */

#include "wifi_control.h" /* wifi_network_t, wifi_saved_network_t, wifi_info_t, results */

#include <stdbool.h>
#include <stddef.h>

/* Events for the UI (wifi_core_take_events()). */
enum {
    WIFI_EVT_ENABLED_CHANGED = 1u << 0,
    WIFI_EVT_CONNECTED = 1u << 1,
    WIFI_EVT_DISCONNECTED = 1u << 2,
    WIFI_EVT_SCAN_DONE = 1u << 3,
    WIFI_EVT_WRONG_KEY = 1u << 4,
    WIFI_EVT_STATE = 1u << 5, /* any cached field changed (signal, IP, ...) */
};

typedef struct {
    bool enabled;
    bool connected; /* wpa_state=COMPLETED */
    int network_id; /* -1 when not connected */
    char ssid[WIFI_MAX_SSID_LEN];
    char wpa_state[24];
    int rssi; /* dBm, 0 = unknown */
    int signal_level; /* 0..3, wifi_rssi_to_level() */
    char ip[16];
    char gateway[16];
    char mac[18];
} wifi_state_t;

typedef struct {
    const char * ctrl_dir;   /* NULL: /var/run/wpa_supplicant */
    const char * iface;      /* NULL: wlan0 */
    const char * client_dir; /* our client sockets; NULL: /tmp */
    const char * route_file; /* NULL: /proc/net/route */
    /* Signal refresh runs only while this returns true (screen on); NULL = always. */
    bool (*screen_on)(void);
    /* Called on a helper thread after CTRL-EVENT-CONNECTED with the state
     * read right after the event, one call at a time (a CONNECTED during a
     * call queues one more); DHCP for the generic back end. NULL: none. */
    void (*on_connected)(const char * iface, const char * ssid, const char * ip);
    int signal_interval_ms; /* 0: 10 s */
    int probe_interval_ms;  /* while off, how often the socket is looked for; 0: 1 s */
    int request_timeout_ms; /* 0: 2 s */
} wifi_core_config_t;

bool wifi_core_start(const wifi_core_config_t * config);
/* Stops the worker and closes the sockets (tests, UI reload). */
void wifi_core_stop(void);

/* Cached, non-blocking: safe on the UI thread. */
void wifi_core_get_state(wifi_state_t * out);
bool wifi_core_is_enabled(void);
/* Bumped on every change of the cached state. */
unsigned wifi_core_generation(void);
/* WIFI_EVT_* bits since the last call (one consumer: the Wi-Fi UI). */
unsigned wifi_core_take_events(void);

/* Wakes the worker to look at the socket now and waits up to timeout_ms for
 * the enabled state to equal `want` (after a power change). */
bool wifi_core_wait_enabled(bool want, int timeout_ms);

/* Request/reply on the shared request socket (any worker thread, never the
 * UI thread). Returns the reply length or a WPA_CTRL_* error. */
int wifi_core_request(const char * cmd, char * reply, size_t reply_size);
/* true when the reply starts with "OK". */
bool wifi_core_request_ok(const char * cmd);
/* Formats a command, then wifi_core_request_ok(). */
bool wifi_core_cmd_ok(const char * fmt, ...) __attribute__((format(printf, 1, 2)));

/* Re-reads STATUS (and SIGNAL_POLL when connected) into the cache now. */
void wifi_core_refresh(void);

/* Scan: SCAN, then wait for CTRL-EVENT-SCAN-RESULTS. */
bool wifi_core_scan_start(void);
bool wifi_core_scan_wait(int timeout_ms);
int wifi_core_scan_results(wifi_network_t * out, int max_count);
int wifi_core_list_saved(wifi_saved_network_t * out, int max_count);
bool wifi_core_get_info(wifi_info_t * out);

/* Connection attempts: remember the event sequence before acting, then wait
 * for the outcome for network `id` (CONNECTED, WRONG_KEY, ...). */
unsigned wifi_core_event_seq(void);
wifi_connect_result_t wifi_core_wait_connect(int id, unsigned since_seq, int timeout_ms);

/* ---- helpers (pure, unit-tested) ---- */
int wifi_rssi_to_level(int rssi_dbm);
/* Decodes wpa_supplicant's printf-escaped SSID text (\\, \", \n, \xNN ...). */
void wifi_ssid_decode(const char * in, char * out, size_t out_size);
/* Value of `key=` in a STATUS-style "k=v\n" reply; false when missing. */
bool wifi_kv_get(const char * text, const char * key, char * out, size_t out_size);
/* SCAN_RESULTS text -> deduplicated networks (strongest BSSID kept,
 * hidden entries skipped); current_ssid marks is_current. Modifies text. */
int wifi_parse_scan_results(char * text, const char * current_ssid, wifi_network_t * out, int max_count);
/* Security flags of the strongest BSSID advertising ssid in a SCAN_RESULTS
 * text (not modified). false when the SSID is not in the results. */
bool wifi_scan_flags_for(const char * text, const char * ssid, char * flags, size_t flags_size);
/* LIST_NETWORKS text -> saved networks; disabled[i] set for "[DISABLED]"
 * when disabled is not NULL. Modifies text. */
int wifi_parse_list_networks(char * text, wifi_saved_network_t * out, bool * disabled, int max_count);
/* Default IPv4 gateway of iface from a /proc/net/route style file. */
bool wifi_route_gateway(const char * route_file, const char * iface, char * out, size_t out_size);
/* nameserver lines of a resolv.conf. */
int wifi_read_dns(const char * resolv_path, char out[][16], int max_count);
/* A WPA key as wpa_supplicant takes it: a passphrase of 8 to 63
 * characters, or the raw 256-bit PSK as 64 hex digits. */
bool wifi_password_is_valid(const char * password);
bool wifi_password_is_raw_psk(const char * password);
/* A dotted IPv4 address (resolv.conf entries here hold IPv4 only). */
bool wifi_dns_server_is_valid(const char * server);

#endif /* MT_WIFI_CORE_H */
