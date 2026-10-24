/* R3 Pro Wi-Fi power through HiBy's sys_server (WIFI:ON / WIFI:OFF), which
 * loads the bcmdhd firmware paths, runs /sbin/wifi_on.sh (wpa_supplicant on
 * /data/wpa_supplicant.conf + udhcpc) and attaches its own event monitor.
 *
 * Network changes go through the generic control-socket code
 * (wifi_backend_wpa.c), not sys_server's WIFI:SSID_CONNECT/CONNECT/REMOVE:
 * with no stock player left there is nobody to share its conventions with.
 * sys_server never rewrites the supplicant config on its own (decompiled:
 * only SSID_CONNECT, CONNECT and REMOVE call save_config,
 * and only when sent; its monitor thread runs `udhcpc -b -i wlan0 -q` on
 * CTRL-EVENT-CONNECTED and dumps status/list_network to /data/wifi_*.txt),
 * so DHCP is left to it here. */
#include "wifi_backend.h"
#include "hiby_sys_client.h"

#include <stdio.h>
#include <string.h>

/* WIFI:ON runs wifi_on.sh (about 2 s). */
#define WIFI_POWER_TIMEOUT_MS 20000

static bool hiby_power(bool on, const wifi_power_cfg_t * cfg) {
    char cmd[64], ok[32], fail[32];
    const char * verb = on ? "ON" : "OFF";
    snprintf(cmd, sizeof(cmd), "WIFI:%s:%s", verb, cfg->iface);
    snprintf(ok, sizeof(ok), "WIFI:%s:OK", verb);
    snprintf(fail, sizeof(fail), "WIFI:%s:FAIL", verb);
    const char * prefixes[] = { ok, fail, NULL };
    char result[128];
    if (!hiby_sys_request(cmd, prefixes, WIFI_POWER_TIMEOUT_MS, result, sizeof(result))) return false;
    return strcmp(result, ok) == 0;
}

const wifi_backend_t wifi_backend_hiby = {
    .name = "hiby",
    .power = hiby_power,
    .dhcp_on_connect = false, /* sys_server's monitor runs udhcpc on CONNECTED */
    .custom_dns = false,      /* stock udhcpc script, no hook directory */
    .boot_default_from_conf = true,
};
