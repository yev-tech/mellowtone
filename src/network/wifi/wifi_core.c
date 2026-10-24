/* Wi-Fi core: control-socket worker, cached state, events.
 *
 * Threads:
 *   worker      owns the ATTACHed event socket; probes for the supplicant
 *               while it is off; PING liveness, signal refresh (screen on);
 *               every state change goes through set_state().
 *   helper      one at a time, runs config.on_connected (DHCP) after
 *               CTRL-EVENT-CONNECTED so events keep flowing meanwhile.
 *   callers     worker threads of the UI (connect, scan, snapshot) use the
 *               shared request socket under req_mu.
 * The UI thread only reads the cache (wifi_core_get_state() and friends). */
#include "wifi_core.h"
#include "wpa_ctrl.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CTRL_DIR "/var/run/wpa_supplicant"
#define DEFAULT_IFACE "wlan0"
#define DEFAULT_ROUTE_FILE "/proc/net/route"
#define DEFAULT_SIGNAL_MS 10000
#define DEFAULT_PROBE_MS 1000
#define DEFAULT_REQUEST_MS 2000
#define PING_SCREEN_ON_MS 10000
#define PING_SCREEN_OFF_MS 60000
/* After CONNECTED the address arrives from DHCP with no supplicant event:
 * STATUS is re-read every 2 s for up to 30 s until ip_address shows up. */
#define IP_FOLLOWUP_MS 2000
#define IP_FOLLOWUP_WINDOW_MS 30000
#define NOT_FOUND_LIMIT 3

typedef enum { EV_CONNECTED, EV_DISCONNECTED, EV_WRONG_KEY, EV_TEMP_DISABLED, EV_NOT_FOUND, EV_SCAN, EV_OFF } ev_type_t;

typedef struct {
    unsigned seq;
    ev_type_t type;
    int id;
} ev_rec_t;

#define EV_RING 32

static struct {
    wifi_core_config_t cfg;
    char ctrl_path[108];
    char iface[16];
    pthread_t thread;
    bool started;
    atomic_bool stop;
    int wake[2];

    pthread_mutex_t mu; /* state, ring, generation */
    pthread_cond_t cond;
    wifi_state_t state;
    unsigned generation;
    unsigned seq;
    ev_rec_t ring[EV_RING];
    unsigned scan_seq;
    atomic_uint pending;

    pthread_mutex_t req_mu;
    wpa_ctrl_t * req;

    wpa_ctrl_t * mon; /* worker only */
    atomic_bool helper_busy;
    atomic_bool helper_pending;
    atomic_uint barrier_req;
    unsigned barrier_done; /* under mu */
    long long connected_at_ms;
} core = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .req_mu = PTHREAD_MUTEX_INITIALIZER,
    .wake = { -1, -1 },
};

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int request_timeout(void) {
    return core.cfg.request_timeout_ms > 0 ? core.cfg.request_timeout_ms : DEFAULT_REQUEST_MS;
}

static bool screen_on(void) {
    return !core.cfg.screen_on || core.cfg.screen_on();
}

static void kick(void) {
    if (core.wake[1] >= 0) {
        char c = 1;
        ssize_t r = write(core.wake[1], &c, 1);
        (void) r;
    }
}

/* ---- pure helpers ------------------------------------------------------- */

int wifi_rssi_to_level(int rssi_dbm) {
    /* Conventional RSSI buckets for the 4 signal icon levels. */
    if (rssi_dbm >= -50) return 3;
    if (rssi_dbm >= -60) return 2;
    if (rssi_dbm >= -70) return 1;
    return 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Inverse of wpa_supplicant's printf_encode() (wpa_ssid_txt()). */
void wifi_ssid_decode(const char * in, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t o = 0;
    for (const char * p = in ? in : ""; *p && o + 1 < out_size; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'e': c = '\033'; break;
            case 'x':
                if (hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
                    c = (char) (hex_val(p[1]) * 16 + hex_val(p[2]));
                    p += 2;
                } else {
                    c = 'x';
                }
                break;
            default: c = *p; break; /* \\ and \" */
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

bool wifi_kv_get(const char * text, const char * key, char * out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    if (!text || !key) return false;
    size_t klen = strlen(key);
    for (const char * line = text; line && *line;) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            const char * v = line + klen + 1;
            size_t len = strcspn(v, "\n");
            if (out && out_size) {
                if (len >= out_size) len = out_size - 1;
                memcpy(out, v, len);
                out[len] = '\0';
            }
            return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

/* Splits one line on tabs in place (empty fields kept, unlike strtok). */
static int split_tabs(char * line, char ** fields, int max_fields) {
    int n = 0;
    while (n < max_fields) {
        fields[n++] = line;
        char * tab = strchr(line, '\t');
        if (!tab) break;
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

static bool flags_secured(const char * flags) {
    return strstr(flags, "WPA") || strstr(flags, "RSN") || strstr(flags, "WEP") || strstr(flags, "SAE");
}

int wifi_parse_scan_results(char * text, const char * current_ssid, wifi_network_t * out, int max_count) {
    int count = 0;
    char * save = NULL;
    for (char * line = strtok_r(text, "\n", &save); line && count < max_count; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "bssid /", 7) == 0) continue; /* header */
        char * f[5];
        if (split_tabs(line, f, 5) < 5) continue;
        char ssid[WIFI_MAX_SSID_LEN];
        wifi_ssid_decode(f[4], ssid, sizeof(ssid));
        if (!ssid[0]) continue; /* hidden */
        int level = wifi_rssi_to_level(atoi(f[2]));
        int existing = -1;
        for (int i = 0; i < count; i++)
            if (strcmp(out[i].ssid, ssid) == 0) {
                existing = i;
                break;
            }
        if (existing >= 0) {
            /* Another BSSID of the same network: keep the strongest. */
            if (level > out[existing].signal_level) out[existing].signal_level = level;
            continue;
        }
        snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid);
        out[count].signal_level = level;
        out[count].secured = flags_secured(f[3]);
        out[count].is_current = current_ssid && current_ssid[0] && strcmp(current_ssid, ssid) == 0;
        count++;
    }
    return count;
}

bool wifi_scan_flags_for(const char * text, const char * ssid, char * flags, size_t flags_size) {
    if (flags && flags_size) flags[0] = '\0';
    if (!text || !ssid) return false;
    char * copy = strdup(text);
    if (!copy) return false;
    bool found = false;
    int best = -1000;
    char * save = NULL;
    for (char * line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "bssid /", 7) == 0) continue;
        char * f[5];
        if (split_tabs(line, f, 5) < 5) continue;
        char name[WIFI_MAX_SSID_LEN];
        wifi_ssid_decode(f[4], name, sizeof(name));
        int rssi = atoi(f[2]);
        if (strcmp(name, ssid) != 0 || rssi < best) continue;
        best = rssi;
        found = true;
        if (flags && flags_size) snprintf(flags, flags_size, "%s", f[3]);
    }
    free(copy);
    return found;
}

int wifi_parse_list_networks(char * text, wifi_saved_network_t * out, bool * disabled, int max_count) {
    int count = 0;
    char * save = NULL;
    for (char * line = strtok_r(text, "\n", &save); line && count < max_count; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "network id", 10) == 0) continue; /* header */
        char * f[4];
        int n = split_tabs(line, f, 4);
        if (n < 2 || f[0][0] < '0' || f[0][0] > '9') continue;
        out[count].id = atoi(f[0]);
        wifi_ssid_decode(f[1], out[count].ssid, sizeof(out[count].ssid));
        if (disabled) disabled[count] = n >= 4 && strstr(f[3], "[DISABLED]") != NULL;
        count++;
    }
    return count;
}

bool wifi_route_gateway(const char * route_file, const char * iface, char * out, size_t out_size) {
    out[0] = '\0';
    FILE * f = fopen(route_file, "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char name[32];
        unsigned long dest, gw;
        if (sscanf(line, "%31s %lx %lx", name, &dest, &gw) != 3) continue; /* header too */
        if (strcmp(name, iface) != 0 || dest != 0 || gw == 0) continue;
        snprintf(out, out_size, "%lu.%lu.%lu.%lu", gw & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff,
                 (gw >> 24) & 0xff);
        found = true;
    }
    fclose(f);
    return found;
}

int wifi_read_dns(const char * resolv_path, char out[][16], int max_count) {
    FILE * f = fopen(resolv_path, "r");
    if (!f) return 0;
    int count = 0;
    char line[128];
    while (count < max_count && fgets(line, sizeof(line), f)) {
        char ip[16];
        if (sscanf(line, "nameserver %15s", ip) == 1) snprintf(out[count++], 16, "%s", ip);
    }
    fclose(f);
    return count;
}

bool wifi_password_is_raw_psk(const char * password) {
    if (!password || strlen(password) != 64) return false;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char) password[i])) return false;
    return true;
}

bool wifi_password_is_valid(const char * password) {
    size_t len = password ? strlen(password) : 0;
    return (len >= 8 && len <= 63) || wifi_password_is_raw_psk(password);
}

bool wifi_dns_server_is_valid(const char * server) {
    struct in_addr addr;
    return server && inet_pton(AF_INET, server, &addr) == 1;
}

/* ---- requests ----------------------------------------------------------- */

int wifi_core_request(const char * cmd, char * reply, size_t reply_size) {
    pthread_mutex_lock(&core.req_mu);
    if (!core.req) core.req = wpa_ctrl_open(core.ctrl_path, core.cfg.client_dir);
    int n = WPA_CTRL_ERR;
    if (core.req) {
        n = wpa_ctrl_request(core.req, cmd, reply, reply_size, request_timeout());
        if (n == WPA_CTRL_ERR) {
            wpa_ctrl_close(core.req);
            core.req = NULL;
        }
    }
    pthread_mutex_unlock(&core.req_mu);
    if (n < 0 && reply && reply_size) reply[0] = '\0';
    return n;
}

bool wifi_core_request_ok(const char * cmd) {
    char reply[64];
    return wifi_core_request(cmd, reply, sizeof(reply)) >= 2 && strncmp(reply, "OK", 2) == 0;
}

bool wifi_core_cmd_ok(const char * fmt, ...) {
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= sizeof(cmd)) return false;
    return wifi_core_request_ok(cmd);
}

static void close_request_socket(void) {
    pthread_mutex_lock(&core.req_mu);
    wpa_ctrl_close(core.req);
    core.req = NULL;
    pthread_mutex_unlock(&core.req_mu);
}

/* ---- state -------------------------------------------------------------- */

static void post(unsigned bits) {
    atomic_fetch_or(&core.pending, bits);
}

/* Caller holds core.mu. */
static void record_event(ev_type_t type, int id);

/* An event for connection waiters, plus UI bits, posted under the lock
 * so that a woken waiter already sees them. */
static void record_post(ev_type_t type, int id, unsigned bits) {
    pthread_mutex_lock(&core.mu);
    post(bits);
    record_event(type, id);
    pthread_mutex_unlock(&core.mu);
}

static void record_event(ev_type_t type, int id) {
    unsigned seq = ++core.seq;
    core.ring[seq % EV_RING] = (ev_rec_t) { .seq = seq, .type = type, .id = id };
    pthread_cond_broadcast(&core.cond);
}

static void set_state(const wifi_state_t * next) {
    pthread_mutex_lock(&core.mu);
    bool changed = memcmp(&core.state, next, sizeof(*next)) != 0;
    bool enabled_changed = core.state.enabled != next->enabled;
    if (changed) {
        core.state = *next;
        core.generation++;
        /* Posted before waiters wake: they may look at the events next. */
        post(WIFI_EVT_STATE | (enabled_changed ? WIFI_EVT_ENABLED_CHANGED : 0));
        if (enabled_changed && !next->enabled) record_event(EV_OFF, -1);
        pthread_cond_broadcast(&core.cond);
    }
    pthread_mutex_unlock(&core.mu);
}

static void get_state_locked_copy(wifi_state_t * out) {
    pthread_mutex_lock(&core.mu);
    *out = core.state;
    pthread_mutex_unlock(&core.mu);
}

static void set_disabled(void) {
    wifi_state_t s;
    memset(&s, 0, sizeof(s));
    s.network_id = -1;
    set_state(&s);
}

/* STATUS (+ SIGNAL_POLL when connected) into the cache. false when the
 * supplicant did not answer. */
static bool refresh_status(bool with_signal) {
    char buf[1024];
    if (wifi_core_request("STATUS", buf, sizeof(buf)) < 0) return false;

    wifi_state_t prev, s;
    get_state_locked_copy(&prev);
    memset(&s, 0, sizeof(s));
    s.enabled = true;
    s.network_id = -1;
    wifi_kv_get(buf, "wpa_state", s.wpa_state, sizeof(s.wpa_state));
    s.connected = strcmp(s.wpa_state, "COMPLETED") == 0;
    char mac[18];
    if (wifi_kv_get(buf, "address", mac, sizeof(mac))) snprintf(s.mac, sizeof(s.mac), "%s", mac);
    if (s.connected) {
        char raw[4 * WIFI_MAX_SSID_LEN + 1], id[16];
        if (wifi_kv_get(buf, "ssid", raw, sizeof(raw))) wifi_ssid_decode(raw, s.ssid, sizeof(s.ssid));
        if (wifi_kv_get(buf, "id", id, sizeof(id))) s.network_id = atoi(id);
        wifi_kv_get(buf, "ip_address", s.ip, sizeof(s.ip));
        wifi_route_gateway(core.cfg.route_file ? core.cfg.route_file : DEFAULT_ROUTE_FILE, core.iface, s.gateway,
                           sizeof(s.gateway));
        /* Keep the last reading between signal refreshes. */
        s.rssi = prev.connected ? prev.rssi : 0;
        s.signal_level = prev.connected ? prev.signal_level : 0;
        if (with_signal || !prev.connected) {
            char sig[256], rssi[16];
            if (wifi_core_request("SIGNAL_POLL", sig, sizeof(sig)) > 0 && wifi_kv_get(sig, "RSSI", rssi, sizeof(rssi))) {
                s.rssi = atoi(rssi);
                s.signal_level = wifi_rssi_to_level(s.rssi);
            }
        }
    }
    set_state(&s);
    return true;
}

void wifi_core_refresh(void) {
    if (!refresh_status(true)) kick();
}

/* ---- events ------------------------------------------------------------- */

static int parse_id(const char * msg, const char * tag) {
    const char * p = strstr(msg, tag);
    return p ? atoi(p + strlen(tag)) : -1;
}

/* One helper at a time; a CONNECTED that arrives while it runs (a fallback
 * after a failed attempt, a quick roam) makes it run once more. */
static void * helper_thread(void * arg) {
    (void) arg;
    while (atomic_exchange(&core.helper_pending, false)) {
        wifi_state_t s;
        get_state_locked_copy(&s);
        if (s.connected) core.cfg.on_connected(core.iface, s.ssid, s.ip);
        kick(); /* the lease is in: re-read STATUS */
    }
    atomic_store(&core.helper_busy, false);
    /* A request that raced with the exit above. */
    if (atomic_load(&core.helper_pending) && !atomic_exchange(&core.helper_busy, true)) return helper_thread(NULL);
    return NULL;
}

static void start_helper(void) {
    if (!core.cfg.on_connected) return;
    atomic_store(&core.helper_pending, true);
    if (atomic_exchange(&core.helper_busy, true)) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, helper_thread, NULL) != 0) atomic_store(&core.helper_busy, false);
    pthread_attr_destroy(&attr);
}

static void drop_monitor(void) {
    wpa_ctrl_close(core.mon);
    core.mon = NULL;
    close_request_socket();
    set_disabled();
}

static void handle_event(const char * raw) {
    const char * msg = raw;
    if (msg[0] == '<') {
        const char * gt = strchr(msg, '>');
        if (gt) msg = gt + 1;
    }
    if (strncmp(msg, "CTRL-EVENT-CONNECTED", 20) == 0) {
        int id = parse_id(msg, "[id=");
        refresh_status(true);
        core.connected_at_ms = now_ms();
        wifi_state_t now;
        get_state_locked_copy(&now);
        record_post(EV_CONNECTED, id >= 0 ? id : now.network_id, WIFI_EVT_CONNECTED);
        start_helper();
    } else if (strncmp(msg, "CTRL-EVENT-DISCONNECTED", 23) == 0) {
        refresh_status(false);
        record_post(EV_DISCONNECTED, -1, WIFI_EVT_DISCONNECTED);
    } else if (strncmp(msg, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) == 0) {
        bool wrong_key = strstr(msg, "reason=WRONG_KEY") != NULL;
        record_post(wrong_key ? EV_WRONG_KEY : EV_TEMP_DISABLED, parse_id(msg, " id="),
                    wrong_key ? WIFI_EVT_WRONG_KEY : 0);
    } else if (strstr(msg, "pre-shared key may be incorrect")) {
        /* Older supplicants: "WPA: 4-Way Handshake failed - pre-shared key
         * may be incorrect", without a network id. */
        record_post(EV_WRONG_KEY, -1, WIFI_EVT_WRONG_KEY);
    } else if (strncmp(msg, "CTRL-EVENT-NETWORK-NOT-FOUND", 28) == 0) {
        record_post(EV_NOT_FOUND, -1, 0);
    } else if (strncmp(msg, "CTRL-EVENT-SCAN-RESULTS", 23) == 0) {
        record_post(EV_SCAN, -1, WIFI_EVT_SCAN_DONE);
    } else if (strncmp(msg, "CTRL-EVENT-TERMINATING", 22) == 0) {
        drop_monitor();
    }
}

/* ---- worker ------------------------------------------------------------- */

static void drain_wake(void) {
    char buf[32];
    while (read(core.wake[0], buf, sizeof(buf)) > 0) {}
}

static bool try_attach(void) {
    wpa_ctrl_t * mon = wpa_ctrl_open(core.ctrl_path, core.cfg.client_dir);
    if (!mon) return false;
    if (!wpa_ctrl_attach(mon, request_timeout())) {
        wpa_ctrl_close(mon);
        return false;
    }
    core.mon = mon;
    if (!refresh_status(true)) {
        drop_monitor();
        return false;
    }
    return true;
}

static void publish_barrier(unsigned barrier) {
    pthread_mutex_lock(&core.mu);
    if (core.barrier_done != barrier) {
        core.barrier_done = barrier;
        pthread_cond_broadcast(&core.cond);
    }
    pthread_mutex_unlock(&core.mu);
}

static void * worker(void * arg) {
    (void) arg;
    long long next_ping = 0, next_signal = 0, next_ip = 0;
    while (!atomic_load(&core.stop)) {
        if (!core.mon) {
            if (try_attach()) {
                long long t = now_ms();
                next_ping = t + PING_SCREEN_ON_MS;
                next_signal = t + (core.cfg.signal_interval_ms > 0 ? core.cfg.signal_interval_ms : DEFAULT_SIGNAL_MS);
                continue;
            }
            set_disabled();
            int probe = core.cfg.probe_interval_ms > 0 ? core.cfg.probe_interval_ms : DEFAULT_PROBE_MS;
            if (!screen_on()) probe *= 5;
            struct pollfd pfd = { .fd = core.wake[0], .events = POLLIN };
            if (poll(&pfd, 1, probe) > 0) drain_wake();
            publish_barrier(atomic_load(&core.barrier_req));
            continue;
        }

        long long t = now_ms();
        wifi_state_t s;
        get_state_locked_copy(&s);
        bool ip_followup = s.connected && !s.ip[0] && t - core.connected_at_ms < IP_FOLLOWUP_WINDOW_MS;
        long long next = next_ping;
        if (s.connected && screen_on() && next_signal < next) next = next_signal;
        if (ip_followup && next_ip < next) next = next_ip;
        int timeout = next > t ? (int) (next - t) : 0;

        struct pollfd pfd[2] = {
            { .fd = wpa_ctrl_fd(core.mon), .events = POLLIN },
            { .fd = core.wake[0], .events = POLLIN },
        };
        int r = poll(pfd, 2, timeout);
        if (r < 0 && errno != EINTR) break;
        bool woken = r > 0 && (pfd[1].revents & POLLIN);
        if (woken) drain_wake();
        if (r > 0 && (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            drop_monitor();
            continue;
        }
        /* Barrier (wifi_core_event_seq()): read the request first, then
         * drain every queued event, then publish. */
        unsigned barrier = atomic_load(&core.barrier_req);
        char ev[512];
        int n = 0;
        while (core.mon && (n = wpa_ctrl_recv_event(core.mon, ev, sizeof(ev), 0)) > 0) handle_event(ev);
        if (core.mon && n < 0) drop_monitor();
        publish_barrier(barrier);
        if (!core.mon) continue;

        t = now_ms();
        if (woken || t >= next_ping) {
            /* Liveness: a supplicant killed with SIGKILL sends no TERMINATING. */
            char pong[16];
            int n = wifi_core_request("PING", pong, sizeof(pong));
            /* A slow reply (timeout) is not "off"; a refused send is. */
            if (n == WPA_CTRL_ERR || (n >= 0 && strncmp(pong, "PONG", 4) != 0)) {
                drop_monitor();
                continue;
            }
            next_ping = t + (screen_on() ? PING_SCREEN_ON_MS : PING_SCREEN_OFF_MS);
            if (woken) refresh_status(false);
        }
        get_state_locked_copy(&s);
        if (s.connected && screen_on() && t >= next_signal) {
            refresh_status(true);
            next_signal = t + (core.cfg.signal_interval_ms > 0 ? core.cfg.signal_interval_ms : DEFAULT_SIGNAL_MS);
        } else if (s.connected && !s.ip[0] && t - core.connected_at_ms < IP_FOLLOWUP_WINDOW_MS && t >= next_ip) {
            refresh_status(false);
            next_ip = t + IP_FOLLOWUP_MS;
        }
    }
    if (core.mon) {
        char reply[16];
        wpa_ctrl_request(core.mon, "DETACH", reply, sizeof(reply), 500);
        wpa_ctrl_close(core.mon);
        core.mon = NULL;
    }
    return NULL;
}

bool wifi_core_start(const wifi_core_config_t * config) {
    if (core.started) return true;
    core.cfg = *config;
    snprintf(core.iface, sizeof(core.iface), "%s", config->iface ? config->iface : DEFAULT_IFACE);
    snprintf(core.ctrl_path, sizeof(core.ctrl_path), "%s/%s", config->ctrl_dir ? config->ctrl_dir : DEFAULT_CTRL_DIR,
             core.iface);
    memset(&core.state, 0, sizeof(core.state));
    core.state.network_id = -1;
    atomic_store(&core.stop, false);
    atomic_store(&core.pending, 0);
    if (pipe(core.wake) != 0) return false;
    for (int i = 0; i < 2; i++) {
        fcntl(core.wake[i], F_SETFL, fcntl(core.wake[i], F_GETFL) | O_NONBLOCK);
        fcntl(core.wake[i], F_SETFD, FD_CLOEXEC);
    }
    if (pthread_create(&core.thread, NULL, worker, NULL) != 0) {
        close(core.wake[0]);
        close(core.wake[1]);
        core.wake[0] = core.wake[1] = -1;
        return false;
    }
    core.started = true;
    return true;
}

void wifi_core_stop(void) {
    if (!core.started) return;
    atomic_store(&core.stop, true);
    kick();
    pthread_join(core.thread, NULL);
    while (atomic_load(&core.helper_busy)) usleep(10000);
    close_request_socket();
    close(core.wake[0]);
    close(core.wake[1]);
    core.wake[0] = core.wake[1] = -1;
    core.started = false;
    pthread_mutex_lock(&core.mu);
    memset(&core.state, 0, sizeof(core.state));
    core.state.network_id = -1;
    pthread_mutex_unlock(&core.mu);
}

/* ---- cached reads ------------------------------------------------------- */

void wifi_core_get_state(wifi_state_t * out) {
    get_state_locked_copy(out);
}

bool wifi_core_is_enabled(void) {
    pthread_mutex_lock(&core.mu);
    bool on = core.state.enabled;
    pthread_mutex_unlock(&core.mu);
    return on;
}

unsigned wifi_core_generation(void) {
    pthread_mutex_lock(&core.mu);
    unsigned g = core.generation;
    pthread_mutex_unlock(&core.mu);
    return g;
}

unsigned wifi_core_take_events(void) {
    return atomic_exchange(&core.pending, 0);
}

static void deadline_after(struct timespec * ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

bool wifi_core_wait_enabled(bool want, int timeout_ms) {
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        kick();
        pthread_mutex_lock(&core.mu);
        bool on = core.state.enabled;
        if (on != want) {
            struct timespec ts;
            deadline_after(&ts, 200);
            pthread_cond_timedwait(&core.cond, &core.mu, &ts);
            on = core.state.enabled;
        }
        pthread_mutex_unlock(&core.mu);
        if (on == want) return true;
        if (now_ms() >= deadline) return false;
    }
}

unsigned wifi_core_event_seq(void) {
    /* Events the supplicant sent before now may still sit unread in the
     * event socket (a fallback CONNECTED after an earlier failed attempt):
     * let the worker take them in first, so they count as "before". */
    unsigned want = atomic_fetch_add(&core.barrier_req, 1) + 1;
    kick();
    struct timespec ts;
    deadline_after(&ts, 1000);
    pthread_mutex_lock(&core.mu);
    while (core.started && (int) (core.barrier_done - want) < 0)
        if (pthread_cond_timedwait(&core.cond, &core.mu, &ts) == ETIMEDOUT) break;
    unsigned seq = core.seq;
    pthread_mutex_unlock(&core.mu);
    return seq;
}

/* ---- scan, lists, info -------------------------------------------------- */

bool wifi_core_scan_start(void) {
    pthread_mutex_lock(&core.mu);
    core.scan_seq = core.seq;
    pthread_mutex_unlock(&core.mu);
    char reply[32];
    if (wifi_core_request("SCAN", reply, sizeof(reply)) < 0) return false;
    /* FAIL-BUSY: a scan is already running; its results will do. */
    return strncmp(reply, "OK", 2) == 0 || strncmp(reply, "FAIL-BUSY", 9) == 0;
}

bool wifi_core_scan_wait(int timeout_ms) {
    struct timespec ts;
    deadline_after(&ts, timeout_ms);
    pthread_mutex_lock(&core.mu);
    bool done = false;
    for (;;) {
        for (unsigned s = core.scan_seq + 1; s <= core.seq && !done; s++) {
            const ev_rec_t * e = &core.ring[s % EV_RING];
            if (e->seq == s && (e->type == EV_SCAN || e->type == EV_OFF)) done = true;
        }
        if (done || !core.state.enabled) break;
        if (pthread_cond_timedwait(&core.cond, &core.mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&core.mu);
    return done;
}

int wifi_core_scan_results(wifi_network_t * out, int max_count) {
    char * buf = malloc(16384);
    if (!buf) return 0;
    int count = 0;
    if (wifi_core_request("SCAN_RESULTS", buf, 16384) > 0) {
        wifi_state_t s;
        get_state_locked_copy(&s);
        count = wifi_parse_scan_results(buf, s.connected ? s.ssid : "", out, max_count);
    }
    free(buf);
    return count;
}

int wifi_core_list_saved(wifi_saved_network_t * out, int max_count) {
    char buf[8192];
    if (wifi_core_request("LIST_NETWORKS", buf, sizeof(buf)) <= 0) return 0;
    return wifi_parse_list_networks(buf, out, NULL, max_count);
}

bool wifi_core_get_info(wifi_info_t * out) {
    memset(out, 0, sizeof(*out));
    refresh_status(true);
    wifi_state_t s;
    get_state_locked_copy(&s);
    if (!s.connected) return false;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s.ssid);
    snprintf(out->ip, sizeof(out->ip), "%s", s.ip);
    snprintf(out->gateway, sizeof(out->gateway), "%s", s.gateway);
    snprintf(out->mac, sizeof(out->mac), "%s", s.mac);
    out->signal_level = s.signal_level;
    return true;
}

/* ---- connection outcome ------------------------------------------------- */

wifi_connect_result_t wifi_core_wait_connect(int id, unsigned since_seq, int timeout_ms) {
    struct timespec ts;
    deadline_after(&ts, timeout_ms);
    int not_found = 0;
    wifi_connect_result_t result = WIFI_CONNECT_TIMEOUT;
    bool decided = false;
    pthread_mutex_lock(&core.mu);
    unsigned seen = since_seq;
    while (!decided) {
        /* Events older than the ring are lost; start from what is left. */
        if (core.seq - seen > EV_RING) seen = core.seq - EV_RING;
        for (; seen < core.seq && !decided; seen++) {
            const ev_rec_t * e = &core.ring[(seen + 1) % EV_RING];
            if (e->seq != seen + 1) continue;
            switch (e->type) {
            case EV_CONNECTED:
                /* Connected elsewhere: the requested network was not found
                 * in the scan (it is the preferred one when in range). */
                result = e->id == id ? WIFI_CONNECT_OK : WIFI_CONNECT_NOT_FOUND;
                decided = true;
                break;
            case EV_WRONG_KEY:
                if (e->id == id || e->id < 0) {
                    result = WIFI_CONNECT_WRONG_KEY;
                    decided = true;
                }
                break;
            case EV_TEMP_DISABLED:
                if (e->id == id) {
                    result = WIFI_CONNECT_FAILED;
                    decided = true;
                }
                break;
            case EV_NOT_FOUND:
                if (++not_found >= NOT_FOUND_LIMIT) {
                    result = WIFI_CONNECT_NOT_FOUND;
                    decided = true;
                }
                break;
            case EV_OFF:
                result = WIFI_CONNECT_ERROR;
                decided = true;
                break;
            default: break;
            }
        }
        if (decided) break;
        if (!core.state.enabled) {
            result = WIFI_CONNECT_ERROR;
            break;
        }
        if (pthread_cond_timedwait(&core.cond, &core.mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&core.mu);
    return result;
}
