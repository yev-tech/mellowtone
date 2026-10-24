/* Wi-Fi core tests: wpa_ctrl + wifi_core + the generic
 * control-socket back end against a fake wpa_supplicant.
 *
 * The fake binds <tmp>/ctrl/wlan0 like wpa_supplicant's ctrl_interface and
 * answers the commands the player sends with wpa_supplicant 2.x reply text
 * (ctrl_iface.c: STATUS key=value lines, tab-separated SCAN_RESULTS and
 * LIST_NETWORKS with their header rows, printf-escaped SSIDs, "OK"/"FAIL",
 * GET_NETWORK values without a newline), and sends "<3>CTRL-EVENT-..."
 * events to ATTACHed clients. Access points have a password; on
 * REASSOCIATE the fake picks the enabled network with the highest priority
 * that is in range and checks the configured key (PBKDF2 PSK, SAE password,
 * open) the way an access point would. */
#include "wifi_backend.h"
#include "wifi_core.h"
#include "wpa_ctrl.h"

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* ---- fake wpa_supplicant ------------------------------------------------- */

typedef struct {
    const char * ssid;
    const char * bssid;
    int signal;
    const char * flags;
    const char * password; /* NULL: open */
    bool psk, sae, silent;
} fake_ap_t;

typedef struct {
    int id;
    bool used;
    char ssid[33];
    char key_mgmt[32];
    char psk[80];
    char sae_password[160];
    int priority, scan_ssid, ieee80211w;
    bool disabled;
} fake_net_t;

#define FAKE_NETS 16
#define FAKE_LOG 4096

static struct {
    char dir[256];
    char path[300];
    int fd;
    pthread_t thread;
    atomic_bool stop;
    pthread_mutex_t mu;
    fake_ap_t aps[8];
    int ap_count;
    fake_net_t nets[FAKE_NETS], saved[FAKE_NETS];
    int connected; /* network id or -1 */
    struct sockaddr_un attached[4];
    int attached_count;
    char log[FAKE_LOG];
    int saves;
    bool mute;     /* never reply */
    bool no_scan_event;
    bool sae_capable;
    bool sae_field; /* sae_password known */
} fake = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void to_hex(const unsigned char * d, size_t n, char * out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[2 * n] = '\0';
}

static void ssid_encode(const char * in, char * out, size_t size) {
    size_t o = 0;
    for (const unsigned char * p = (const unsigned char *) in; *p && o + 5 < size; p++) {
        if (*p == '\\' || *p == '"') o += snprintf(out + o, size - o, "\\%c", *p);
        else if (*p >= 32 && *p < 127) out[o++] = (char) *p;
        else o += snprintf(out + o, size - o, "\\x%02x", *p);
    }
    out[o] = '\0';
}

static fake_net_t * net_by_id(int id) {
    for (int i = 0; i < FAKE_NETS; i++)
        if (fake.nets[i].used && fake.nets[i].id == id) return &fake.nets[i];
    return NULL;
}

static const fake_ap_t * ap_by_ssid(const char * ssid) {
    for (int i = 0; i < fake.ap_count; i++)
        if (strcmp(fake.aps[i].ssid, ssid) == 0) return &fake.aps[i];
    return NULL;
}

static fake_net_t * add_net(const char * ssid, int priority, bool disabled) {
    int id = 0;
    for (int i = 0; i < FAKE_NETS; i++)
        if (fake.nets[i].used && fake.nets[i].id >= id) id = fake.nets[i].id + 1;
    for (int i = 0; i < FAKE_NETS; i++)
        if (!fake.nets[i].used) {
            fake.nets[i] = (fake_net_t) { .used = true, .id = id, .priority = priority, .disabled = disabled };
            snprintf(fake.nets[i].ssid, sizeof(fake.nets[i].ssid), "%s", ssid);
            snprintf(fake.nets[i].key_mgmt, sizeof(fake.nets[i].key_mgmt), "WPA-PSK WPA-EAP");
            return &fake.nets[i];
        }
    return NULL;
}

static void psk_for(const char * password, const char * ssid, char * hex) {
    unsigned char psk[32];
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1, (const unsigned char *) password, strlen(password),
                                  (const unsigned char *) ssid, strlen(ssid), 4096, sizeof(psk), psk);
    to_hex(psk, sizeof(psk), hex);
}

/* Would this network's configuration authenticate against ap? */
static bool credentials_ok(const fake_net_t * n, const fake_ap_t * ap) {
    if (!ap->password) return strcmp(n->key_mgmt, "NONE") == 0;
    if (ap->psk && strstr(n->key_mgmt, "WPA-PSK")) {
        char hex[65];
        psk_for(ap->password, ap->ssid, hex);
        if (strcmp(n->psk, hex) == 0) return true;
    }
    if (ap->sae && strstr(n->key_mgmt, "SAE") && n->ieee80211w >= 1) {
        char hex[160], quoted[80];
        to_hex((const unsigned char *) ap->password, strlen(ap->password), hex);
        snprintf(quoted, sizeof(quoted), "\"%s\"", ap->password);
        if (strcmp(n->sae_password, hex) == 0 || strcmp(n->psk, quoted) == 0) return true;
    }
    return false;
}

static void send_event(const char * fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    for (int i = 0; i < fake.attached_count; i++)
        sendto(fake.fd, msg, strlen(msg), 0, (struct sockaddr *) &fake.attached[i], sizeof(fake.attached[i]));
}

/* The best enabled network in range, excluding `skip`. */
static fake_net_t * select_best(int skip) {
    fake_net_t * best = NULL;
    for (int i = 0; i < FAKE_NETS; i++) {
        fake_net_t * n = &fake.nets[i];
        if (!n->used || n->disabled || n->id == skip || !ap_by_ssid(n->ssid)) continue;
        if (!best || n->priority > best->priority) best = n;
    }
    return best;
}

static void reassociate(void) {
    if (fake.connected >= 0) {
        send_event("<3>CTRL-EVENT-DISCONNECTED bssid=00:11:22:33:44:55 reason=3 locally_generated=1");
        fake.connected = -1;
    }
    fake_net_t * cand = select_best(-1);
    if (!cand) {
        for (int i = 0; i < 3; i++) send_event("<3>CTRL-EVENT-NETWORK-NOT-FOUND");
        return;
    }
    const fake_ap_t * ap = ap_by_ssid(cand->ssid);
    if (ap->silent) return;
    if (credentials_ok(cand, ap)) {
        fake.connected = cand->id;
        send_event("<3>CTRL-EVENT-CONNECTED - Connection to %s completed [id=%d id_str=]", ap->bssid, cand->id);
        return;
    }
    char esc[140];
    ssid_encode(cand->ssid, esc, sizeof(esc));
    send_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 reason=WRONG_KEY",
               cand->id, esc);
    fake_net_t * fallback = select_best(cand->id);
    if (fallback && credentials_ok(fallback, ap_by_ssid(fallback->ssid))) {
        fake.connected = fallback->id;
        send_event("<3>CTRL-EVENT-CONNECTED - Connection to %s completed [id=%d id_str=]",
                   ap_by_ssid(fallback->ssid)->bssid, fallback->id);
    }
}

static int set_network(fake_net_t * n, const char * field, const char * value) {
    if (strcmp(field, "ssid") == 0) {
        if (value[0] == '"') {
            snprintf(n->ssid, sizeof(n->ssid), "%.*s", (int) strlen(value) - 2, value + 1);
        } else {
            size_t len = strlen(value) / 2;
            if (len > 32) return -1;
            for (size_t i = 0; i < len; i++) {
                unsigned v;
                sscanf(value + 2 * i, "%2x", &v);
                n->ssid[i] = (char) v;
            }
            n->ssid[len] = '\0';
        }
    } else if (strcmp(field, "psk") == 0) {
        snprintf(n->psk, sizeof(n->psk), "%s", value);
    } else if (strcmp(field, "sae_password") == 0) {
        if (!fake.sae_field) return -1;
        snprintf(n->sae_password, sizeof(n->sae_password), "%s", value);
    } else if (strcmp(field, "key_mgmt") == 0) {
        snprintf(n->key_mgmt, sizeof(n->key_mgmt), "%s", value);
    } else if (strcmp(field, "priority") == 0) {
        n->priority = atoi(value);
    } else if (strcmp(field, "scan_ssid") == 0) {
        n->scan_ssid = atoi(value);
    } else if (strcmp(field, "ieee80211w") == 0) {
        n->ieee80211w = atoi(value);
    } else {
        return -1;
    }
    return 0;
}

static void handle(const char * cmd, char * reply, size_t size) {
    char arg1[64] = "", arg2[64] = "", arg3[200] = "";
    int id;
    if (strcmp(cmd, "PING") == 0) {
        snprintf(reply, size, "PONG\n");
    } else if (strcmp(cmd, "STATUS") == 0) {
        fake_net_t * n = fake.connected >= 0 ? net_by_id(fake.connected) : NULL;
        if (n) {
            char esc[140];
            ssid_encode(n->ssid, esc, sizeof(esc));
            snprintf(reply, size,
                     "bssid=%s\nfreq=2412\nssid=%s\nid=%d\nmode=station\npairwise_cipher=CCMP\n"
                     "group_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\nip_address=192.168.1.23\n"
                     "p2p_device_address=aa:bb:cc:dd:ee:ff\naddress=aa:bb:cc:dd:ee:ff\n"
                     "uuid=12345678-9abc-def0-1234-56789abcdef0\n",
                     ap_by_ssid(n->ssid)->bssid, esc, n->id);
        } else {
            snprintf(reply, size, "wpa_state=DISCONNECTED\nip_address=192.168.1.23\naddress=aa:bb:cc:dd:ee:ff\n"
                                  "uuid=12345678-9abc-def0-1234-56789abcdef0\n");
        }
    } else if (strcmp(cmd, "SIGNAL_POLL") == 0) {
        snprintf(reply, size, "RSSI=-55\nLINKSPEED=72\nNOISE=9999\nFREQUENCY=2412\n");
    } else if (strcmp(cmd, "SCAN") == 0) {
        snprintf(reply, size, "OK\n");
    } else if (strcmp(cmd, "SCAN_RESULTS") == 0) {
        size_t o = snprintf(reply, size, "bssid / frequency / signal level / flags / ssid\n");
        for (int i = 0; i < fake.ap_count; i++) {
            char esc[140];
            ssid_encode(fake.aps[i].ssid, esc, sizeof(esc));
            o += snprintf(reply + o, size - o, "%s\t2412\t%d\t%s\t%s\n", fake.aps[i].bssid, fake.aps[i].signal,
                          fake.aps[i].flags, esc);
        }
        /* A hidden network: empty SSID. */
        snprintf(reply + o, size - o, "66:55:44:33:22:11\t2437\t-40\t[WPA2-PSK-CCMP][ESS]\t\n");
    } else if (strcmp(cmd, "LIST_NETWORKS") == 0) {
        size_t o = snprintf(reply, size, "network id / ssid / bssid / flags\n");
        for (int i = 0; i < FAKE_NETS; i++) {
            fake_net_t * n = &fake.nets[i];
            if (!n->used) continue;
            char esc[140];
            ssid_encode(n->ssid, esc, sizeof(esc));
            o += snprintf(reply + o, size - o, "%d\t%s\tany\t%s%s\n", n->id, esc,
                          n->id == fake.connected ? "[CURRENT]" : "", n->disabled ? "[DISABLED]" : "");
        }
    } else if (strcmp(cmd, "ADD_NETWORK") == 0) {
        fake_net_t * n = add_net("", 0, true);
        snprintf(reply, size, n ? "%d\n" : "FAIL\n", n ? n->id : 0);
    } else if (sscanf(cmd, "SET_NETWORK %d %63s %199[^\n]", &id, arg2, arg3) == 3) {
        fake_net_t * n = net_by_id(id);
        snprintf(reply, size, n && set_network(n, arg2, arg3) == 0 ? "OK\n" : "FAIL\n");
    } else if (sscanf(cmd, "GET_NETWORK %d %63s", &id, arg2) == 2) {
        fake_net_t * n = net_by_id(id);
        if (!n) snprintf(reply, size, "FAIL\n");
        else if (strcmp(arg2, "priority") == 0) snprintf(reply, size, "%d", n->priority);
        else if (strcmp(arg2, "scan_ssid") == 0) snprintf(reply, size, "%d", n->scan_ssid);
        else if (strcmp(arg2, "key_mgmt") == 0) snprintf(reply, size, "%s", n->key_mgmt);
        else snprintf(reply, size, "FAIL\n");
    } else if (sscanf(cmd, "ENABLE_NETWORK %63s", arg1) == 1) {
        fake_net_t * n = net_by_id(atoi(arg1));
        if (strcmp(arg1, "all") == 0) {
            for (int i = 0; i < FAKE_NETS; i++) fake.nets[i].disabled = false;
            snprintf(reply, size, "OK\n");
        } else {
            if (n) n->disabled = false;
            snprintf(reply, size, n ? "OK\n" : "FAIL\n");
        }
    } else if (sscanf(cmd, "REMOVE_NETWORK %d", &id) == 1) {
        fake_net_t * n = net_by_id(id);
        if (n) {
            n->used = false;
            if (fake.connected == id) fake.connected = -1;
        }
        snprintf(reply, size, n ? "OK\n" : "FAIL\n");
    } else if (strcmp(cmd, "SAVE_CONFIG") == 0) {
        memcpy(fake.saved, fake.nets, sizeof(fake.nets));
        fake.saves++;
        snprintf(reply, size, "OK\n");
    } else if (strcmp(cmd, "RECONFIGURE") == 0) {
        memcpy(fake.nets, fake.saved, sizeof(fake.nets));
        snprintf(reply, size, "OK\n");
    } else if (strcmp(cmd, "REASSOCIATE") == 0 || strcmp(cmd, "DISCONNECT") == 0) {
        snprintf(reply, size, "OK\n");
    } else if (strcmp(cmd, "GET_CAPABILITY key_mgmt") == 0) {
        snprintf(reply, size, fake.sae_capable ? "NONE IEEE8021X WPA-PSK WPA-EAP SAE\n" : "NONE IEEE8021X WPA-PSK WPA-EAP\n");
    } else {
        snprintf(reply, size, "UNKNOWN COMMAND\n");
    }
}

static void * fake_thread(void * arg) {
    (void) arg;
    while (!atomic_load(&fake.stop)) {
        struct pollfd pfd = { .fd = fake.fd, .events = POLLIN };
        if (poll(&pfd, 1, 20) <= 0) continue;
        char cmd[512];
        struct sockaddr_un from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fake.fd, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *) &from, &from_len);
        if (n <= 0) continue;
        cmd[n] = '\0';
        pthread_mutex_lock(&fake.mu);
        size_t used = strlen(fake.log);
        snprintf(fake.log + used, sizeof(fake.log) - used, "%s\n", cmd);
        if (fake.mute) {
            pthread_mutex_unlock(&fake.mu);
            continue;
        }
        char * reply = malloc(16384);
        reply[0] = '\0';
        if (strcmp(cmd, "ATTACH") == 0) {
            if (fake.attached_count < 4) fake.attached[fake.attached_count++] = from;
            snprintf(reply, 16384, "OK\n");
        } else if (strcmp(cmd, "DETACH") == 0) {
            fake.attached_count = 0;
            snprintf(reply, 16384, "OK\n");
        } else {
            handle(cmd, reply, 16384);
        }
        sendto(fake.fd, reply, strlen(reply), 0, (struct sockaddr *) &from, from_len);
        free(reply);
        /* Events after the reply, as the supplicant does. */
        if (strcmp(cmd, "SCAN") == 0 && !fake.no_scan_event) {
            send_event("<3>CTRL-EVENT-SCAN-STARTED ");
            send_event("<3>CTRL-EVENT-SCAN-RESULTS ");
        } else if (strcmp(cmd, "REASSOCIATE") == 0) {
            reassociate();
        } else if (strcmp(cmd, "DISCONNECT") == 0 && fake.connected >= 0) {
            fake.connected = -1;
            send_event("<3>CTRL-EVENT-DISCONNECTED bssid=00:11:22:33:44:55 reason=3 locally_generated=1");
        }
        pthread_mutex_unlock(&fake.mu);
    }
    return NULL;
}

static bool fake_bind(void) {
    fake.fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", fake.path);
    unlink(fake.path);
    return bind(fake.fd, (struct sockaddr *) &addr, sizeof(addr)) == 0;
}

static void fake_start(void) {
    atomic_store(&fake.stop, false);
    if (!fake_bind()) {
        perror("fake bind");
        exit(2);
    }
    pthread_create(&fake.thread, NULL, fake_thread, NULL);
}

/* Supplicant gone. stale_file: killed (-9), its socket file stays behind. */
static void fake_kill(bool stale_file, bool terminating_event) {
    pthread_mutex_lock(&fake.mu);
    if (terminating_event) send_event("<3>CTRL-EVENT-TERMINATING");
    pthread_mutex_unlock(&fake.mu);
    atomic_store(&fake.stop, true);
    pthread_join(fake.thread, NULL);
    close(fake.fd);
    if (!stale_file) unlink(fake.path);
    fake.attached_count = 0;
}

static void fake_reset_log(void) {
    pthread_mutex_lock(&fake.mu);
    fake.log[0] = '\0';
    pthread_mutex_unlock(&fake.mu);
}

static bool fake_log_has(const char * needle) {
    pthread_mutex_lock(&fake.mu);
    bool has = strstr(fake.log, needle) != NULL;
    pthread_mutex_unlock(&fake.mu);
    return has;
}

static int fake_count_ssid(const char * ssid) {
    pthread_mutex_lock(&fake.mu);
    int n = 0;
    for (int i = 0; i < FAKE_NETS; i++)
        if (fake.nets[i].used && strcmp(fake.nets[i].ssid, ssid) == 0) n++;
    pthread_mutex_unlock(&fake.mu);
    return n;
}

static fake_net_t fake_net_copy(const char * ssid) {
    fake_net_t copy = { .id = -1 };
    pthread_mutex_lock(&fake.mu);
    for (int i = 0; i < FAKE_NETS; i++)
        if (fake.nets[i].used && strcmp(fake.nets[i].ssid, ssid) == 0) {
            copy = fake.nets[i];
            break;
        }
    pthread_mutex_unlock(&fake.mu);
    return copy;
}

/* ---- pure helpers -------------------------------------------------------- */

static void test_parsers(void) {
    char out[64];
    wifi_ssid_decode("Caf\\xc3\\xa9 \\\"Q\\\" \\\\ \\x4", out, sizeof(out));
    CHECK(strcmp(out, "Caf\xc3\xa9 \"Q\" \\ x4") == 0, "ssid decode: %s", out);

    CHECK(wifi_rssi_to_level(-45) == 3 && wifi_rssi_to_level(-55) == 2 && wifi_rssi_to_level(-65) == 1 &&
              wifi_rssi_to_level(-80) == 0,
          "rssi buckets");

    char status[] = "bssid=00:11:22:33:44:55\nssid=Home\nid=3\nwpa_state=COMPLETED\nip_address=10.0.0.2\n";
    char v[32];
    CHECK(wifi_kv_get(status, "ssid", v, sizeof(v)) && strcmp(v, "Home") == 0, "kv ssid");
    CHECK(wifi_kv_get(status, "bssid", v, sizeof(v)) && strcmp(v, "00:11:22:33:44:55") == 0, "kv first line");
    CHECK(!wifi_kv_get(status, "p2p", v, sizeof(v)) && v[0] == '\0', "kv missing");

    char scan[] = "bssid / frequency / signal level / flags / ssid\n"
                  "00:11:22:33:44:55\t2412\t-72\t[WPA2-PSK-CCMP][ESS]\tHome\n"
                  "00:11:22:33:44:56\t5180\t-48\t[WPA2-PSK-CCMP][ESS]\tHome\n"
                  "00:11:22:33:44:57\t2437\t-61\t[ESS]\tCaf\\xc3\\xa9\n"
                  "00:11:22:33:44:58\t2437\t-50\t[RSN-SAE-CCMP][ESS]\tWpa3\n"
                  "00:11:22:33:44:59\t2437\t-30\t[WPA2-PSK-CCMP][ESS]\t\n";
    char flags[64];
    CHECK(wifi_scan_flags_for(scan, "Wpa3", flags, sizeof(flags)) && strcmp(flags, "[RSN-SAE-CCMP][ESS]") == 0,
          "flags for Wpa3: %s", flags);
    CHECK(!wifi_scan_flags_for(scan, "Nope", flags, sizeof(flags)), "flags for a missing SSID");
    wifi_network_t nets[8];
    int n = wifi_parse_scan_results(scan, "Home", nets, 8);
    CHECK(n == 3, "scan results: %d (dedupe by SSID, hidden skipped)", n);
    CHECK(n == 3 && strcmp(nets[0].ssid, "Home") == 0 && nets[0].signal_level == 3 && nets[0].is_current &&
              nets[0].secured,
          "strongest BSSID kept, current marked");
    CHECK(n == 3 && strcmp(nets[1].ssid, "Caf\xc3\xa9") == 0 && !nets[1].secured && !nets[1].is_current, "open, decoded");
    CHECK(n == 3 && nets[2].secured, "SAE counts as secured");

    CHECK(wifi_password_is_valid("12345678") && !wifi_password_is_valid("1234567") && !wifi_password_is_valid(""),
          "passphrase lower bound");
    char key[66];
    memset(key, 'a', 63);
    key[63] = '\0';
    CHECK(wifi_password_is_valid(key) && !wifi_password_is_raw_psk(key), "63 characters is a passphrase");
    key[63] = '0';
    key[64] = '\0';
    CHECK(wifi_password_is_valid(key) && wifi_password_is_raw_psk(key), "64 hex digits is a raw key");
    key[10] = 'z';
    CHECK(!wifi_password_is_valid(key), "64 characters that are not all hex");
    key[10] = 'a';
    key[64] = 'a';
    key[65] = '\0';
    CHECK(!wifi_password_is_valid(key), "65 characters");
    CHECK(wifi_dns_server_is_valid("1.1.1.1") && wifi_dns_server_is_valid("192.168.0.254"), "dns ok");
    CHECK(!wifi_dns_server_is_valid("") && !wifi_dns_server_is_valid("1.1.1") &&
              !wifi_dns_server_is_valid("dns.example") && !wifi_dns_server_is_valid("1.1.1.1 ; x") &&
              !wifi_dns_server_is_valid("256.1.1.1"),
          "dns rejected");

    char list[] = "network id / ssid / bssid / flags\n0\tHome\tany\t[CURRENT]\n1\t\tany\t[DISABLED]\n2\tCafe\tany\t\n";
    wifi_saved_network_t saved[8];
    bool disabled[8];
    n = wifi_parse_list_networks(list, saved, disabled, 8);
    CHECK(n == 3 && saved[1].id == 1 && saved[1].ssid[0] == '\0' && disabled[1] && !disabled[0] && !disabled[2] &&
              strcmp(saved[2].ssid, "Cafe") == 0,
          "list networks with an empty SSID and [DISABLED]");
}

/* ---- core and back end against the fake ---------------------------------- */

static atomic_int dhcp_runs;
static void count_dhcp(const char * iface) {
    (void) iface;
    atomic_fetch_add(&dhcp_runs, 1);
}

static void on_connected(const char * iface, const char * ssid, const char * ip) {
    wifi_wpa_dhcp(iface, ssid, ip);
}

static bool wait_for(bool (*cond)(void), int ms) {
    for (int i = 0; i < ms / 10; i++) {
        if (cond()) return true;
        usleep(10000);
    }
    return cond();
}

static int dhcp_target;
static bool dhcp_reached(void) {
    return atomic_load(&dhcp_runs) >= dhcp_target;
}

static void test_against_fake(const char * tmp) {
    snprintf(fake.dir, sizeof(fake.dir), "%s/ctrl", tmp);
    mkdir(fake.dir, 0700);
    snprintf(fake.path, sizeof(fake.path), "%s/wlan0", fake.dir);
    fake.ap_count = 0;
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Home", .bssid = "00:11:22:33:44:55", .signal = -48,
                                              .flags = "[WPA2-PSK-CCMP][ESS]", .password = "homepass1", .psk = true };
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Cafe", .bssid = "00:11:22:33:44:66", .signal = -66,
                                              .flags = "[WPA2-PSK-CCMP][ESS]", .password = "coffee123", .psk = true };
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Wpa3", .bssid = "00:11:22:33:44:77", .signal = -60,
                                              .flags = "[RSN-SAE-CCMP][ESS]", .password = "sae-secret", .sae = true };
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Mixed", .bssid = "00:11:22:33:44:88", .signal = -60,
                                              .flags = "[WPA2-PSK+SAE-CCMP][ESS]", .password = "mixedpass",
                                              .psk = true, .sae = true };
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Caf\xc3\xa9", .bssid = "00:11:22:33:44:99", .signal = -70,
                                              .flags = "[ESS]" };
    fake.aps[fake.ap_count++] = (fake_ap_t) { .ssid = "Slow", .bssid = "00:11:22:33:44:aa", .signal = -70,
                                              .flags = "[WPA2-PSK-CCMP][ESS]", .password = "slowslow", .psk = true,
                                              .silent = true };
    fake.sae_capable = true;
    fake.sae_field = true;
    fake.connected = -1;
    /* Saved: Home (connected), Cafe twice (the old duplicate bug), the
     * second disabled by an old select_network. */
    fake_net_t * home = add_net("Home", 0, false);
    char hex[65];
    psk_for("homepass1", "Home", hex);
    snprintf(home->psk, sizeof(home->psk), "%s", hex);
    snprintf(home->key_mgmt, sizeof(home->key_mgmt), "WPA-PSK");
    add_net("Cafe", 0, false);
    add_net("Cafe", 0, true);
    fake.connected = home->id;
    memcpy(fake.saved, fake.nets, sizeof(fake.nets));
    fake_start();

    char route[300];
    snprintf(route, sizeof(route), "%s/route", tmp);
    FILE * f = fopen(route, "w");
    fputs("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
          "eth0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
          "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
          "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n",
          f);
    fclose(f);
    char gw[16];
    CHECK(wifi_route_gateway(route, "wlan0", gw, sizeof(gw)) && strcmp(gw, "192.168.1.1") == 0, "gateway %s", gw);

    wifi_wpa_set_dhcp_runner(count_dhcp);
    wifi_core_config_t cfg = {
        .ctrl_dir = fake.dir, .iface = "wlan0", .client_dir = tmp, .route_file = route,
        .on_connected = on_connected, .probe_interval_ms = 50, .request_timeout_ms = 300,
    };
    CHECK(wifi_core_start(&cfg), "core start");
    CHECK(wifi_core_wait_enabled(true, 3000), "enabled after PING/ATTACH");

    wifi_state_t s;
    wifi_core_get_state(&s);
    CHECK(s.enabled && s.connected && strcmp(s.ssid, "Home") == 0 && s.network_id == home->id, "cached: %s", s.ssid);
    CHECK(strcmp(s.ip, "192.168.1.23") == 0 && strcmp(s.gateway, "192.168.1.1") == 0 &&
              strcmp(s.mac, "aa:bb:cc:dd:ee:ff") == 0 && s.rssi == -55 && s.signal_level == 2,
          "cached details ip=%s gw=%s mac=%s rssi=%d", s.ip, s.gateway, s.mac, s.rssi);
    CHECK(wifi_core_take_events() & WIFI_EVT_ENABLED_CHANGED, "enabled event");

    /* Scan: SCAN, then CTRL-EVENT-SCAN-RESULTS. */
    CHECK(wifi_core_scan_start(), "scan start");
    CHECK(wifi_core_scan_wait(2000), "scan results event");
    wifi_network_t nets[16];
    int n = wifi_core_scan_results(nets, 16);
    CHECK(n == 6 && nets[0].is_current, "scan results %d", n);
    wifi_saved_network_t saved[16];
    CHECK(wifi_core_list_saved(saved, 16) == 3, "saved list");
    wifi_info_t info;
    CHECK(wifi_core_get_info(&info) && strcmp(info.ssid, "Home") == 0 && strcmp(info.gateway, "192.168.1.1") == 0,
          "info");

    /* Connect to a saved SSID: the existing entry is updated (no
     * ADD_NETWORK), the duplicate removed, the disabled one re-enabled,
     * priorities set, never select_network; saved once it worked. */
    fake_reset_log();
    int saves = fake.saves;
    wifi_core_take_events();
    wifi_connect_result_t r = wifi_wpa_connect("Cafe", "coffee123", 3000);
    CHECK(r == WIFI_CONNECT_OK, "connect Cafe: %d", r);
    CHECK(!fake_log_has("ADD_NETWORK"), "existing entry updated, not added");
    CHECK(!fake_log_has("SELECT_NETWORK"), "no select_network");
    CHECK(fake_log_has("ENABLE_NETWORK") && fake_log_has("REASSOCIATE"), "enable_network + reassociate");
    CHECK(fake_count_ssid("Cafe") == 1, "duplicate Cafe removed: %d", fake_count_ssid("Cafe"));
    CHECK(fake.saves == saves + 1, "saved once");
    fake_net_t cafe = fake_net_copy("Cafe"), home_now = fake_net_copy("Home");
    CHECK(cafe.priority == 2 && home_now.priority <= 1 && !home_now.disabled && !cafe.disabled && cafe.scan_ssid == 1,
          "priorities cafe=%d home=%d", cafe.priority, home_now.priority);
    CHECK(strcmp(cafe.key_mgmt, "WPA-PSK") == 0, "key_mgmt %s", cafe.key_mgmt);
    wifi_core_get_state(&s);
    CHECK(s.connected && strcmp(s.ssid, "Cafe") == 0, "now on Cafe");
    CHECK(wifi_core_take_events() & WIFI_EVT_CONNECTED, "connected event");
    /* DHCP: a different network than the one with the address -> once. */
    dhcp_target = 1;
    CHECK(wait_for(dhcp_reached, 2000) && atomic_load(&dhcp_runs) == 1, "dhcp once: %d", atomic_load(&dhcp_runs));

    /* Wrong password for a new network: WRONG_KEY, the entry removed again,
     * nothing saved; the supplicant falls back to the network it had. */
    fake.aps[1].password = "coffee123";
    fake_reset_log();
    saves = fake.saves;
    int before = fake_count_ssid("Mixed");
    fake.sae_capable = false; /* Mixed is a transition AP: plain WPA-PSK then */
    r = wifi_wpa_connect("Mixed", "wrongpass", 3000);
    CHECK(r == WIFI_CONNECT_WRONG_KEY, "wrong key reported: %d", r);
    CHECK(fake_log_has("ADD_NETWORK") && fake_log_has("REMOVE_NETWORK"), "new entry removed after failure");
    CHECK(fake_count_ssid("Mixed") == before, "no leftover entry");
    CHECK(fake.saves == saves, "nothing saved after a failure");
    CHECK(!fake_log_has("sae_password"), "no SAE without supplicant support");
    CHECK(wifi_core_take_events() & WIFI_EVT_WRONG_KEY, "wrong-key event for the UI");

    /* Right password, transition AP, supplicant without SAE: WPA-PSK. */
    r = wifi_wpa_connect("Mixed", "mixedpass", 3000);
    fake_net_t mixed = fake_net_copy("Mixed");
    CHECK(r == WIFI_CONNECT_OK && strcmp(mixed.key_mgmt, "WPA-PSK") == 0, "mixed: %d %s", r, mixed.key_mgmt);

    /* SAE-only AP with SAE support: key_mgmt SAE, PMF required, sae_password. */
    fake.sae_capable = true;
    r = wifi_wpa_connect("Wpa3", "sae-secret", 3000);
    fake_net_t wpa3 = fake_net_copy("Wpa3");
    CHECK(r == WIFI_CONNECT_OK, "wpa3 connect: %d", r);
    CHECK(strcmp(wpa3.key_mgmt, "SAE") == 0 && wpa3.ieee80211w == 2 && wpa3.sae_password[0] && !wpa3.psk[0],
          "wpa3 config: %s pmf=%d", wpa3.key_mgmt, wpa3.ieee80211w);
    /* Older supplicant: SAE capable but no sae_password field -> quoted psk. */
    fake.sae_field = false;
    r = wifi_wpa_connect("Wpa3", "sae-secret", 3000);
    wpa3 = fake_net_copy("Wpa3");
    CHECK(r == WIFI_CONNECT_OK && strcmp(wpa3.psk, "\"sae-secret\"") == 0, "wpa3 psk fallback: %d %s", r, wpa3.psk);
    fake.sae_field = true;

    /* Wrong password for an existing entry: the saved file is reloaded. */
    fake_reset_log();
    r = wifi_wpa_connect("Home", "nothome!", 3000);
    CHECK(r == WIFI_CONNECT_WRONG_KEY, "existing wrong key: %d", r);
    CHECK(fake_log_has("RECONFIGURE"), "updated entry restored");
    home_now = fake_net_copy("Home");
    CHECK(strcmp(home_now.psk, hex) == 0, "old key back");

    /* Open network, escaped SSID. */
    r = wifi_wpa_connect("Caf\xc3\xa9", NULL, 3000);
    fake_net_t open = fake_net_copy("Caf\xc3\xa9");
    CHECK(r == WIFI_CONNECT_OK && strcmp(open.key_mgmt, "NONE") == 0, "open network: %d", r);

    /* A raw 64-hex-digit key goes to the supplicant as the PSK itself. */
    char raw_psk[65];
    psk_for("coffee123", "Cafe", raw_psk);
    fake.sae_capable = false;
    r = wifi_wpa_connect("Cafe", raw_psk, 3000);
    cafe = fake_net_copy("Cafe");
    CHECK(r == WIFI_CONNECT_OK && strcmp(cafe.psk, raw_psk) == 0 && strcmp(cafe.key_mgmt, "WPA-PSK") == 0,
          "raw psk: %d %s", r, cafe.key_mgmt);
    fake.sae_capable = true;
    /* Passphrases outside 8..63 never reach the supplicant. */
    fake_reset_log();
    CHECK(wifi_wpa_connect("Cafe", "short", 3000) == WIFI_CONNECT_ERROR, "7-character passphrase refused");
    char too_long[65];
    memset(too_long, 'g', 64);
    too_long[64] = '\0';
    CHECK(wifi_wpa_connect("Cafe", too_long, 3000) == WIFI_CONNECT_ERROR, "64 non-hex characters refused");
    CHECK(!fake_log_has("SET_NETWORK") && !fake_log_has("ADD_NETWORK"), "nothing sent for a refused key");

    /* Reconnect to a saved network by id. */
    fake_reset_log();
    home_now = fake_net_copy("Home");
    usleep(300000); /* let the DHCP helper finish the earlier connects */
    int dhcp_before = atomic_load(&dhcp_runs);
    r = wifi_wpa_connect_saved(home_now.id, 3000);
    CHECK(r == WIFI_CONNECT_OK, "connect saved: %d", r);
    CHECK(!fake_log_has("SELECT_NETWORK") && fake_net_copy("Home").priority == 2, "connect saved by priority");
    dhcp_target = dhcp_before + 1;
    CHECK(wait_for(dhcp_reached, 2000), "dhcp for the new network");
    /* A roam within the same SSID keeps the lease. */
    pthread_mutex_lock(&fake.mu);
    send_event("<3>CTRL-EVENT-CONNECTED - Connection to 00:11:22:33:44:55 completed [id=%d id_str=]", home_now.id);
    pthread_mutex_unlock(&fake.mu);
    usleep(300000);
    CHECK(atomic_load(&dhcp_runs) == dhcp_before + 1, "no DHCP on a roam: %d -> %d", dhcp_before, atomic_load(&dhcp_runs));

    /* Not in range: the supplicant stays on/returns to another network. */
    pthread_mutex_lock(&fake.mu);
    fake_net_t * far = add_net("FarAway", 0, false);
    int far_id = far->id;
    pthread_mutex_unlock(&fake.mu);
    r = wifi_wpa_connect_saved(far_id, 3000);
    CHECK(r == WIFI_CONNECT_NOT_FOUND, "not in range: %d", r);

    /* No verdict in time. */
    r = wifi_wpa_connect("Slow", "slowslow", 400);
    CHECK(r == WIFI_CONNECT_TIMEOUT, "timeout: %d", r);

    /* Forget and disconnect. */
    fake_net_t slow = fake_net_copy("Slow");
    CHECK(slow.id < 0, "failed attempt left no entry");
    CHECK(wifi_wpa_forget(fake_net_copy("Mixed").id) && fake_count_ssid("Mixed") == 0, "forget");
    CHECK(wifi_wpa_disconnect(), "disconnect");
    usleep(200000);
    wifi_core_get_state(&s);
    CHECK(!s.connected && s.enabled, "disconnected but on");

    /* No scan event: scan_wait gives up at its timeout. */
    fake.no_scan_event = true;
    CHECK(wifi_core_scan_start() && !wifi_core_scan_wait(200), "scan wait times out");
    fake.no_scan_event = false;

    /* A mute supplicant: requests time out instead of hanging. */
    fake.mute = true;
    char reply[64];
    CHECK(wifi_core_request("PING", reply, sizeof(reply)) == WPA_CTRL_TIMEOUT, "request timeout");
    fake.mute = false;

    /* Killed with SIGKILL: the socket file stays, PING fails -> off. */
    fake_kill(true, false);
    CHECK(wifi_core_wait_enabled(false, 3000), "stale socket reads as off");
    CHECK(!wifi_core_is_enabled(), "is_enabled false");
    CHECK(wifi_wpa_connect("Home", "homepass1", 500) == WIFI_CONNECT_ERROR, "connect while off");
    wpa_ctrl_t * c = wpa_ctrl_open(fake.path, tmp);
    CHECK(c == NULL, "open refused on a stale socket");
    wpa_ctrl_close(c);

    /* Back on, then a clean shutdown with CTRL-EVENT-TERMINATING. */
    fake_start();
    CHECK(wifi_core_wait_enabled(true, 3000), "back on");
    fake_kill(false, true);
    CHECK(wifi_core_wait_enabled(false, 1000), "terminating event -> off");

    wifi_core_stop();
}

int main(void) {
    char tmp[] = "/tmp/mt-wifi-test-XXXXXX";
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 2;
    }
    test_parsers();
    test_against_fake(tmp);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("wifi: all checks passed\n");
    return 0;
}
