#ifndef MT_WIFI_BACKEND_H
#define MT_WIFI_BACKEND_H

/* Wi-Fi back ends: power and DHCP/DNS policy per board, chosen
 * by BOARD.wifi.backend. Network changes are the same on every board: the
 * generic control-socket functions below (wifi_backend_wpa.c). */

#include "wifi_control.h"

#include <stdbool.h>

typedef struct {
    const char * iface;
    const char * power_on_script;  /* wpa back end */
    const char * power_off_script; /* wpa back end */
} wifi_power_cfg_t;

typedef struct {
    const char * name;
    bool (*power)(bool on, const wifi_power_cfg_t * cfg);
    /* The player starts DHCP on CTRL-EVENT-CONNECTED (wifi_wpa_dhcp()).
     * false: the system does it (R3 Pro: sys_server's monitor runs udhcpc). */
    bool dhcp_on_connect;
    /* Custom DNS can be applied (resolv.conf is ours to write). */
    bool custom_dns;
    /* No saved power choice yet: on when the supplicant config has a network
     * (the stock player's habit, R3 Pro). Otherwise off. */
    bool boot_default_from_conf;
} wifi_backend_t;

extern const wifi_backend_t wifi_backend_wpa;
extern const wifi_backend_t wifi_backend_hiby;

/* ---- generic network changes over the control socket (all boards) ---- */

/* Add-or-update, preferred by priority, enable + reassociate, wait for the
 * outcome; SAVE_CONFIG on success, undo on failure (wifi_control.h). */
wifi_connect_result_t wifi_wpa_connect(const char * ssid, const char * password, int timeout_ms);
wifi_connect_result_t wifi_wpa_connect_saved(int id, int timeout_ms);
bool wifi_wpa_disconnect(void);
bool wifi_wpa_forget(int id);

/* DHCP for iface after CTRL-EVENT-CONNECTED: once per new network (a roam
 * within the same SSID keeps its lease); renews a running udhcpc for iface
 * (SIGUSR1) or starts `udhcpc -b -i iface -q`. Never touches udhcpc
 * instances of other interfaces. */
void wifi_wpa_dhcp(const char * iface, const char * ssid, const char * ip);
/* Forget the network the last lease was for (Wi-Fi turned off). */
void wifi_wpa_dhcp_reset(void);
/* Test hook: replaces the udhcpc start/renew (NULL restores it). */
void wifi_wpa_set_dhcp_runner(void (*runner)(const char * iface));

#endif /* MT_WIFI_BACKEND_H */
