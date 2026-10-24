#define _GNU_SOURCE
#include "radio_list.h"
#include "storage_paths.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Large enough for any hand-made list, small enough to bound memory. */
#define RADIO_MAX_ENTRIES 2000
#define RADIO_FILE_MAX_BYTES (1024 * 1024)

static pthread_mutex_t radio_mutex = PTHREAD_MUTEX_INITIALIZER;

void radio_entries_free(radio_entries_t * list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].url);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

static bool entries_push(radio_entries_t * list, const char * name, const char * url, bool base, bool in_sd,
                         bool hidden) {
    if (list->count >= RADIO_MAX_ENTRIES) return false;
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 32;
        radio_station_t * grown = realloc(list->items, (size_t) cap * sizeof(*grown));
        if (!grown) return false;
        list->items = grown;
        list->cap = cap;
    }
    radio_station_t * s = &list->items[list->count];
    s->name = strdup(name);
    s->url = strdup(url);
    if (!s->name || !s->url) {
        free(s->name);
        free(s->url);
        return false;
    }
    s->base = base;
    s->in_sd = in_sd;
    s->hidden = hidden;
    list->count++;
    return true;
}

static char * trim(char * s) {
    while (isspace((unsigned char) *s)) s++;
    char * end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) *--end = '\0';
    return s;
}

bool radio_url_is_http(const char * s) {
    return s && (strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0);
}

void radio_url_normalize(const char * url, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!url) return;
    while (isspace((unsigned char) *url)) url++;
    size_t len = strlen(url);
    while (len > 0 && isspace((unsigned char) url[len - 1])) len--;
    const char * sep = strstr(url, "://");
    size_t o = 0;
    if (!sep || (size_t) (sep - url) > len) {
        snprintf(out, out_size, "%.*s", (int) len, url);
        return;
    }
    size_t scheme_len = (size_t) (sep - url);
    char scheme[16] = "";
    for (size_t i = 0; i < scheme_len && i + 1 < sizeof(scheme); i++) scheme[i] = (char) tolower((unsigned char) url[i]);
    scheme[scheme_len < sizeof(scheme) ? scheme_len : sizeof(scheme) - 1] = '\0';
    const char * host = sep + 3;
    const char * end = url + len;
    const char * path = host;
    while (path < end && *path != '/' && *path != '?' && *path != '#') path++;
    /* host[:port] */
    size_t host_len = (size_t) (path - host);
    const char * colon = memchr(host, ':', host_len);
    size_t name_len = colon ? (size_t) (colon - host) : host_len;
    const char * port = colon ? colon + 1 : NULL;
    size_t port_len = colon ? host_len - name_len - 1 : 0;
    bool default_port = port && ((strcmp(scheme, "http") == 0 && port_len == 2 && memcmp(port, "80", 2) == 0) ||
                                 (strcmp(scheme, "https") == 0 && port_len == 3 && memcmp(port, "443", 3) == 0));
    o += (size_t) snprintf(out + o, out_size - o, "%s://", scheme);
    for (size_t i = 0; i < name_len && o + 1 < out_size; i++) out[o++] = (char) tolower((unsigned char) host[i]);
    if (port && !default_port && port_len > 0 && o + 1 < out_size) {
        out[o++] = ':';
        for (size_t i = 0; i < port_len && o + 1 < out_size; i++) out[o++] = port[i];
    }
    size_t rest_len = (size_t) (end - path);
    while (rest_len > 0 && path[rest_len - 1] == '/') rest_len--;
    for (size_t i = 0; i < rest_len && o + 1 < out_size; i++) out[o++] = path[i];
    out[o < out_size ? o : out_size - 1] = '\0';
}

/* Parses one line. Returns false for blanks, comments and invalid lines. */
static bool parse_line(char * line, char ** name_out, char ** url_out) {
    char * text = trim(line);
    if (!text[0] || text[0] == '#') return false;
    char * name = text, * url = NULL;
    char * bar = strchr(text, '|');
    if (bar) {
        *bar = '\0';
        name = trim(text);
        url = trim(bar + 1);
    } else if (radio_url_is_http(text)) {
        url = text;
    }
    if (!url || !radio_url_is_http(url) || strlen(url) >= RADIO_URL_MAX) return false;
    if (!name[0] || name == url) name = url;
    *name_out = name;
    *url_out = url;
    return true;
}

bool radio_entries_parse_file(const char * path, radio_entries_t * out) {
    memset(out, 0, sizeof(*out));
    FILE * f = path ? fopen(path, "r") : NULL;
    if (!f) return false;
    char line[RADIO_URL_MAX + RADIO_NAME_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            /* Over-long line: skip the rest of it. */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        char * name, * url;
        if (!parse_line(line, &name, &url)) continue;
        if (!entries_push(out, name, url, false, false, false)) break;
    }
    fclose(f);
    return true;
}

/* Normalized URL set, as a sorted array for bsearch. */
typedef struct {
    char ** urls;
    int count;
} url_set_t;

static int cmp_str_ptr(const void * a, const void * b) {
    return strcmp(*(char * const *) a, *(char * const *) b);
}

static void url_set_free(url_set_t * set) {
    for (int i = 0; i < set->count; i++) free(set->urls[i]);
    free(set->urls);
    set->urls = NULL;
    set->count = 0;
}

static bool url_set_from_entries(const radio_entries_t * list, url_set_t * set) {
    set->count = 0;
    set->urls = calloc((size_t) (list->count ? list->count : 1), sizeof(char *));
    if (!set->urls) return false;
    for (int i = 0; i < list->count; i++) {
        char norm[RADIO_URL_MAX];
        radio_url_normalize(list->items[i].url, norm, sizeof(norm));
        set->urls[set->count] = strdup(norm);
        if (!set->urls[set->count]) { url_set_free(set); return false; }
        set->count++;
    }
    qsort(set->urls, (size_t) set->count, sizeof(char *), cmp_str_ptr);
    return true;
}

static bool url_set_has(const url_set_t * set, const char * url) {
    if (!set || set->count == 0) return false;
    char norm[RADIO_URL_MAX];
    radio_url_normalize(url, norm, sizeof(norm));
    const char * key = norm;
    return bsearch(&key, set->urls, (size_t) set->count, sizeof(char *), cmp_str_ptr) != NULL;
}

/* The hidden list: one URL per line. */
static bool parse_hidden(const char * path, url_set_t * set) {
    memset(set, 0, sizeof(*set));
    radio_entries_t raw = { 0 };
    FILE * f = path ? fopen(path, "r") : NULL;
    if (f) {
        char line[RADIO_URL_MAX + 8];
        while (fgets(line, sizeof(line), f)) {
            char * text = trim(line);
            if (!text[0] || text[0] == '#' || !radio_url_is_http(text)) continue;
            if (!entries_push(&raw, text, text, false, false, false)) break;
        }
        fclose(f);
    }
    bool ok = url_set_from_entries(&raw, set);
    radio_entries_free(&raw);
    return ok;
}

static int find_norm(const radio_entries_t * list, const char * url) {
    char a[RADIO_URL_MAX], b[RADIO_URL_MAX];
    radio_url_normalize(url, a, sizeof(a));
    for (int i = 0; i < list->count; i++) {
        radio_url_normalize(list->items[i].url, b, sizeof(b));
        if (strcmp(a, b) == 0) return i;
    }
    return -1;
}

bool radio_list_build(const char * base_path, const char * sd_path, const char * hidden_path,
                      bool include_hidden, radio_entries_t * out) {
    memset(out, 0, sizeof(*out));
    radio_entries_t base = { 0 }, sd = { 0 };
    url_set_t hidden = { 0 };
    (void) radio_entries_parse_file(base_path, &base);
    bool have_sd = sd_path && radio_entries_parse_file(sd_path, &sd);
    if (!parse_hidden(hidden_path, &hidden)) {
        radio_entries_free(&base);
        radio_entries_free(&sd);
        return false;
    }
    bool ok = true;
    for (int i = 0; ok && i < base.count; i++) {
        if (find_norm(out, base.items[i].url) >= 0) continue; /* duplicate in the base list */
        bool is_hidden = url_set_has(&hidden, base.items[i].url);
        if (is_hidden && !include_hidden) continue;
        int rename = have_sd ? find_norm(&sd, base.items[i].url) : -1;
        const char * name = rename >= 0 ? sd.items[rename].name : base.items[i].name;
        ok = entries_push(out, name, base.items[i].url, true, rename >= 0, is_hidden);
    }
    for (int i = 0; ok && i < sd.count; i++) {
        if (find_norm(&base, sd.items[i].url) >= 0) continue; /* a rename, handled above */
        if (find_norm(out, sd.items[i].url) >= 0) continue;
        ok = entries_push(out, sd.items[i].name, sd.items[i].url, false, true, false);
    }
    radio_entries_free(&base);
    radio_entries_free(&sd);
    url_set_free(&hidden);
    if (!ok) radio_entries_free(out);
    return ok;
}

/* ---- atomic writes ------------------------------------------------------ */

static bool fsync_parent(const char * path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (!slash) return true;
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool write_atomic(const char * path, const char * data, size_t len) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    (void) fsync_parent(path);
    return true;
}

typedef struct {
    char * data;
    size_t len, cap;
    bool failed;
} strbuf_t;

static void sb_add(strbuf_t * sb, const char * s) {
    if (sb->failed) return;
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 1024;
        while (cap < sb->len + n + 1) cap *= 2;
        char * grown = realloc(sb->data, cap);
        if (!grown) { sb->failed = true; return; }
        sb->data = grown;
        sb->cap = cap;
    }
    memcpy(sb->data + sb->len, s, n + 1);
    sb->len += n;
}

static void sb_add_station(strbuf_t * sb, const char * name, const char * url) {
    char clean[RADIO_NAME_MAX];
    size_t o = 0;
    for (const char * p = name; *p && o + 1 < sizeof(clean); p++)
        clean[o++] = (*p == '|') ? '/' : (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
    clean[o] = '\0';
    sb_add(sb, clean);
    sb_add(sb, " | ");
    sb_add(sb, url);
    sb_add(sb, "\n");
}

static const char SD_HEADER[] =
    "# Your radio stations, added to the built-in list.\n"
    "# One \"Station Name | http://stream-url\" per line. A built-in station\n"
    "# listed here with another name is shown under that name.\n";

/* ---- the player's list ---------------------------------------------------- */

static bool card_readable(void) {
    return sd_card_state_readable(mt_state_card());
}

bool radio_list_editable(void) {
    return mt_state_available();
}

bool radio_list_load(radio_entries_t * out, bool include_hidden) {
    pthread_mutex_lock(&radio_mutex);
    bool card = card_readable();
    bool ok = radio_list_build(MT_RADIO_BASE_FILE, card ? MT_RADIO_SD_FILE : NULL,
                               card ? MT_RADIO_HIDDEN_FILE : NULL, include_hidden, out);
    pthread_mutex_unlock(&radio_mutex);
    return ok;
}

int radio_list_hidden_count(void) {
    radio_entries_t all = { 0 };
    if (!radio_list_load(&all, true)) return 0;
    int n = 0;
    for (int i = 0; i < all.count; i++) if (all.items[i].hidden) n++;
    radio_entries_free(&all);
    return n;
}

/* Reads a whole small file; NULL with errno ENOENT when it does not exist. */
static char * read_small_file(const char * path, size_t * len_out) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * buf = malloc(RADIO_FILE_MAX_BYTES + 1);
    size_t n = buf ? fread(buf, 1, RADIO_FILE_MAX_BYTES + 1, f) : 0;
    fclose(f);
    if (!buf || n > RADIO_FILE_MAX_BYTES) { free(buf); errno = EFBIG; return NULL; }
    buf[n] = '\0';
    *len_out = n;
    return buf;
}

/* Rewrites a list file without the lines whose station URL matches. */
static radio_result_t drop_url_from_file(const char * path, const char * url, bool hidden_format, bool * removed) {
    *removed = false;
    size_t len = 0;
    char * text = read_small_file(path, &len);
    if (!text) return errno == ENOENT ? RADIO_OK : RADIO_IO_ERROR;
    char target[RADIO_URL_MAX];
    radio_url_normalize(url, target, sizeof(target));
    strbuf_t out = { 0 };
    sb_add(&out, "");
    char * line = text;
    while (line && *line) {
        char * next = strchr(line, '\n');
        if (next) *next = '\0';
        char copy[RADIO_URL_MAX + RADIO_NAME_MAX + 64];
        snprintf(copy, sizeof(copy), "%s", line);
        bool match = false;
        if (hidden_format) {
            char * t = trim(copy);
            if (t[0] && t[0] != '#') {
                char norm[RADIO_URL_MAX];
                radio_url_normalize(t, norm, sizeof(norm));
                match = strcmp(norm, target) == 0;
            }
        } else {
            char * name, * u;
            if (parse_line(copy, &name, &u)) {
                char norm[RADIO_URL_MAX];
                radio_url_normalize(u, norm, sizeof(norm));
                match = strcmp(norm, target) == 0;
            }
        }
        if (match) *removed = true;
        else {
            sb_add(&out, line);
            sb_add(&out, "\n");
        }
        line = next ? next + 1 : NULL;
    }
    radio_result_t result = RADIO_OK;
    if (out.failed) result = RADIO_IO_ERROR;
    else if (*removed) {
        bool ok = hidden_format ? mt_state_write_file(path, out.data, out.len)
                                : write_atomic(path, out.data, out.len);
        if (!ok) result = RADIO_IO_ERROR;
    }
    free(out.data);
    free(text);
    return result;
}

static radio_result_t append_line(const char * path, const char * line, bool use_state_write) {
    size_t len = 0;
    char * text = read_small_file(path, &len);
    if (!text && errno != ENOENT) return RADIO_IO_ERROR;
    strbuf_t out = { 0 };
    sb_add(&out, "");
    if (text) {
        sb_add(&out, text);
        if (len > 0 && text[len - 1] != '\n') sb_add(&out, "\n");
    } else if (!use_state_write) {
        sb_add(&out, SD_HEADER);
    }
    sb_add(&out, line);
    free(text);
    bool ok = !out.failed && (use_state_write ? mt_state_write_file(path, out.data, out.len)
                                              : write_atomic(path, out.data, out.len));
    free(out.data);
    return ok ? RADIO_OK : RADIO_IO_ERROR;
}

radio_result_t radio_list_add(const char * name, const char * url) {
    if (!url) return RADIO_BAD_URL;
    char url_clean[RADIO_URL_MAX];
    snprintf(url_clean, sizeof(url_clean), "%s", url);
    char * u = trim(url_clean);
    if (!radio_url_is_http(u) || strchr(u, '|') || strpbrk(u, " \t\r\n")) return RADIO_BAD_URL;
    if (!radio_list_editable()) return RADIO_NO_CARD;

    radio_entries_t all = { 0 };
    if (!radio_list_load(&all, true)) return RADIO_IO_ERROR;
    int found = find_norm(&all, u);
    bool was_hidden = found >= 0 && all.items[found].hidden;
    radio_entries_free(&all);
    if (found >= 0 && !was_hidden) return RADIO_EXISTS;
    if (was_hidden) {
        radio_result_t r = radio_list_restore(u);
        return r == RADIO_OK ? RADIO_ADDED_RESTORED : r;
    }

    strbuf_t line = { 0 };
    sb_add_station(&line, (name && name[0]) ? name : u, u);
    if (line.failed) return RADIO_IO_ERROR;
    pthread_mutex_lock(&radio_mutex);
    radio_result_t r = append_line(MT_RADIO_SD_FILE, line.data, false);
    pthread_mutex_unlock(&radio_mutex);
    free(line.data);
    return r;
}

radio_result_t radio_list_remove(const char * url) {
    if (!url || !radio_url_is_http(url)) return RADIO_BAD_URL;
    if (!radio_list_editable()) return RADIO_NO_CARD;
    radio_entries_t all = { 0 };
    if (!radio_list_load(&all, true)) return RADIO_IO_ERROR;
    int found = find_norm(&all, url);
    bool is_base = found >= 0 && all.items[found].base;
    bool hidden = found >= 0 && all.items[found].hidden;
    char base_url[RADIO_URL_MAX];
    snprintf(base_url, sizeof(base_url), "%s", found >= 0 ? all.items[found].url : url);
    radio_entries_free(&all);
    if (found < 0 || hidden) return RADIO_NOT_FOUND;

    pthread_mutex_lock(&radio_mutex);
    bool removed = false;
    radio_result_t r = drop_url_from_file(MT_RADIO_SD_FILE, url, false, &removed);
    if (r == RADIO_OK && is_base) {
        strbuf_t line = { 0 };
        sb_add(&line, base_url);
        sb_add(&line, "\n");
        r = line.failed ? RADIO_IO_ERROR
                        : (mt_config_writable() ? append_line(MT_RADIO_HIDDEN_FILE, line.data, true) : RADIO_NO_CARD);
        free(line.data);
    }
    pthread_mutex_unlock(&radio_mutex);
    return r;
}

radio_result_t radio_list_restore(const char * url) {
    if (!url) return RADIO_BAD_URL;
    if (!mt_config_writable()) return RADIO_NO_CARD;
    pthread_mutex_lock(&radio_mutex);
    bool removed = false;
    radio_result_t r = drop_url_from_file(MT_RADIO_HIDDEN_FILE, url, true, &removed);
    pthread_mutex_unlock(&radio_mutex);
    if (r != RADIO_OK) return r;
    return removed ? RADIO_OK : RADIO_NOT_FOUND;
}

const char * radio_result_text(radio_result_t result) {
    switch (result) {
    case RADIO_OK: return "ok";
    case RADIO_ADDED_RESTORED: return "restored a removed station";
    case RADIO_EXISTS: return "already in the Radio list";
    case RADIO_NOT_FOUND: return "not in the Radio list";
    case RADIO_NO_CARD: return "no writable SD card";
    case RADIO_BAD_URL: return "not an http(s) stream URL";
    default: return "could not write the Radio list";
    }
}
