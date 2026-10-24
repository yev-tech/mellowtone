#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

/* Wi-Fi control, implemented in src/network/wifi/: a shared
 * wpa_supplicant control-socket core with cached state, plus a power back
 * end chosen by the board descriptor (BOARD.wifi.backend). No wpa_cli or ip
 * processes: reads come from the cache or one control-socket request. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_MAX_SSID_LEN 33 /* 32 bytes + NUL, the 802.11 max SSID length */

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int signal_level; /* 0 (weakest) - 3 (strongest), wifi_rssi_to_level() */
    bool secured;
    bool is_current;
} wifi_network_t;

/* Outcome of a connection attempt, once the supplicant has decided. */
typedef enum {
    WIFI_CONNECT_OK = 0,
    WIFI_CONNECT_WRONG_KEY, /* the access point rejected the password */
    WIFI_CONNECT_NOT_FOUND, /* the network is not in range */
    WIFI_CONNECT_FAILED,    /* association or authentication failed otherwise */
    WIFI_CONNECT_TIMEOUT,   /* no decision in time */
    WIFI_CONNECT_ERROR,     /* Wi-Fi off, or the supplicant refused the configuration */
} wifi_connect_result_t;

/* Starts the Wi-Fi core (control-socket worker, cached state). Call once at
 * startup, before wifi_control_restore_boot_power(). */
void wifi_control_init(void);

/* Whether wpa_supplicant answers on its control socket (cached; PING on the
 * worker), i.e. whether Wi-Fi is on at all -- independent of association
 * (wifi_get_status() in wifi_status.h). Cheap; fine on the UI thread. */
bool wifi_control_is_enabled(void);

/* Turns the radio on/off through the board's power back end (X1600: the
 * stock wifi_on.sh / wifi_off.sh; R3 Pro: sys_server WIFI:ON / WIFI:OFF) and
 * waits briefly for the control socket to appear/vanish. DHCP is started
 * on the "connected" event, not here. Saves the choice on the card (unless
 * wifi_control_set_power_persist(false)). Blocking; call off the UI thread. */
void wifi_control_enable(void);
void wifi_control_disable(void);
/* Turns Wi-Fi back on at startup if it was on when Mellowtone last ran
 * (choice in MT_WIFI_POWER_FILE on the card). */
void wifi_control_restore_boot_power(void);
/* false while the idle radio suspend (or system suspend) switches Wi-Fi
 * off, so that transient power-save does not become the saved choice.
 * Returns the previous setting. */
bool wifi_control_set_power_persist(bool persist);
/* True when the DNS Settings screen's servers can be applied on this board
 * (X1600: written to resolv.conf and re-applied by the udhcpc hook on every
 * lease); the R3 Pro takes DNS from the stock udhcpc lease. */
bool wifi_control_supports_custom_dns(void);

/* Starts a radio scan (control socket SCAN); wifi_control_scan_wait() then
 * waits up to timeout_ms for CTRL-EVENT-SCAN-RESULTS. */
void wifi_control_scan_start(void);
bool wifi_control_scan_wait(int timeout_ms);

/* Fills out[] with up to max_count networks from the most recent scan
 * (deduplicated by SSID, strongest BSSID kept; hidden/blank-SSID entries
 * skipped), returns how many were written. Blocking (one socket request). */
int wifi_control_get_results(wifi_network_t * out, int max_count);

/* Joins `ssid` with `password` (NULL/empty for an open network) and waits
 * for the outcome. An existing saved entry for the SSID is updated, not
 * duplicated. WPA-PSK keys are derived in-process (PBKDF2-HMAC-SHA1, salt =
 * SSID, 4096 iterations); SAE (WPA3) is added when the access point
 * advertises it and the supplicant supports it. The network becomes the
 * preferred one (priority) without disabling the others (no
 * select_network). Saved only on success; on failure a new entry is removed
 * and an updated one restored. Blocking (up to ~30 s); call off the UI thread. */
wifi_connect_result_t wifi_control_connect(const char * ssid, const char * password);

/* True for a password wifi_control_connect() accepts: a passphrase of 8 to
 * 63 characters or a raw key of 64 hex digits (empty means an open
 * network and is checked by the caller). */
bool wifi_control_password_is_valid(const char * password);

/* Drops the current association (DISCONNECT) without touching the saved
 * profiles; the supplicant stays disconnected until the next connect.
 * Blocking; call off the UI thread. */
bool wifi_control_disconnect(void);

typedef struct {
    int id; /* wpa_supplicant's own network id -- needed by wifi_control_forget() */
    char ssid[WIFI_MAX_SSID_LEN];
} wifi_saved_network_t;

/* Every network wpa_supplicant has saved (LIST_NETWORKS) -- distinct from
 * wifi_control_get_results()'s scan list. Blocking (one socket request). */
int wifi_control_list_saved(wifi_saved_network_t * out, int max_count);

/* Removes a saved network by id (REMOVE_NETWORK + SAVE_CONFIG). Blocking. */
bool wifi_control_forget(int id);

/* Reconnects to an already-saved network by id, reusing its stored key:
 * it becomes the preferred network (priority) and is enabled, then the
 * supplicant reassociates; waits for the outcome like wifi_control_connect().
 * Other saved networks stay enabled. Blocking; call off the UI thread. */
wifi_connect_result_t wifi_control_connect_saved(int id);

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char ip[16];
    char gateway[16];
    char mac[18];
    int signal_level;
} wifi_info_t;

/* Current connection details for the Wi-Fi info screen -- false if not
 * associated. Blocking (one STATUS request); call off the UI thread. */
bool wifi_control_get_info(wifi_info_t * out);

/* DNS servers currently active, from /etc/resolv.conf. */
int wifi_control_get_dns(char out[][16], int max_count);
/* Custom DNS (wifi_control_supports_custom_dns() boards): applied now,
 * kept on the card and re-applied after every DHCP lease. count == 0 (or
 * only empty strings) goes back to the servers DHCP provides. */
bool wifi_control_set_dns(const char * const * servers, int count);
/* True for a server wifi_control_set_dns() accepts (a dotted IPv4 address). */
bool wifi_control_dns_server_is_valid(const char * server);

#endif /* WIFI_CONTROL_H */
