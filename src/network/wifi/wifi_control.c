/* wifi_control.h / wifi_status.h on top of the Wi-Fi core: the
 * board descriptor picks the power back end (BOARD.wifi.backend); the power
 * choice and custom DNS live on the card; everything else is the shared
 * core and the generic control-socket back end. */
#include "wifi_control.h"
#include "wifi_status.h"
#include "wifi_backend.h"
#include "wifi_core.h"

#include "backlight.h"
#include "board.h"
#include "board_config.h"
#include "storage_paths.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert((unsigned) WIFI_STATUS_EVT_ENABLED == (unsigned) WIFI_EVT_ENABLED_CHANGED &&
                   (unsigned) WIFI_STATUS_EVT_CONNECTED == (unsigned) WIFI_EVT_CONNECTED &&
                   (unsigned) WIFI_STATUS_EVT_DISCONNECTED == (unsigned) WIFI_EVT_DISCONNECTED &&
                   (unsigned) WIFI_STATUS_EVT_SCAN_DONE == (unsigned) WIFI_EVT_SCAN_DONE &&
                   (unsigned) WIFI_STATUS_EVT_WRONG_KEY == (unsigned) WIFI_EVT_WRONG_KEY,
               "wifi_status.h and wifi_core.h event bits differ");

#define CONNECT_TIMEOUT_MS 30000
#define POWER_ON_WAIT_MS 5000
#define POWER_OFF_WAIT_MS 3000
#define RESOLV_CONF "/etc/resolv.conf"
/* Custom DNS for the udhcpc hook (boards/common/overlay/usr/share/udhcpc/
 * default.script.d/mellowtone_dns): RAM only, rewritten at start from the
 * copy on the card. */
#define DNS_HOOK_FILE "/tmp/mellowtone_dns"

static const wifi_backend_t * backend(void) {
    return BOARD.wifi.backend == BOARD_WIFI_HIBY_SYS_SERVER ? &wifi_backend_hiby : &wifi_backend_wpa;
}

static const char * iface(void) {
    return BOARD.wifi.iface ? BOARD.wifi.iface : "wlan0";
}

static wifi_power_cfg_t power_cfg(void) {
    return (wifi_power_cfg_t) {
        .iface = iface(), .power_on_script = BOARD.wifi.power_on, .power_off_script = BOARD.wifi.power_off,
    };
}

/* ---- custom DNS --------------------------------------------------------- */

static pthread_mutex_t dns_mu = PTHREAD_MUTEX_INITIALIZER;

/* Up to two servers, one per line; false when none is set. */
static bool load_custom_dns(char servers[2][16]) {
    servers[0][0] = servers[1][0] = '\0';
    FILE * f = fopen(MT_WIFI_DNS_FILE, "r");
    if (!f) return false;
    char line[64];
    int n = 0;
    while (n < 2 && fgets(line, sizeof(line), f))
        if (sscanf(line, "%15s", servers[n]) == 1 && wifi_dns_server_is_valid(servers[n])) n++;
    for (int i = n; i < 2; i++) servers[i][0] = '\0'; /* a rejected line may be left there */
    fclose(f);
    return n > 0;
}

static bool write_lines(const char * path, const char * prefix, char servers[2][16]) {
    FILE * f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < 2; i++)
        if (servers[i][0]) fprintf(f, "%s%s\n", prefix, servers[i]);
    return fclose(f) == 0;
}

/* resolv.conf from the custom servers (after a lease, or when set). */
static bool apply_custom_dns(void) {
    char servers[2][16];
    pthread_mutex_lock(&dns_mu);
    bool ok = load_custom_dns(servers) && write_lines(RESOLV_CONF, "nameserver ", servers);
    pthread_mutex_unlock(&dns_mu);
    return ok;
}

static void sync_dns_hook_file(void) {
    char servers[2][16];
    pthread_mutex_lock(&dns_mu);
    if (load_custom_dns(servers)) write_lines(DNS_HOOK_FILE, "", servers);
    else unlink(DNS_HOOK_FILE);
    pthread_mutex_unlock(&dns_mu);
}

bool wifi_control_supports_custom_dns(void) {
    return backend()->custom_dns;
}

int wifi_control_get_dns(char out[][16], int max_count) {
    return wifi_read_dns(RESOLV_CONF, out, max_count);
}

bool wifi_control_set_dns(const char * const * servers, int count) {
    if (!backend()->custom_dns) return false;
    char list[2][16] = { "", "" };
    int n = 0;
    for (int i = 0; i < count && n < 2; i++) {
        if (!servers[i] || !servers[i][0]) continue;
        if (!wifi_dns_server_is_valid(servers[i])) return false;
        snprintf(list[n++], sizeof(list[0]), "%s", servers[i]);
    }

    pthread_mutex_lock(&dns_mu);
    bool ok;
    if (n == 0) {
        /* Back to DHCP's servers from the next lease on. */
        unlink(MT_WIFI_DNS_FILE);
        unlink(DNS_HOOK_FILE);
        ok = true;
    } else {
        ok = mt_state_prepare_write(MT_WIFI_DNS_FILE) && write_lines(MT_WIFI_DNS_FILE, "", list);
        write_lines(DNS_HOOK_FILE, "", list);
        /* Applied now as well; without a card it lasts until the next lease. */
        ok = write_lines(RESOLV_CONF, "nameserver ", list) || ok;
    }
    pthread_mutex_unlock(&dns_mu);
    return ok;
}

/* Core helper thread, after CTRL-EVENT-CONNECTED. */
static void on_connected(const char * ifname, const char * ssid, const char * ip) {
    wifi_wpa_dhcp(ifname, ssid, ip);
    /* Images without the udhcpc hook: put the custom servers back after the lease. */
    apply_custom_dns();
}

/* ---- power -------------------------------------------------------------- */

static atomic_bool persist_power_choice = true;

bool wifi_control_set_power_persist(bool persist) {
    return atomic_exchange(&persist_power_choice, persist);
}

static void save_power_choice(bool on) {
    if (!atomic_load(&persist_power_choice) || !mt_state_prepare_write(MT_WIFI_POWER_FILE)) return;
    FILE * f = fopen(MT_WIFI_POWER_FILE, "w");
    if (!f) return;
    fputs(on ? "1\n" : "0\n", f);
    fclose(f);
}

static bool saved_power_choice(void) {
    FILE * f = fopen(MT_WIFI_POWER_FILE, "r");
    if (f) {
        int c = fgetc(f);
        fclose(f);
        return c != '0';
    }
    if (!backend()->boot_default_from_conf || !BOARD.wifi.conf) return false;
    f = fopen(BOARD.wifi.conf, "r");
    if (!f) return false;
    char line[256];
    bool has_network = false;
    while (!has_network && fgets(line, sizeof(line), f)) has_network = strstr(line, "network=") != NULL;
    fclose(f);
    return has_network;
}

void wifi_control_init(void) {
    wifi_core_config_t cfg = {
        .ctrl_dir = BOARD.wifi.ctrl_dir,
        .iface = iface(),
        .screen_on = backlight_screen_is_on,
        .on_connected = backend()->dhcp_on_connect ? on_connected : NULL,
    };
    wifi_core_start(&cfg);
    if (backend()->custom_dns) sync_dns_hook_file();
}

bool wifi_control_is_enabled(void) {
    return wifi_core_is_enabled();
}

void wifi_control_enable(void) {
    save_power_choice(true);
    wifi_power_cfg_t cfg = power_cfg();
    backend()->power(true, &cfg);
    /* Only until the control socket answers: association and DHCP follow
     * as events. */
    wifi_core_wait_enabled(true, POWER_ON_WAIT_MS);
}

void wifi_control_disable(void) {
    save_power_choice(false);
    wifi_power_cfg_t cfg = power_cfg();
    backend()->power(false, &cfg);
    wifi_core_wait_enabled(false, POWER_OFF_WAIT_MS);
}

static void * restore_power_thread(void * arg) {
    (void) arg;
    sleep(4); /* after the UI and the sys_client listener are up */
    if (!wifi_core_wait_enabled(true, 1500)) {
        wifi_power_cfg_t cfg = power_cfg();
        backend()->power(true, &cfg);
        wifi_core_wait_enabled(true, POWER_ON_WAIT_MS);
    }
    return NULL;
}

void wifi_control_restore_boot_power(void) {
    if (!saved_power_choice()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, restore_power_thread, NULL) == 0) pthread_detach(t);
}

/* ---- scan, lists, connections ------------------------------------------- */

void wifi_control_scan_start(void) {
    wifi_core_scan_start();
}

bool wifi_control_scan_wait(int timeout_ms) {
    return wifi_core_scan_wait(timeout_ms);
}

int wifi_control_get_results(wifi_network_t * out, int max_count) {
    return wifi_core_scan_results(out, max_count);
}

int wifi_control_list_saved(wifi_saved_network_t * out, int max_count) {
    return wifi_core_list_saved(out, max_count);
}

bool wifi_control_get_info(wifi_info_t * out) {
    return wifi_core_get_info(out);
}

wifi_connect_result_t wifi_control_connect(const char * ssid, const char * password) {
    return wifi_wpa_connect(ssid, password, CONNECT_TIMEOUT_MS);
}

bool wifi_control_password_is_valid(const char * password) {
    return wifi_password_is_valid(password);
}

bool wifi_control_dns_server_is_valid(const char * server) {
    return wifi_dns_server_is_valid(server);
}

wifi_connect_result_t wifi_control_connect_saved(int id) {
    return wifi_wpa_connect_saved(id, CONNECT_TIMEOUT_MS);
}

bool wifi_control_disconnect(void) {
    return wifi_wpa_disconnect();
}

bool wifi_control_forget(int id) {
    return wifi_wpa_forget(id);
}

/* ---- wifi_status.h ------------------------------------------------------ */

bool wifi_get_status(int * out_signal_level) {
    wifi_state_t s;
    wifi_core_get_state(&s);
    if (!s.enabled || !s.connected) return false;
    *out_signal_level = s.signal_level;
    return true;
}

bool wifi_status_last_ssid(char * out, size_t size) {
    if (!out || size == 0) return false;
    wifi_state_t s;
    wifi_core_get_state(&s);
    snprintf(out, size, "%s", s.enabled && s.connected ? s.ssid : "");
    return out[0] != '\0';
}

unsigned wifi_status_generation(void) {
    return wifi_core_generation();
}

unsigned wifi_status_take_events(void) {
    return wifi_core_take_events();
}
