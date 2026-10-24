/* Generic Wi-Fi back end: power through the board's scripts
 * (X1600: the stock wifi_on.sh / wifi_off.sh) and every network change over
 * the wpa_supplicant control socket -- used by every board, the R3 Pro
 * included (its config is /data/wpa_supplicant.conf, which sys_server only
 * writes when asked to; see wifi_backend_hiby.c).
 *
 * Policy:
 *   - one entry per SSID: an existing one is updated, never duplicated;
 *   - the chosen network gets priority 2, the others at most 1, and stays
 *     enabled with them (no select_network, which disables all others);
 *   - success means CTRL-EVENT-CONNECTED for that network; WRONG_KEY and the
 *     rest are reported; nothing is saved until it worked;
 *   - SAE (WPA3) when the access point and the supplicant both have it;
 *   - DHCP once per new network, for this interface only. */
#include "wifi_backend.h"
#include "wifi_core.h"
#include "subprocess.h"

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PRIORITY_PREFERRED 2
#define PRIORITY_OTHER 1
#define MAX_SAVED 64
#define DHCP_GRACE_MS 3000

/* ---- power -------------------------------------------------------------- */

static bool run_script(const char * path) {
    if (!path || !path[0]) return false;
    char * argv[] = { (char *) path, NULL };
    return subprocess_run(argv, NULL, 0);
}

static bool wpa_power(bool on, const wifi_power_cfg_t * cfg) {
    if (!on) wifi_wpa_dhcp_reset();
    return run_script(on ? cfg->power_on_script : cfg->power_off_script);
}

const wifi_backend_t wifi_backend_wpa = {
    .name = "wpa",
    .power = wpa_power,
    .dhcp_on_connect = true,
    .custom_dns = true,
    .boot_default_from_conf = false,
};

/* ---- helpers ------------------------------------------------------------ */

static void to_hex(const unsigned char * data, size_t len, char * out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * len] = '\0';
}

static bool has_token(const char * list, const char * token) {
    size_t n = strlen(token);
    for (const char * p = list; (p = strstr(p, token)); p += n) {
        bool start = p == list || isspace((unsigned char) p[-1]);
        bool end = p[n] == '\0' || isspace((unsigned char) p[n]);
        if (start && end) return true;
    }
    return false;
}

static int list_saved(wifi_saved_network_t * out, bool * disabled, int max) {
    char buf[8192];
    if (wifi_core_request("LIST_NETWORKS", buf, sizeof(buf)) <= 0) return -1;
    return wifi_parse_list_networks(buf, out, disabled, max);
}

static int get_priority(int id) {
    char cmd[48], reply[32];
    snprintf(cmd, sizeof(cmd), "GET_NETWORK %d priority", id);
    if (wifi_core_request(cmd, reply, sizeof(reply)) <= 0 || strncmp(reply, "FAIL", 4) == 0) return 0;
    return atoi(reply);
}

/* Makes id the preferred network without disabling any other. */
static bool prefer(int id, bool * changed) {
    wifi_saved_network_t saved[MAX_SAVED];
    int n = list_saved(saved, NULL, MAX_SAVED);
    if (n < 0) return false;
    for (int i = 0; i < n; i++) {
        int prio = get_priority(saved[i].id);
        int want = saved[i].id == id ? PRIORITY_PREFERRED : (prio > PRIORITY_OTHER ? PRIORITY_OTHER : prio);
        if (prio == want) continue;
        if (!wifi_core_cmd_ok("SET_NETWORK %d priority %d", saved[i].id, want)) return false;
        *changed = true;
    }
    return true;
}

/* Re-enables networks a select_network (the old X1600 code, the stock
 * CONNECT) left disabled, so the supplicant can roam between all of them. */
static void enable_all_saved(bool * changed) {
    wifi_saved_network_t saved[MAX_SAVED];
    bool disabled[MAX_SAVED];
    int n = list_saved(saved, disabled, MAX_SAVED);
    for (int i = 0; i < n; i++)
        if (disabled[i] && wifi_core_cmd_ok("ENABLE_NETWORK %d", saved[i].id)) *changed = true;
}

static bool supplicant_has_sae(void) {
    char buf[256];
    return wifi_core_request("GET_CAPABILITY key_mgmt", buf, sizeof(buf)) > 0 && has_token(buf, "SAE");
}

/* What the strongest access point for ssid advertises in the last scan. */
static void ap_security(const char * ssid, bool * psk, bool * sae) {
    *psk = true; /* not in the scan (hidden network): plain WPA-PSK */
    *sae = false;
    char * buf = malloc(16384);
    if (!buf) return;
    char flags[160];
    if (wifi_core_request("SCAN_RESULTS", buf, 16384) > 0 && wifi_scan_flags_for(buf, ssid, flags, sizeof(flags))) {
        *sae = strstr(flags, "SAE") != NULL;
        *psk = strstr(flags, "PSK") != NULL || !*sae;
    }
    free(buf);
}

/* ---- connect ------------------------------------------------------------ */

static bool set_security(int id, const char * ssid, const char * password) {
    if (!password || !password[0]) return wifi_core_cmd_ok("SET_NETWORK %d key_mgmt NONE", id);

    bool ap_psk, ap_sae;
    ap_security(ssid, &ap_psk, &ap_sae);
    bool use_sae = ap_sae && supplicant_has_sae();
    bool sae_only = use_sae && !ap_psk;

    if (wifi_password_is_raw_psk(password)) {
        /* The 256-bit key itself: WPA-PSK only, SAE needs the passphrase. */
        if (sae_only || !wifi_core_cmd_ok("SET_NETWORK %d psk %s", id, password)) return false;
        return wifi_core_cmd_ok("SET_NETWORK %d key_mgmt WPA-PSK", id);
    }

    if (!sae_only) {
        /* WPA-PSK: the 256-bit key from the passphrase, as wpa_passphrase does. */
        unsigned char psk[32];
        if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1, (const unsigned char *) password, strlen(password),
                                          (const unsigned char *) ssid, strlen(ssid), 4096, sizeof(psk), psk) != 0)
            return false;
        char psk_hex[sizeof(psk) * 2 + 1];
        to_hex(psk, sizeof(psk), psk_hex);
        bool ok = wifi_core_cmd_ok("SET_NETWORK %d psk %s", id, psk_hex);
        memset(psk, 0, sizeof(psk));
        memset(psk_hex, 0, sizeof(psk_hex));
        if (!ok) return false;
    }
    if (use_sae) {
        /* SAE needs the passphrase itself: sae_password (hex, so any byte
         * is safe), or on supplicants without that field the quoted psk. */
        size_t len = strlen(password);
        char * hex = malloc(len * 2 + 1);
        if (!hex) return false;
        to_hex((const unsigned char *) password, len, hex);
        bool ok = wifi_core_cmd_ok("SET_NETWORK %d sae_password %s", id, hex);
        memset(hex, 0, len * 2);
        free(hex);
        if (!ok && sae_only) ok = !strpbrk(password, "\r\n") && wifi_core_cmd_ok("SET_NETWORK %d psk \"%s\"", id, password);
        if (!ok) {
            if (sae_only) return false;
            use_sae = false; /* transition-mode AP: WPA-PSK alone still works */
        }
    }
    if (use_sae) {
        /* SAE requires management frame protection; transition mode makes it optional. */
        if (!wifi_core_cmd_ok("SET_NETWORK %d key_mgmt %s", id, sae_only ? "SAE" : "WPA-PSK SAE")) return false;
        return wifi_core_cmd_ok("SET_NETWORK %d ieee80211w %d", id, sae_only ? 2 : 1);
    }
    return wifi_core_cmd_ok("SET_NETWORK %d key_mgmt WPA-PSK", id);
}

static void remove_duplicates(int keep_id, const char * ssid) {
    wifi_saved_network_t saved[MAX_SAVED];
    int n = list_saved(saved, NULL, MAX_SAVED);
    for (int i = 0; i < n; i++)
        if (saved[i].id != keep_id && strcmp(saved[i].ssid, ssid) == 0)
            wifi_core_cmd_ok("REMOVE_NETWORK %d", saved[i].id);
}

wifi_connect_result_t wifi_wpa_connect(const char * ssid, const char * password, int timeout_ms) {
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len == 0 || ssid_len >= WIFI_MAX_SSID_LEN || !wifi_core_is_enabled()) return WIFI_CONNECT_ERROR;
    if (password && password[0] && !wifi_password_is_valid(password)) return WIFI_CONNECT_ERROR;

    wifi_saved_network_t saved[MAX_SAVED];
    int n = list_saved(saved, NULL, MAX_SAVED);
    if (n < 0) return WIFI_CONNECT_ERROR;
    int id = -1;
    for (int i = 0; i < n && id < 0; i++)
        if (strcmp(saved[i].ssid, ssid) == 0) id = saved[i].id;
    bool added = id < 0;
    if (added) {
        char reply[32];
        if (wifi_core_request("ADD_NETWORK", reply, sizeof(reply)) <= 0 || !isdigit((unsigned char) reply[0]))
            return WIFI_CONNECT_ERROR;
        id = atoi(reply);
    }

    char ssid_hex[WIFI_MAX_SSID_LEN * 2 + 1];
    to_hex((const unsigned char *) ssid, ssid_len, ssid_hex);
    bool changed = false;
    /* scan_ssid=1: active probes naming this SSID, needed for hidden
     * networks (Manual SSID Entry), harmless for the others. */
    bool ok = wifi_core_cmd_ok("SET_NETWORK %d ssid %s", id, ssid_hex) &&
              wifi_core_cmd_ok("SET_NETWORK %d scan_ssid 1", id) && set_security(id, ssid, password) &&
              prefer(id, &changed);

    wifi_connect_result_t result = WIFI_CONNECT_ERROR;
    if (ok) {
        unsigned seq = wifi_core_event_seq();
        /* ENABLE_NETWORK keeps the others enabled; REASSOCIATE makes the
         * supplicant pick again (now preferring id), also after DISCONNECT. */
        if (wifi_core_cmd_ok("ENABLE_NETWORK %d", id) && wifi_core_request_ok("REASSOCIATE"))
            result = wifi_core_wait_connect(id, seq, timeout_ms);
    }

    if (result == WIFI_CONNECT_OK) {
        remove_duplicates(id, ssid);
        enable_all_saved(&changed);
        wifi_core_request_ok("SAVE_CONFIG");
    } else if (added) {
        wifi_core_cmd_ok("REMOVE_NETWORK %d", id);
    } else {
        /* The old key and priorities are still in the saved file. */
        wifi_core_request_ok("RECONFIGURE");
    }
    return result;
}

wifi_connect_result_t wifi_wpa_connect_saved(int id, int timeout_ms) {
    if (!wifi_core_is_enabled()) return WIFI_CONNECT_ERROR;
    bool changed = false;
    /* Profiles saved before the hidden-SSID fix have scan_ssid=0. */
    char cmd[48], reply[16];
    snprintf(cmd, sizeof(cmd), "GET_NETWORK %d scan_ssid", id);
    if (wifi_core_request(cmd, reply, sizeof(reply)) > 0 && strncmp(reply, "FAIL", 4) != 0 && atoi(reply) != 1) {
        if (wifi_core_cmd_ok("SET_NETWORK %d scan_ssid 1", id)) changed = true;
    }
    if (!prefer(id, &changed)) return WIFI_CONNECT_ERROR;

    unsigned seq = wifi_core_event_seq();
    if (!wifi_core_cmd_ok("ENABLE_NETWORK %d", id) || !wifi_core_request_ok("REASSOCIATE")) return WIFI_CONNECT_ERROR;
    wifi_connect_result_t result = wifi_core_wait_connect(id, seq, timeout_ms);
    if (result == WIFI_CONNECT_OK) {
        enable_all_saved(&changed);
        if (changed) wifi_core_request_ok("SAVE_CONFIG");
    }
    return result;
}

bool wifi_wpa_disconnect(void) {
    return wifi_core_request_ok("DISCONNECT");
}

bool wifi_wpa_forget(int id) {
    if (!wifi_core_cmd_ok("REMOVE_NETWORK %d", id)) return false;
    wifi_core_request_ok("SAVE_CONFIG");
    return true;
}

/* ---- DHCP --------------------------------------------------------------- */

static pthread_mutex_t dhcp_mu = PTHREAD_MUTEX_INITIALIZER;
static char leased_ssid[WIFI_MAX_SSID_LEN];
static void (*dhcp_runner)(const char * iface);

void wifi_wpa_set_dhcp_runner(void (*runner)(const char * iface)) {
    dhcp_runner = runner;
}

void wifi_wpa_dhcp_reset(void) {
    pthread_mutex_lock(&dhcp_mu);
    leased_ssid[0] = '\0';
    pthread_mutex_unlock(&dhcp_mu);
}

/* A udhcpc already serving iface ("-i wlan0", "-iwlan0", "--interface=wlan0"). */
static pid_t udhcpc_for(const char * iface) {
    DIR * d = opendir("/proc");
    if (!d) return -1;
    pid_t found = -1;
    struct dirent * e;
    while (found < 0 && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[300], args[512];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE * f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(args, 1, sizeof(args) - 1, f);
        fclose(f);
        args[n] = '\0';
        const char * argv0 = args;
        const char * base = strrchr(argv0, '/');
        if (strcmp(base ? base + 1 : argv0, "udhcpc") != 0) continue;
        bool next_is_iface = false;
        for (size_t off = strlen(args) + 1; off < n; off += strlen(args + off) + 1) {
            const char * a = args + off;
            if ((next_is_iface && strcmp(a, iface) == 0) || (strncmp(a, "-i", 2) == 0 && strcmp(a + 2, iface) == 0) ||
                (strncmp(a, "--interface=", 12) == 0 && strcmp(a + 12, iface) == 0)) {
                found = (pid_t) atoi(e->d_name);
                break;
            }
            next_is_iface = strcmp(a, "-i") == 0 || strcmp(a, "--interface") == 0;
        }
    }
    closedir(d);
    return found;
}

static void run_udhcpc(const char * iface) {
    pid_t pid = udhcpc_for(iface);
    if (pid > 0 && kill(pid, SIGUSR1) == 0) return; /* renew on the running client */
    char * argv[] = { (char *) "udhcpc", (char *) "-b", (char *) "-i", (char *) iface, (char *) "-q", NULL };
    subprocess_run(argv, NULL, 0);
}

void wifi_wpa_dhcp(const char * iface, const char * ssid, const char * ip) {
    /* An address already on the interface at "connected" may belong to the
     * previous network. Without one, wifi_on.sh's own udhcpc may be getting
     * it: give that a moment, and take what it brings as fresh. */
    bool had_ip = ip && ip[0];
    bool have_ip = had_ip;
    for (int waited = 0; !have_ip && waited < DHCP_GRACE_MS; waited += 250) {
        usleep(250000);
        wifi_state_t s;
        wifi_core_get_state(&s);
        have_ip = s.ip[0] != '\0';
    }
    bool fresh = have_ip && !had_ip;
    pthread_mutex_lock(&dhcp_mu);
    /* A roam within one network keeps its lease. */
    bool need = !have_ip || (!fresh && strcmp(leased_ssid, ssid) != 0);
    snprintf(leased_ssid, sizeof(leased_ssid), "%s", ssid);
    pthread_mutex_unlock(&dhcp_mu);
    if (need) (dhcp_runner ? dhcp_runner : run_udhcpc)(iface);
}
