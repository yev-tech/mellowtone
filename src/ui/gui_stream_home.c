/* Streaming home.
 *
 * The Stream shortcut opens the active Subsonic server's home: its name and
 * a live status line (tap the name to switch servers), search, a "Recently
 * added" shelf with covers, Albums / Artists / Playlists / Random mix, and a
 * Continue card for the album last streamed. Albums open as a cover grid
 * with a sort chip. Song lists, artists and playlists reuse gui_subsonic.c's
 * screens and playback.
 *
 * Settings > Streaming owns setup: servers (by name), add / edit / remove
 * with a connection test, stream quality and downloads.
 *
 * Network work runs on one job thread plus a cover thread; a 150 ms timer
 * applies results. Covers are fetched at the size shown, converted to LVGL
 * .bin thumbnails and cached on the SD card (never internal flash). */
#include "board_config.h"
#include "storage_paths.h"


#include "gui_stream_home.h"
#include "gui.h"
#include "gui_subsonic.h"
#include "gui_navigation.h"
#include "gui_text_input.h"
#include "screen_builders.h"
#include "settings.h"
#include "metadata_db.h"
#include "subsonic_client.h"
#include "dlna_client.h"
#include "http_client.h"
#include "image_thumb.h"
#include "wifi_status.h"
#include "utf8_util.h"
#include "i18n.h"
#include "assets.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern lv_color_t accent_lv_color(void);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_muted;

#define COVER_DIR MT_STREAM_COVERS_DIR
#define NAMES_FILE MT_STREAM_NAMES_FILE
#define SHELF_COVER BOARD_UI_PX(96)
#define GRID_COVER BOARD_UI_PX(160)
#define ROW_COVER BOARD_UI_PX(44)
#define PAGE_SIZE 30

#define C_TEXT lv_color_hex(GUI_COLOR_TEXT)
#define C_MUTED lv_color_hex(GUI_COLOR_TEXT_MUTED)
#define C_CARD lv_color_hex(GUI_COLOR_CARD)
#define C_PANEL lv_color_hex(GUI_COLOR_SURFACE)

/* ---- server names ---------------------------------------------------------
 * Saved servers are keyed by URL (subsonic_saved_servers.c); the display
 * name lives in a small url<TAB>name file next to it. */

static void host_of(const char * url, char * out, size_t size) {
    const char * p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/:");
    snprintf(out, size, "%.*s", (int) n, p);
}

static void server_name(const char * url, char * out, size_t size) {
    out[0] = '\0';
    FILE * f = fopen(NAMES_FILE, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char * tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            if (strcmp(line, url) == 0) { snprintf(out, size, "%s", tab + 1); break; }
        }
        fclose(f);
    }
    if (!out[0]) host_of(url, out, size);
}

/* name NULL removes the entry. */
static void set_server_name(const char * url, const char * name) {
    char lines[64][384];
    int n = 0;
    FILE * f = fopen(NAMES_FILE, "r");
    if (f) {
        char line[384];
        while (n < 64 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char * tab = strchr(line, '\t');
            if (!tab || ((size_t) (tab - line) == strlen(url) && strncmp(line, url, strlen(url)) == 0)) continue;
            snprintf(lines[n++], sizeof(lines[0]), "%s", line);
        }
        fclose(f);
    }
    if (name && name[0] && n < 64) snprintf(lines[n++], sizeof(lines[0]), "%s\t%s", url, name);
    if (!mt_state_prepare_write(NAMES_FILE)) return;
    f = fopen(NAMES_FILE ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", lines[i]);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(NAMES_FILE ".tmp", NAMES_FILE);
}

/* ---- DLNA servers ------------------------------------------------------------
 * Saved as location<TAB>udn<TAB>name<TAB>model. Which source Stream opens is
 * ACTIVE_FILE: "dlna<TAB>location", or no file for the active Subsonic server. */

#define DLNA_FILE MT_DLNA_SERVERS_FILE
#define ACTIVE_FILE MT_STREAM_ACTIVE_FILE
#define DLNA_CONTINUE_FILE MT_DLNA_CONTINUE_FILE
#define DL_MAX_SAVED 16

typedef struct {
    char location[512], udn[96], name[96], model[96];
} dl_saved_t;

static void write_file_atomic(const char * path, const char * data) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!mt_state_prepare_write(path)) return;
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    fputs(data, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

/* Splits a TSV line in place; returns the field count. */
static int tsv_split(char * line, char ** fields, int max) {
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    while (n < max) {
        fields[n++] = line;
        char * tab = strchr(line, '\t');
        if (!tab) break;
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

static int dl_load(dl_saved_t * out) {
    int n = 0;
    FILE * f = fopen(DLNA_FILE, "r");
    if (!f) return 0;
    char line[900];
    while (n < DL_MAX_SAVED && fgets(line, sizeof(line), f)) {
        char * fl[4] = { 0 };
        int k = tsv_split(line, fl, 4);
        if (k < 1 || strncmp(fl[0], "http", 4) != 0) continue;
        memset(&out[n], 0, sizeof(out[n]));
        /* A cut-off location is a different URL: skip the entry instead. */
        if ((size_t) snprintf(out[n].location, sizeof(out[n].location), "%s", fl[0]) >= sizeof(out[n].location))
            continue;
        if (k > 1) snprintf(out[n].udn, sizeof(out[n].udn), "%s", fl[1]);
        if (k > 2) snprintf(out[n].name, sizeof(out[n].name), "%s", fl[2]);
        if (k > 3) snprintf(out[n].model, sizeof(out[n].model), "%s", fl[3]);
        if (!out[n].name[0]) host_of(out[n].location, out[n].name, sizeof(out[n].name));
        n++;
    }
    fclose(f);
    return n;
}

static void dl_store(const dl_saved_t * list, int n) {
    char * buf = malloc((size_t) DL_MAX_SAVED * sizeof(dl_saved_t) + 64);
    if (!buf) return;
    size_t k = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++)
        k += (size_t) sprintf(buf + k, "%s\t%s\t%s\t%s\n", list[i].location, list[i].udn, list[i].name, list[i].model);
    write_file_atomic(DLNA_FILE, buf);
    free(buf);
}

/* Replaces the entry with old_location (or the same device), else appends. */
static void dl_upsert(const dl_saved_t * s, const char * old_location) {
    dl_saved_t list[DL_MAX_SAVED];
    int n = dl_load(list), at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if ((old_location && !strcmp(list[i].location, old_location)) || !strcmp(list[i].location, s->location) ||
            (s->udn[0] && !strcmp(list[i].udn, s->udn)))
            at = i;
    if (at < 0) {
        if (n == DL_MAX_SAVED) return;
        at = n++;
    }
    list[at] = *s;
    dl_store(list, n);
}

static void dl_remove(const char * location) {
    dl_saved_t list[DL_MAX_SAVED];
    int n = dl_load(list), k = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].location, location) != 0) list[k++] = list[i];
    dl_store(list, k);
}

/* The active DLNA server's location, or false when Stream opens Subsonic. */
static bool dl_active_location(char * out, size_t size) {
    out[0] = '\0';
    FILE * f = fopen(ACTIVE_FILE, "r");
    if (!f) return false;
    char line[600];
    bool ok = false;
    if (fgets(line, sizeof(line), f)) {
        char * fl[2];
        if (tsv_split(line, fl, 2) == 2 && !strcmp(fl[0], "dlna") && fl[1][0]) {
            snprintf(out, size, "%s", fl[1]);
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

static void dl_set_active(const char * location) {
    if (!location) { unlink(ACTIVE_FILE); return; }
    char buf[600];
    snprintf(buf, sizeof(buf), "dlna\t%s\n", location);
    write_file_atomic(ACTIVE_FILE, buf);
}

static bool dl_find(const char * location, dl_saved_t * out) {
    dl_saved_t list[DL_MAX_SAVED];
    int n = dl_load(list);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].location, location)) { *out = list[i]; return true; }
    return false;
}

/* The active DLNA server when it is still saved (else Subsonic is used). */
static bool dl_active(dl_saved_t * out) {
    char loc[600];
    if (!dl_active_location(loc, sizeof(loc))) return false;
    if (dl_find(loc, out)) return true;
    dl_set_active(NULL);
    return false;
}

/* Continue for DLNA: location, folder id, album filter, title, artist, art, index. */
typedef struct {
    char location[512], id[128], filter[128], title[128], artist[128], art[512];
    int index;
} dl_continue_t;

static bool dl_continue_load(dl_continue_t * c) {
    FILE * f = fopen(DLNA_CONTINUE_FILE, "r");
    if (!f) return false;
    char line[1800];
    bool ok = false;
    if (fgets(line, sizeof(line), f)) {
        char * fl[7];
        /* A cut-off location is a different URL: no Continue entry then. */
        if (tsv_split(line, fl, 7) == 7 &&
            (size_t) snprintf(c->location, sizeof(c->location), "%s", fl[0]) < sizeof(c->location)) {
            snprintf(c->id, sizeof(c->id), "%s", fl[1]);
            snprintf(c->filter, sizeof(c->filter), "%s", fl[2]);
            snprintf(c->title, sizeof(c->title), "%s", fl[3]);
            snprintf(c->artist, sizeof(c->artist), "%s", fl[4]);
            snprintf(c->art, sizeof(c->art), "%s", fl[5]);
            c->index = atoi(fl[6]);
            ok = c->id[0] != '\0';
        }
    }
    fclose(f);
    return ok;
}

static void dl_continue_save(const dl_continue_t * c) {
    char buf[1800];
    snprintf(buf, sizeof(buf), "%s\t%s\t%s\t%s\t%s\t%s\t%d\n", c->location, c->id, c->filter, c->title, c->artist,
             c->art, c->index);
    for (char * p = buf; *p && p[1]; p++) if (*p == '\n') *p = ' '; /* fields never hold newlines */
    write_file_atomic(DLNA_CONTINUE_FILE, buf);
}

/* ---- covers --------------------------------------------------------------- */

typedef enum { OWNER_HOME = 1, OWNER_GRID, OWNER_SEARCH } cover_owner_t;

typedef struct {
    lv_obj_t * img;
    char path[200];
    int owner;
} cover_slot_t;

typedef struct {
    char id[64];   /* Subsonic cover id, or */
    char url[600]; /* a full image URL (DLNA album art) */
    char path[200];
    int size;
} cover_req_t;

#define COVER_SLOTS 96
#define COVER_QUEUE 48

static cover_slot_t cover_slots[COVER_SLOTS];
static cover_req_t cover_queue[COVER_QUEUE];
static int cover_queue_n;
static char cover_done[COVER_QUEUE][200];
static int cover_done_n;
static pthread_mutex_t cover_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cover_cv = PTHREAD_COND_INITIALIZER;
static bool cover_thread_started;
static subsonic_server_t cover_server;

static unsigned hash32(const char * s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char) *s++) * 16777619u;
    return h;
}

static bool cover_path(const char * id, int size, char * out, size_t out_size) {
    char safe[64];
    size_t n = 0;
    for (const char * p = id; *p && n + 1 < sizeof(safe); p++)
        safe[n++] = (isalnum((unsigned char) *p) || *p == '-' || *p == '_') ? *p : '_';
    safe[n] = '\0';
    if (!n) return false;
    snprintf(out, out_size, COVER_DIR "/%08x_%s_%d.bin", hash32(current_settings.subsonic_url), safe, size);
    return true;
}

static void * cover_thread(void * arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&cover_mu);
        while (cover_queue_n == 0) pthread_cond_wait(&cover_cv, &cover_mu);
        cover_req_t req = cover_queue[0];
        memmove(&cover_queue[0], &cover_queue[1], sizeof(cover_queue[0]) * (size_t) (--cover_queue_n));
        subsonic_server_t server = cover_server;
        pthread_mutex_unlock(&cover_mu);

        if (access(req.path, F_OK) != 0) {
            char url[1600];
            if (req.url[0]) snprintf(url, sizeof(url), "%s", req.url);
            else subsonic_build_cover_art_url_sized(&server, req.id, req.size, url, sizeof(url));
            int status = 0;
            uint8_t * body = NULL;
            size_t len = 0;
            if (http_get_to_buffer_limited(url, server.verify_tls, 2u * 1024u * 1024u, &status, &body, &len) &&
                status == 200 && len > 0) {
                if (mt_state_mkdirs(COVER_DIR))
                    image_thumb_write_bin(body, len, req.size, req.size, req.path, ARTWORK_PRIO_THUMBNAIL, NULL, NULL, NULL);
            }
            free(body);
        }
        pthread_mutex_lock(&cover_mu);
        if (access(req.path, F_OK) == 0 && cover_done_n < COVER_QUEUE)
            snprintf(cover_done[cover_done_n++], sizeof(cover_done[0]), "%s", req.path);
        pthread_mutex_unlock(&cover_mu);
    }
    return NULL;
}

static void covers_forget(int owner) {
    for (int i = 0; i < COVER_SLOTS; i++)
        if (cover_slots[i].owner == owner) cover_slots[i].owner = 0;
}

/* A cover image (rounded placeholder until the art arrives): a Subsonic
 * cover id, or with by_url a full image URL. */
static lv_obj_t * cover_image_ex(lv_obj_t * parent, const char * cover_id, bool by_url, int size, int owner) {
    lv_obj_t * img = lv_image_create(parent);
    lv_obj_set_size(img, size, size);
    lv_obj_set_style_radius(img, size > BOARD_UI_PX(60) ? BOARD_UI_PX(10) : BOARD_UI_PX(6), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_set_style_bg_color(img, lv_color_hex(GUI_COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    char path[200];
    if (!cover_id || !cover_id[0]) return img;
    if (by_url) {
        char key[20];
        snprintf(key, sizeof(key), "u%08x%08x", hash32(cover_id), hash32(cover_id + strlen(cover_id) / 2));
        snprintf(path, sizeof(path), COVER_DIR "/%s_%d.bin", key, size);
    } else if (!cover_path(cover_id, size, path, sizeof(path))) {
        return img;
    }
    char src[210];
    if (access(path, F_OK) == 0) {
        snprintf(src, sizeof(src), "S:%s", path);
        lv_image_set_src(img, src);
        return img;
    }
    for (int i = 0; i < COVER_SLOTS; i++) {
        if (cover_slots[i].owner) continue;
        cover_slots[i].img = img;
        cover_slots[i].owner = owner;
        snprintf(cover_slots[i].path, sizeof(cover_slots[i].path), "%s", path);
        break;
    }
    pthread_mutex_lock(&cover_mu);
    if (!cover_thread_started) {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 192 * 1024);
        cover_thread_started = pthread_create(&th, &attr, cover_thread, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    cover_server = gui_subsonic_active_server();
    bool queued = false;
    for (int i = 0; i < cover_queue_n && !queued; i++) queued = strcmp(cover_queue[i].path, path) == 0;
    if (!queued && cover_queue_n < COVER_QUEUE) {
        cover_req_t * r = &cover_queue[cover_queue_n++];
        r->id[0] = r->url[0] = '\0';
        if (by_url) snprintf(r->url, sizeof(r->url), "%s", cover_id);
        else snprintf(r->id, sizeof(r->id), "%s", cover_id);
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->size = size;
        pthread_cond_signal(&cover_cv);
    }
    pthread_mutex_unlock(&cover_mu);
    return img;
}

static lv_obj_t * cover_image(lv_obj_t * parent, const char * cover_id, int size, int owner) {
    return cover_image_ex(parent, cover_id, false, size, owner);
}

static void covers_apply(void) {
    char done[COVER_QUEUE][200];
    pthread_mutex_lock(&cover_mu);
    int n = cover_done_n;
    memcpy(done, cover_done, sizeof(done[0]) * (size_t) n);
    cover_done_n = 0;
    pthread_mutex_unlock(&cover_mu);
    for (int d = 0; d < n; d++)
        for (int i = 0; i < COVER_SLOTS; i++) {
            if (!cover_slots[i].owner || strcmp(cover_slots[i].path, done[d]) != 0) continue;
            char src[210];
            snprintf(src, sizeof(src), "S:%.200s", done[d]);
            lv_image_set_src(cover_slots[i].img, src);
            cover_slots[i].owner = 0;
        }
}

/* ---- jobs ------------------------------------------------------------------ */

typedef enum {
    JOB_NONE, JOB_HOME, JOB_ALBUMS, JOB_RANDOM, JOB_SEARCH, JOB_TEST,
    JOB_DL_HOME, JOB_DL_BROWSE, JOB_DL_DISCOVER, JOB_DL_DESCRIBE
} job_t;

static struct {
    subsonic_server_t server;
    char type[24];
    char query[128];
    int offset;
    bool ok;
    char error[160];
    subsonic_album_t * albums;
    int album_count;
    subsonic_song_t * songs;
    int song_count;
    /* DLNA */
    dlna_server_t dl;      /* in: location (+udn); out: described */
    char dl_id[128];
    int dl_start, dl_count;
    unsigned dl_gen;       /* the folder screen a browse is for */
    int dl_purpose;        /* JOB_DL_DESCRIBE: 0 add from an address, 1 test in the form */
    bool dl_moved;         /* found again by udn at a new address */
    dlna_object_t * objs, * top, * recent;
    int obj_n, obj_total, top_n, recent_n;
    char recent_id[128];
    dlna_server_t * found;
    int found_n;
} J;

static atomic_int job_running;
static atomic_int job_done;
static http_cancel_token_t job_cancel;

static void copy_error(void) {
    snprintf(J.error, sizeof(J.error), "%s", subsonic_last_error());
}

static bool contains_ci(const char * s, const char * needle) {
    size_t n = strlen(needle);
    for (; *s; s++)
        if (!strncasecmp(s, needle, n)) return true;
    return false;
}

static bool name_is(const char * s, const char * const * names) {
    for (; *names; names++)
        if (!strcasecmp(s, *names)) return true;
    return false;
}

static void dl_job_home(void) {
    dlna_server_t d;
    J.ok = dlna_describe(J.dl.location, &d, &job_cancel);
    if (!J.ok && J.dl.udn[0] && !http_cancel_token_is_cancelled(&job_cancel)) {
        /* The NAS may have a new address: look for the same device. */
        dlna_server_t * f = NULL;
        int n = dlna_discover(&f, 2500, &job_cancel);
        for (int i = 0; i < n && !J.ok; i++)
            if (!strcmp(f[i].udn, J.dl.udn)) { d = f[i]; J.ok = J.dl_moved = true; }
        free(f);
        if (!J.ok) { snprintf(J.error, sizeof(J.error), "%s", "Server not found on this network"); return; }
    }
    if (!J.ok) return;
    J.dl = d;
    dlna_object_t * root = NULL;
    int rn = 0, total = 0;
    J.ok = dlna_browse(&J.dl, "0", 0, 60, &root, &rn, &total, &job_cancel);
    if (!J.ok) return;
    /* Most servers split the root into Music / Video / Pictures: start in Music. */
    static const char * const music[] = { "Music", "Musik", "Musique", "Música", "Musica", "Muziek", "Audio", "Musiikki",
                                          "Muzyka", "Музыка", NULL };
    for (int i = 0; i < rn; i++) {
        if (!root[i].is_container || !name_is(root[i].title, music)) continue;
        dlna_object_t * m = NULL;
        int mn = 0;
        if (dlna_browse(&J.dl, root[i].id, 0, 60, &m, &mn, &total, &job_cancel) && mn > 0) {
            free(root);
            root = m;
            rn = mn;
        } else {
            free(m);
        }
        break;
    }
    J.top = root;
    J.top_n = rn;
    for (int i = 0; i < rn; i++) {
        if (!root[i].is_container) continue;
        if (!contains_ci(root[i].title, "recent") && !contains_ci(root[i].title, "latest") &&
            !contains_ci(root[i].title, "newest") && !contains_ci(root[i].title, "zuletzt"))
            continue;
        snprintf(J.recent_id, sizeof(J.recent_id), "%s", root[i].id);
        dlna_browse(&J.dl, root[i].id, 0, 80, &J.recent, &J.recent_n, &total, &job_cancel);
        break;
    }
}

static void * job_thread(void * arg) {
    job_t kind = (job_t) (intptr_t) arg;
    J.ok = false;
    switch (kind) {
        case JOB_DL_HOME: dl_job_home(); break;
        case JOB_DL_BROWSE:
            J.ok = dlna_browse(&J.dl, J.dl_id, J.dl_start, J.dl_count, &J.objs, &J.obj_n, &J.obj_total, &job_cancel);
            break;
        case JOB_DL_DISCOVER:
            J.found_n = dlna_discover(&J.found, 3000, &job_cancel);
            J.ok = J.found_n > 0;
            break;
        case JOB_DL_DESCRIBE: {
            char loc[512];
            snprintf(loc, sizeof(loc), "%s", J.dl.location);
            J.ok = dlna_describe(loc, &J.dl, &job_cancel);
            break;
        }
        case JOB_HOME:
            J.ok = subsonic_ping(&J.server, &job_cancel);
            if (J.ok) subsonic_get_album_list(&J.server, "newest", 12, 0, &J.albums, &J.album_count, &job_cancel);
            break;
        case JOB_ALBUMS:
            J.ok = subsonic_get_album_list(&J.server, J.type, PAGE_SIZE, J.offset, &J.albums, &J.album_count, &job_cancel);
            break;
        case JOB_RANDOM:
            J.ok = subsonic_get_random_songs(&J.server, 50, &J.songs, &J.song_count, &job_cancel);
            break;
        case JOB_SEARCH:
            J.ok = subsonic_search(&J.server, J.query, 20, 30, &J.albums, &J.album_count, &J.songs, &J.song_count,
                                   &job_cancel);
            break;
        case JOB_TEST:
            J.ok = subsonic_ping(&J.server, &job_cancel);
            if (J.ok) subsonic_get_album_list(&J.server, "newest", 1, 0, &J.albums, &J.album_count, &job_cancel);
            break;
        default: break;
    }
    if (!J.ok && !J.error[0]) {
        if (kind >= JOB_DL_HOME) snprintf(J.error, sizeof(J.error), "%s", dlna_last_error());
        else copy_error();
    }
    /* job_running stays set until poll_cb() has applied the results, so a new
     * job can't free them first. */
    atomic_store(&job_done, kind);
    return NULL;
}

static void job_free_results(void) {
    free(J.albums);
    free(J.songs);
    J.albums = NULL;
    J.songs = NULL;
    J.album_count = J.song_count = 0;
    free(J.objs);
    free(J.top);
    free(J.recent);
    free(J.found);
    J.objs = J.top = J.recent = NULL;
    J.found = NULL;
    J.obj_n = J.obj_total = J.top_n = J.recent_n = J.found_n = 0;
    J.recent_id[0] = '\0';
    J.dl_moved = false;
}

static bool start_job(job_t kind) {
    if (atomic_load(&job_running) != JOB_NONE) {
        show_info_toast(TR("Previous request still finishing"));
        return false;
    }
    job_free_results();
    J.error[0] = '\0';
    http_cancel_token_init(&job_cancel);
    atomic_store(&job_running, kind);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    bool ok = pthread_create(&th, &attr, job_thread, (void *) (intptr_t) kind) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) atomic_store(&job_running, JOB_NONE);
    return ok;
}

/* ---- small widgets --------------------------------------------------------- */

static lv_obj_t * text(lv_obj_t * parent, const char * s, gui_font_role_t role, lv_color_t color, int32_t width) {
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, gui_theme_font(role), 0);
    lv_obj_set_style_text_color(l, color, 0);
    if (width > 0) {
        lv_obj_set_width(l, width);
        lv_obj_set_height(l, lv_font_get_line_height(gui_theme_font(role)));
        label_set_text_single_line(l, s);
    } else {
        lv_label_set_text(l, s);
    }
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t * box(lv_obj_t * parent, int32_t w, int32_t h, lv_color_t bg, int32_t radius) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t * section(lv_obj_t * parent, const char * title) {
    lv_obj_t * l = text(parent, title, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_set_style_pad_top(l, BOARD_UI_PX(6), 0);
    lv_obj_set_style_pad_left(l, BOARD_UI_PX(4), 0);
    return l;
}

static lv_obj_t * new_screen(const char * title, lv_obj_t ** out_title, lv_obj_t ** out_content) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * t = build_screen_header(scr, title, generic_back_cb, NULL, NULL);
    if (out_title) *out_title = t;
    lv_obj_t * c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(c, lv_pct(100), BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(c, 0, top);
    lv_obj_set_style_pad_hor(c, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_bottom(c, BOARD_UI_PX(16), 0);
    lv_obj_set_style_pad_row(c, BOARD_UI_PX(8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);
    *out_content = c;
    finalize_screen_navigation(scr);
    return scr;
}

static void loading(lv_obj_t * parent, const char * msg) {
    lv_obj_t * l = text(parent, msg, GUI_FONT_ROLE_BODY, C_MUTED, 0);
    lv_obj_set_style_pad_top(l, BOARD_UI_PX(40), 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

/* "Try again" row under a load failure (DLNA folders, Subsonic albums and search). */
static void try_again_row(lv_obj_t * parent, lv_event_cb_t cb) {
    lv_obj_t * retry = box(parent, lv_pct(100), BOARD_UI_PX(44), C_CARD, BOARD_UI_PX(22));
    lv_obj_add_flag(retry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(retry, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(text(retry, TR("Try again"), GUI_FONT_ROLE_BODY, C_TEXT, 0));
}

static bool wifi_ok(void) {
    int level;
    return wifi_get_status(&level);
}

/* Rebuilding a list from a tap on one of its own rows would delete the
 * object whose event is still being handled; these run after it. */
static void render_form(void);
static void render_grid(void);
static void form_render_async(void * p) { (void) p; render_form(); }
static void grid_render_async(void * p) { (void) p; render_grid(); }

/* ---- the home ---------------------------------------------------------------- */

static lv_obj_t * home_screen, * home_content, * home_name, * home_status, * home_dot;
static lv_obj_t * home_shelf; /* registered swipe dead zone, unregistered before each rebuild */
static subsonic_album_t * home_recent;
static int home_recent_count;
static char home_continue_id[64], home_continue_title[128], home_continue_artist[128], home_continue_cover[64];
static int home_continue_track = -1;
static lv_timer_t * poll_timer;

static bool home_is_dlna;
static dl_saved_t home_dl;
static dlna_server_t dl_server; /* described: has the control URL */
static bool dl_server_ready;
static dlna_object_t * home_dl_top, * home_dl_recent;
static int home_dl_top_n, home_dl_recent_n;
static char home_dl_recent_id[128];
static dl_continue_t home_dl_cont;
static bool home_dl_cont_ok;
static lv_obj_t * home_search;
static bool home_opening;

static void render_home(void);
static void dl_render_home(void);
static void dl_refresh_home(void);
static void open_add_type(void);
static void open_dlna_form(const dl_saved_t * s);
static void dl_apply_job(int done);
static void dl_teardown(void);
static void open_albums_grid(void);
static void open_server_sheet(void);
static void open_search(void);
static void open_server_form(int index);

static int saved_count(void) {
    subsonic_server_row_t * rows = NULL;
    int n = 0;
    metadata_db_load_subsonic_servers(&rows, &n);
    free(rows);
    dl_saved_t dl[DL_MAX_SAVED];
    return n + dl_load(dl);
}

static void set_status(const char * msg, lv_color_t dot) {
    if (!home_status) return;
    label_set_text_single_line(home_status, msg);
    lv_obj_set_style_bg_color(home_dot, dot, 0);
}

static void refresh_home(void) {
    covers_forget(OWNER_HOME);
    home_is_dlna = dl_active(&home_dl);
    if (home_search) lv_obj_set_style_opa(home_search, home_is_dlna ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (home_is_dlna) {
        dl_refresh_home();
        return;
    }
    free(home_recent);
    home_recent = NULL;
    home_recent_count = 0;
    char name[96];
    server_name(current_settings.subsonic_url, name, sizeof(name));
    lv_label_set_text(home_name, name);
    if (!gui_subsonic_continue_info(home_continue_id, sizeof(home_continue_id), home_continue_title,
                                    sizeof(home_continue_title), home_continue_artist, sizeof(home_continue_artist),
                                    home_continue_cover, sizeof(home_continue_cover), &home_continue_track))
        home_continue_track = -1;
    if (!wifi_ok()) {
        set_status(TR("Wi-Fi not connected"), lv_color_hex(GUI_COLOR_TEXT_DIM));
        render_home();
        return;
    }
    set_status(TR("Connecting…"), accent_lv_color());
    J.server = gui_subsonic_active_server();
    start_job(JOB_HOME);
    render_home();
}

static void shelf_album_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < home_recent_count) gui_subsonic_open_album(home_recent[i].id, home_recent[i].name, -1);
}

static void tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    switch ((int) (intptr_t) lv_event_get_user_data(e)) {
        case 0: open_albums_grid(); break;
        case 1: gui_subsonic_open_artists(); break;
        case 2: gui_subsonic_open_playlists(); break;
        case 3:
            J.server = gui_subsonic_active_server();
            if (start_job(JOB_RANDOM)) show_info_toast(TR("Picking random songs…"));
            break;
    }
}

static void continue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool play = lv_event_get_user_data(e) != NULL;
    gui_subsonic_open_album(home_continue_id, home_continue_title, play ? home_continue_track : -1);
}

static void add_server_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_add_type();
}

static lv_obj_t * nav_tile(lv_obj_t * parent, const char * symbol, const char * label, uint32_t color, int id) {
    lv_obj_t * t = box(parent, BOARD_UI_PX(158), BOARD_UI_PX(50), C_CARD, BOARD_UI_PX(14));
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_t * i = text(t, symbol, GUI_FONT_ROLE_BODY, lv_color_hex(color), 0);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, BOARD_UI_PX(14), 0);
    lv_obj_t * l = text(t, label, GUI_FONT_ROLE_BODY, C_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, BOARD_UI_PX(44), 0);
    return t;
}

static void render_home(void) {
    if (!home_content) return;
    covers_forget(OWNER_HOME);
    if (home_shelf) { unregister_swipe_dead_zone(home_shelf); home_shelf = NULL; }
    lv_obj_clean(home_content);

    if (!home_is_dlna && saved_count() == 0 && !current_settings.subsonic_url[0]) {
        lv_obj_add_flag(home_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * col = lv_obj_create(home_content);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(col, BOARD_UI_PX(30), 0);
        lv_obj_set_style_pad_row(col, BOARD_UI_PX(12), 0);
        lv_obj_t * badge = box(col, BOARD_UI_PX(72), BOARD_UI_PX(72), C_CARD, LV_RADIUS_CIRCLE);
        lv_obj_center(text(badge, LV_SYMBOL_WIFI, GUI_FONT_ROLE_TITLE, lv_color_hex(0x48ADE5), 0));
        text(col, TR("Stream from your own server"), GUI_FONT_ROLE_ROW, C_TEXT, 0);
        lv_obj_t * d = text(col, TR("A Subsonic server (Navidrome, Gonic, Airsonic) or the DLNA media server of your NAS."),
                            GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
        lv_obj_set_width(d, BOARD_UI_PX(290));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t * btn = box(col, BOARD_UI_PX(260), BOARD_UI_PX(46), accent_lv_color(), BOARD_UI_PX(23));
        lv_obj_set_style_margin_top(btn, BOARD_UI_PX(16), 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, add_server_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(text(btn, TR("Add a server"), GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_ON_ACCENT), 0));
        set_status("", C_MUTED);
        return;
    }
    lv_obj_remove_flag(home_name, LV_OBJ_FLAG_HIDDEN);
    if (home_is_dlna) {
        dl_render_home();
        return;
    }

    /* Recently added */
    lv_obj_t * head = lv_obj_create(home_content);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t * h = text(head, TR("Recently added"), GUI_FONT_ROLE_SUBTEXT, C_TEXT, 0);
    lv_obj_align(h, LV_ALIGN_LEFT_MID, BOARD_UI_PX(4), 0);
    lv_obj_t * shelf = lv_obj_create(home_content);
    lv_obj_remove_style_all(shelf);
    lv_obj_set_size(shelf, lv_pct(100), SHELF_COVER + BOARD_UI_PX(44));
    lv_obj_set_flex_flow(shelf, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(shelf, BOARD_UI_PX(10), 0);
    lv_obj_set_scroll_dir(shelf, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(shelf, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(shelf, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(shelf);
    home_shelf = shelf;
    if (atomic_load(&job_running) == JOB_HOME) {
        loading(shelf, TR("Loading…"));
    } else if (home_recent_count == 0) {
        lv_obj_t * m = text(shelf, wifi_ok() ? TR("Nothing to show yet.") : TR("Connect to Wi-Fi to browse."),
                            GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
        lv_obj_set_style_pad_top(m, BOARD_UI_PX(40), 0);
    }
    for (int i = 0; i < home_recent_count; i++) {
        lv_obj_t * card = lv_obj_create(shelf);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, SHELF_COVER, SHELF_COVER + BOARD_UI_PX(40));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, shelf_album_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        cover_image(card, home_recent[i].cover_art, SHELF_COVER, OWNER_HOME);
        lv_obj_t * t = text(card, home_recent[i].name, GUI_FONT_ROLE_SUBTEXT, C_TEXT, SHELF_COVER);
        lv_obj_set_pos(t, 0, SHELF_COVER + BOARD_UI_PX(4));
        lv_obj_t * a = text(card, home_recent[i].artist, GUI_FONT_ROLE_SUBTEXT, C_MUTED, SHELF_COVER);
        lv_obj_set_pos(a, 0, SHELF_COVER + BOARD_UI_PX(22));
    }

    /* Ways in */
    lv_obj_t * grid = lv_obj_create(home_content);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, BOARD_UI_PX(8), 0);
    lv_obj_set_style_pad_row(grid, BOARD_UI_PX(8), 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    nav_tile(grid, LV_SYMBOL_AUDIO, TR("Albums"), 0xF5B457, 0);
    nav_tile(grid, LV_SYMBOL_LIST, TR("Artists"), 0x48ADE5, 1);
    nav_tile(grid, LV_SYMBOL_DIRECTORY, TR("Playlists"), 0x39BD95, 2);
    nav_tile(grid, LV_SYMBOL_SHUFFLE, TR("Random mix"), 0xF08A5D, 3);

    /* Continue */
    if (home_continue_track >= 0) {
        lv_obj_t * card = box(home_content, lv_pct(100), BOARD_UI_PX(64), C_PANEL, BOARD_UI_PX(14));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, continue_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * c = cover_image(card, home_continue_cover, ROW_COVER, OWNER_HOME);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, BOARD_UI_PX(10), 0);
        lv_obj_t * k = text(card, TR("Continue"), GUI_FONT_ROLE_SUBTEXT, accent_lv_color(), 0);
        lv_obj_set_pos(k, BOARD_UI_PX(64), BOARD_UI_PX(4));
        lv_obj_t * t = text(card, home_continue_title, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(200));
        lv_obj_set_pos(t, BOARD_UI_PX(64), BOARD_UI_PX(22));
        char sub[180];
        snprintf(sub, sizeof(sub), TR("%s · track %d"), home_continue_artist, home_continue_track + 1);
        lv_obj_t * s = text(card, sub, GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(200));
        lv_obj_set_pos(s, BOARD_UI_PX(64), BOARD_UI_PX(42));
        lv_obj_t * play = box(card, BOARD_UI_PX(40), BOARD_UI_PX(40), accent_lv_color(), LV_RADIUS_CIRCLE);
        lv_obj_align(play, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-10), 0);
        lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(play, continue_cb, LV_EVENT_CLICKED, (void *) 1);
        lv_obj_center(text(play, LV_SYMBOL_PLAY, GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_ON_ACCENT), 0));
    }
}

static void name_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_server_sheet();
}

static void search_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (home_is_dlna) show_info_toast(TR("Search isn't available for DLNA servers yet"));
    else open_search();
}

static void home_loaded_cb(lv_event_t * e) {
    (void) e;
    /* Back from Settings or a server switch: show what is current now. */
    if (home_opening) return;
    dl_saved_t cur;
    bool dl = dl_active(&cur);
    if (dl != home_is_dlna || (dl && (strcmp(cur.location, home_dl.location) || strcmp(cur.name, home_dl.name)))) {
        if (atomic_load(&job_running) == JOB_NONE) refresh_home();
        return;
    }
    if (dl) home_dl_cont_ok = dl_continue_load(&home_dl_cont) && !strcmp(home_dl_cont.location, home_dl.location);
    if (atomic_load(&job_running) == JOB_NONE) render_home();
}

static void build_home(void) {
    home_screen = lv_obj_create(NULL);
    lv_obj_add_style(home_screen, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(home_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * t = build_screen_header(home_screen, "", generic_back_cb, NULL, NULL);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    /* Server name (tap to switch) with a status line under it. */
    lv_obj_t * head = lv_obj_create(home_screen);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, BOARD_UI_PX(240), BOARD_UI_PX(46));
    lv_obj_set_pos(head, BOARD_UI_PX(56), STATUS_BAR_CLEARANCE + BOARD_UI_PX(4));
    lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(head, name_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * name_row = lv_obj_create(head);
    lv_obj_remove_style_all(name_row);
    lv_obj_set_size(name_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(name_row, BOARD_UI_PX(6), 0);
    lv_obj_remove_flag(name_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(name_row, 0, 0);
    home_name = text(name_row, "", GUI_FONT_ROLE_ROW, C_TEXT, 0);
    lv_obj_set_style_max_width(home_name, BOARD_UI_PX(200), 0);
    lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
    text(name_row, LV_SYMBOL_DOWN, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    home_dot = box(head, BOARD_UI_PX(7), BOARD_UI_PX(7), C_MUTED, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(home_dot, 0, BOARD_UI_PX(32));
    home_status = text(head, "", GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(220));
    lv_obj_set_pos(home_status, BOARD_UI_PX(12), BOARD_UI_PX(25));
    lv_obj_t * search = box(home_screen, BOARD_UI_PX(44), BOARD_UI_PX(44), lv_color_black(), BOARD_UI_PX(22));
    lv_obj_set_style_bg_opa(search, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(search, BOARD_SCREEN_WIDTH - BOARD_UI_PX(56), STATUS_BAR_CLEARANCE + BOARD_UI_PX(4));
    lv_obj_add_flag(search, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(search, search_cb, LV_EVENT_CLICKED, NULL);
    home_search = search;
    lv_obj_t * glass = lv_image_create(search);
    lv_image_set_src(glass, asset_path("sub_back/btn_search.png"));
    lv_obj_set_size(glass, BOARD_UI_PX(26), BOARD_UI_PX(26));
    lv_image_set_inner_align(glass, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(glass, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(glass);
    home_content = lv_obj_create(home_screen);
    lv_obj_remove_style_all(home_content);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(home_content, lv_pct(100), BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(home_content, 0, top);
    lv_obj_set_style_pad_hor(home_content, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_bottom(home_content, BOARD_UI_PX(16), 0);
    lv_obj_set_style_pad_row(home_content, BOARD_UI_PX(10), 0);
    lv_obj_set_flex_flow(home_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(home_content, LV_DIR_VER);
    lv_obj_add_event_cb(home_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(home_screen);
}

/* ---- albums grid -------------------------------------------------------------- */

static lv_obj_t * grid_screen, * grid_content, * grid_sort_label;
static subsonic_album_t * grid_albums;
static int grid_count;
static int grid_sort;
static bool grid_more;
static bool grid_failed; /* the last page request failed (not: the server has no albums) */
static const char * const grid_sort_type[] = { "newest", "alphabeticalByName", "frequent", "random" };
static const char * const grid_sort_name[] = { N_("Newest"), N_("A–Z"), N_("Most played"), N_("Random") };

static void grid_album_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < grid_count) gui_subsonic_open_album(grid_albums[i].id, grid_albums[i].name, -1);
}

static void grid_load(bool more) {
    if (!more) {
        free(grid_albums);
        grid_albums = NULL;
        grid_count = 0;
    }
    J.server = gui_subsonic_active_server();
    snprintf(J.type, sizeof(J.type), "%s", grid_sort_type[grid_sort]);
    J.offset = grid_count;
    if (start_job(JOB_ALBUMS)) {
        grid_failed = false;
        lv_async_call(grid_render_async, NULL);
    }
}

static void grid_retry_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) grid_load(grid_count > 0);
}

static void grid_sort_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || atomic_load(&job_running) != JOB_NONE) return;
    grid_sort = (grid_sort + 1) % 4;
    lv_label_set_text(grid_sort_label, TR(grid_sort_name[grid_sort]));
    grid_load(false);
}

static void grid_more_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) grid_load(true);
}

static void render_grid(void) {
    covers_forget(OWNER_GRID);
    lv_obj_clean(grid_content);
    lv_obj_t * wrap = lv_obj_create(grid_content);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(wrap, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_row(wrap, BOARD_UI_PX(12), 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < grid_count; i++) {
        lv_obj_t * card = lv_obj_create(wrap);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, GRID_COVER, GRID_COVER + BOARD_UI_PX(42));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, grid_album_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        cover_image(card, grid_albums[i].cover_art, GRID_COVER, OWNER_GRID);
        lv_obj_t * t = text(card, grid_albums[i].name, GUI_FONT_ROLE_BODY, C_TEXT, GRID_COVER);
        lv_obj_set_pos(t, 0, GRID_COVER + BOARD_UI_PX(4));
        lv_obj_t * a = text(card, grid_albums[i].artist, GUI_FONT_ROLE_SUBTEXT, C_MUTED, GRID_COVER);
        lv_obj_set_pos(a, 0, GRID_COVER + BOARD_UI_PX(24));
    }
    if (atomic_load(&job_running) == JOB_ALBUMS) {
        loading(grid_content, TR("Loading…"));
    } else if (grid_failed) {
        loading(grid_content, TR("Couldn't load. Check the connection."));
        try_again_row(grid_content, grid_retry_cb);
    } else if (grid_more) {
        lv_obj_t * more = box(grid_content, lv_pct(100), BOARD_UI_PX(44), C_CARD, BOARD_UI_PX(22));
        lv_obj_add_flag(more, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(more, grid_more_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(text(more, TR("Show more"), GUI_FONT_ROLE_BODY, C_TEXT, 0));
    } else if (grid_count == 0) {
        loading(grid_content, TR("No albums"));
    }
}

static void open_albums_grid(void) {
    if (!grid_screen) {
        lv_obj_t * title;
        grid_screen = new_screen(TR("Albums"), &title, &grid_content);
        lv_obj_t * chip = box(grid_screen, BOARD_UI_PX(110), BOARD_UI_PX(30), C_CARD, BOARD_UI_PX(15));
        lv_obj_set_pos(chip, BOARD_SCREEN_WIDTH - BOARD_UI_PX(122), STATUS_BAR_CLEARANCE + BOARD_UI_PX(10));
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, grid_sort_cb, LV_EVENT_CLICKED, NULL);
        grid_sort_label = text(chip, "", GUI_FONT_ROLE_SUBTEXT, C_TEXT, 0);
        lv_obj_center(grid_sort_label);
    }
    lv_label_set_text(grid_sort_label, TR(grid_sort_name[grid_sort]));
    nav_push(grid_screen);
    grid_load(false);
}

/* ---- search -------------------------------------------------------------------- */

static lv_obj_t * search_screen, * search_content, * search_title;
static subsonic_album_t * search_albums;
static int search_album_count;
static subsonic_song_t * search_songs;
static int search_song_count;
static bool search_failed; /* the request failed (not: nothing matched) */
static char search_query[sizeof(J.query)];
static void render_search(void);
static void search_render_async(void * p) { (void) p; render_search(); }
static void search_retry_cb(lv_event_t * e);

static void search_album_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < search_album_count) gui_subsonic_open_album(search_albums[i].id, search_albums[i].name, -1);
}

static void search_song_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= search_song_count) return;
    subsonic_song_t * copy = malloc(sizeof(subsonic_song_t) * (size_t) search_song_count);
    if (!copy) return;
    memcpy(copy, search_songs, sizeof(subsonic_song_t) * (size_t) search_song_count);
    gui_subsonic_play_songs(copy, search_song_count, i);
}

static lv_obj_t * list_row(lv_obj_t * parent, const char * cover, const char * title, const char * sub,
                           lv_event_cb_t cb, int index) {
    lv_obj_t * row = box(parent, lv_pct(100), BOARD_UI_PX(56), lv_color_black(), BOARD_UI_PX(10));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
    int32_t x = BOARD_UI_PX(6);
    if (cover) {
        lv_obj_t * c = cover_image(row, cover, ROW_COVER, OWNER_SEARCH);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, BOARD_UI_PX(6), 0);
        x = BOARD_UI_PX(60);
    }
    lv_obj_t * t = text(row, title, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(330) - x);
    lv_obj_set_pos(t, x, BOARD_UI_PX(7));
    lv_obj_t * s = text(row, sub, GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(330) - x);
    lv_obj_set_pos(s, x, BOARD_UI_PX(30));
    return row;
}

static void render_search(void) {
    covers_forget(OWNER_SEARCH);
    lv_obj_clean(search_content);
    if (atomic_load(&job_running) == JOB_SEARCH) { loading(search_content, TR("Searching…")); return; }
    if (search_failed) {
        loading(search_content, TR("Couldn't load. Check the connection."));
        try_again_row(search_content, search_retry_cb);
        return;
    }
    if (!search_album_count && !search_song_count) { loading(search_content, TR("Nothing found")); return; }
    if (search_album_count) section(search_content, TR("Albums"));
    for (int i = 0; i < search_album_count; i++)
        list_row(search_content, search_albums[i].cover_art[0] ? search_albums[i].cover_art : "", search_albums[i].name,
                 search_albums[i].artist, search_album_cb, i);
    if (search_song_count) section(search_content, TR("Songs"));
    for (int i = 0; i < search_song_count; i++) {
        char sub[300];
        snprintf(sub, sizeof(sub), "%s · %s", search_songs[i].artist, search_songs[i].album);
        list_row(search_content, NULL, search_songs[i].title, sub, search_song_cb, i);
    }
}

static bool search_start(const char * q) {
    free(search_albums);
    free(search_songs);
    search_albums = NULL;
    search_songs = NULL;
    search_album_count = search_song_count = 0;
    snprintf(search_query, sizeof(search_query), "%s", q);
    J.server = gui_subsonic_active_server();
    snprintf(J.query, sizeof(J.query), "%s", q);
    /* Not started (a request is still finishing): nothing loaded, so the
     * screen keeps offering Try again rather than "Nothing found". */
    search_failed = !start_job(JOB_SEARCH);
    return !search_failed;
}

static void search_retry_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !search_query[0]) return;
    char q[sizeof(search_query)];
    snprintf(q, sizeof(q), "%s", search_query);
    /* Not from inside the row's own event: render_search() deletes it. */
    (void) search_start(q);
    lv_async_call(search_render_async, NULL);
}

static void search_done(const char * q, void * user) {
    (void) user;
    if (!q || !q[0]) return;
    if (!search_screen) search_screen = new_screen("", &search_title, &search_content);
    char title[160];
    snprintf(title, sizeof(title), "\"%s\"", q);
    lv_label_set_text(search_title, title);
    if (search_start(q)) {
        nav_push(search_screen);
        render_search();
    }
}

static void open_search(void) {
    show_text_entry(TR("Search the server"), "", false, false, 0, search_done, NULL);
}

/* ---- server switcher sheet ------------------------------------------------------ */

static lv_obj_t * sheet, * sheet_backdrop;
static subsonic_server_row_t * sheet_rows;
static int sheet_count;

static void sheet_close(void) {
    /* Usually called from a tap on the sheet itself: delete after the event. */
    if (sheet_backdrop) { lv_obj_delete_async(sheet_backdrop); sheet_backdrop = NULL; }
    if (sheet) { lv_obj_delete_async(sheet); sheet = NULL; }
    free(sheet_rows);
    sheet_rows = NULL;
    sheet_count = 0;
}

static void sheet_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) sheet_close();
}

static void sheet_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < sheet_count) {
        subsonic_server_t s = { 0 };
        snprintf(s.base_url, sizeof(s.base_url), "%s", sheet_rows[i].url);
        snprintf(s.username, sizeof(s.username), "%s", sheet_rows[i].username);
        snprintf(s.password, sizeof(s.password), "%s", sheet_rows[i].password);
        s.verify_tls = sheet_rows[i].verify_tls;
        dl_set_active(NULL);
        gui_subsonic_activate_server(&s);
    }
    sheet_close();
    refresh_home();
}

static dl_saved_t sheet_dl[DL_MAX_SAVED];
static int sheet_dl_n;

static void sheet_pick_dl_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < sheet_dl_n) dl_set_active(sheet_dl[i].location);
    sheet_close();
    refresh_home();
}

static void sheet_manage_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    sheet_close();
    gui_stream_settings_open();
}

static void open_server_sheet(void) {
    if (sheet) return;
    metadata_db_load_subsonic_servers(&sheet_rows, &sheet_count);
    sheet_dl_n = dl_load(sheet_dl);
    char active_dl[600];
    bool dl_on = dl_active_location(active_dl, sizeof(active_dl));
    sheet_backdrop = box(lv_layer_top(), BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sheet_backdrop, LV_OPA_50, 0);
    lv_obj_add_flag(sheet_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sheet_backdrop, sheet_backdrop_cb, LV_EVENT_CLICKED, NULL);
    int32_t h = BOARD_UI_PX(70) + BOARD_UI_PX(52) * (sheet_count + sheet_dl_n + 1);
    if (h > BOARD_SCREEN_HEIGHT - BOARD_UI_PX(80)) h = BOARD_SCREEN_HEIGHT - BOARD_UI_PX(80);
    sheet = box(lv_layer_top(), BOARD_SCREEN_WIDTH, h + BOARD_UI_PX(16), lv_color_hex(GUI_COLOR_SHEET), BOARD_UI_PX(18));
    lv_obj_set_pos(sheet, 0, BOARD_SCREEN_HEIGHT - h);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sheet, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_row(sheet, BOARD_UI_PX(4), 0);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    text(sheet, TR("Servers"), GUI_FONT_ROLE_ROW, C_TEXT, 0);
    for (int i = 0; i < sheet_count; i++) {
        char name[96], host[96];
        server_name(sheet_rows[i].url, name, sizeof(name));
        host_of(sheet_rows[i].url, host, sizeof(host));
        lv_obj_t * row = list_row(sheet, NULL, name, host, sheet_pick_cb, i);
        if (!dl_on && strcmp(sheet_rows[i].url, current_settings.subsonic_url) == 0) {
            lv_obj_t * ok = text(row, LV_SYMBOL_OK, GUI_FONT_ROLE_BODY, accent_lv_color(), 0);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-10), 0);
        }
    }
    for (int i = 0; i < sheet_dl_n; i++) {
        char sub[160], host[96];
        host_of(sheet_dl[i].location, host, sizeof(host));
        snprintf(sub, sizeof(sub), "DLNA · %s", host);
        lv_obj_t * row = list_row(sheet, NULL, sheet_dl[i].name, sub, sheet_pick_dl_cb, i);
        if (dl_on && !strcmp(sheet_dl[i].location, active_dl)) {
            lv_obj_t * ok = text(row, LV_SYMBOL_OK, GUI_FONT_ROLE_BODY, accent_lv_color(), 0);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-10), 0);
        }
    }
    lv_obj_t * m = list_row(sheet, NULL, TR("Manage servers"), TR("Settings › Streaming"), sheet_manage_cb, 0);
    (void) m;
}

/* ---- Settings > Streaming ---------------------------------------------------------- */

static lv_obj_t * set_screen, * set_content;
static subsonic_server_row_t * set_rows;
static int set_count;

static void render_settings(void);

static lv_obj_t * set_row(lv_obj_t * group, const char * symbol, uint32_t color, const char * label, const char * value,
                          lv_event_cb_t cb, int id, bool last) {
    lv_obj_t * row = box(group, lv_pct(100), BOARD_UI_PX(50), C_CARD, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    if (!last) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER), 0);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_t * i = text(row, symbol, GUI_FONT_ROLE_BODY, lv_color_hex(color), 0);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, BOARD_UI_PX(14), 0);
    lv_obj_t * l = text(row, label, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(170));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, BOARD_UI_PX(46), 0);
    lv_obj_t * v = text(row, value, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-34), 0);
    lv_obj_t * c = text(row, LV_SYMBOL_RIGHT, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), 0);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-12), 0);
    return row;
}

static lv_obj_t * set_group(lv_obj_t * parent, const char * title) {
    section(parent, title);
    lv_obj_t * g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(g, BOARD_UI_PX(14), 0);
    lv_obj_set_style_clip_corner(g, true, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

static dl_saved_t set_dl[DL_MAX_SAVED];
static int set_dl_n;

static void set_server_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int id = (int) (intptr_t) lv_event_get_user_data(e);
    if (id < 0) open_add_type();
    else open_server_form(id);
}

static void set_dl_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < set_dl_n) open_dlna_form(&set_dl[i]);
}

static void set_quality_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_subsonic_open_quality();
}

static void set_downloads_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_subsonic_open_download_settings();
}

static void render_settings(void) {
    lv_obj_clean(set_content);
    free(set_rows);
    set_rows = NULL;
    set_count = 0;
    metadata_db_load_subsonic_servers(&set_rows, &set_count);
    char active_dl[600];
    bool dl_on = dl_active_location(active_dl, sizeof(active_dl));
    lv_obj_t * g = set_group(set_content, TR("Servers"));
    for (int i = 0; i < set_count; i++) {
        char name[96];
        server_name(set_rows[i].url, name, sizeof(name));
        bool active = !dl_on && strcmp(set_rows[i].url, current_settings.subsonic_url) == 0;
        set_row(g, LV_SYMBOL_WIFI, active ? 0x7FD17F : 0xA3A89F, name, active ? TR("In use") : TR("Subsonic"), set_server_cb, i,
                false);
    }
    set_dl_n = dl_load(set_dl);
    for (int i = 0; i < set_dl_n; i++) {
        bool active = dl_on && !strcmp(set_dl[i].location, active_dl);
        set_row(g, LV_SYMBOL_DRIVE, active ? 0x7FD17F : 0xA3A89F, set_dl[i].name, active ? TR("In use") : "DLNA", set_dl_cb,
                i, false);
    }
    set_row(g, LV_SYMBOL_PLUS, 0xA3A89F, TR("Add a server"), "", set_server_cb, -1, true);
    static const char * const q[] = { N_("Original"), N_("96 kbps"), N_("192 kbps"), N_("320 kbps") };
    int qi = current_settings.subsonic_stream_quality;
    g = set_group(set_content, TR("Playback"));
    set_row(g, LV_SYMBOL_AUDIO, 0xB9A4F0, TR("Stream quality"), TR(q[(qi >= 0 && qi < 4) ? qi : 0]), set_quality_cb, 0, true);
    g = set_group(set_content, TR("Downloads"));
    set_row(g, LV_SYMBOL_DOWNLOAD, 0xF0C879, TR("Save downloads to"),
            current_settings.subsonic_download_subfolder[0] ? current_settings.subsonic_download_subfolder : TR("SD card"),
            set_downloads_cb, 0, true);
}

static void set_loaded_cb(lv_event_t * e) {
    (void) e;
    render_settings();
}

static void ensure_poll(void);

void gui_stream_settings_open(void) {
    ensure_poll();
    if (!set_screen) {
        set_screen = new_screen(TR("Streaming"), NULL, &set_content);
        lv_obj_add_event_cb(set_screen, set_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    }
    render_settings();
    nav_push(set_screen);
}

/* ---- add / edit a server ------------------------------------------------------------ */

static lv_obj_t * form_screen, * form_content, * form_title, * form_result;
static struct {
    int index; /* -1 new */
    char original_url[256];
    char name[96];
    subsonic_server_t server;
    bool confirm_remove;
    bool tested_ok;
} F;

static void form_text_done(const char * value, void * user) {
    int field = (int) (intptr_t) user;
    if (!value) return;
    switch (field) {
        case 0: snprintf(F.name, sizeof(F.name), "%s", value); break;
        case 1: snprintf(F.server.base_url, sizeof(F.server.base_url), "%s", value); break;
        case 2: snprintf(F.server.username, sizeof(F.server.username), "%s", value); break;
        case 3: snprintf(F.server.password, sizeof(F.server.password), "%s", value); break;
    }
    F.tested_ok = false;
    render_form();
}

static void form_field_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int field = (int) (intptr_t) lv_event_get_user_data(e);
    static const char * const titles[] = { N_("Name"), N_("Address (e.g. http://192.168.1.10:4533)"), N_("Username"),
                                           N_("Password") };
    const char * initial = field == 0 ? F.name : field == 1 ? F.server.base_url : field == 2 ? F.server.username : "";
    size_t size = field == 0 ? sizeof(F.name) : field == 1 ? sizeof(F.server.base_url)
                : field == 2 ? sizeof(F.server.username) : sizeof(F.server.password);
    show_text_entry(TR(titles[field]), initial, field == 3, false, size, form_text_done, (void *) (intptr_t) field);
}

static void form_tls_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    F.server.verify_tls = !F.server.verify_tls;
    lv_async_call(form_render_async, NULL);
}

static void form_test_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!F.server.base_url[0]) { show_info_toast(TR("Enter the address first")); return; }
    if (!wifi_ok()) { show_error_toast(TR("Connect to Wi-Fi first")); return; }
    J.server = F.server;
    if (start_job(JOB_TEST)) {
        lv_label_set_text(form_result, TR("Testing…"));
        lv_obj_set_style_text_color(form_result, C_MUTED, 0);
    }
}

static void form_save_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!F.server.base_url[0]) { show_info_toast(TR("Enter the address first")); return; }
    if (F.index >= 0 && strcmp(F.original_url, F.server.base_url) != 0) {
        /* The address is the key: an edited address replaces the old entry. */
        metadata_db_subsonic_server_remove(F.original_url);
        settings_subsonic_clear_active(&current_settings, F.original_url);
        set_server_name(F.original_url, NULL);
    }
    metadata_db_subsonic_server_save(F.server.base_url, F.server.username, F.server.password, F.server.verify_tls);
    set_server_name(F.server.base_url, F.name[0] ? F.name : NULL);
    /* The first server, or the one in use, becomes / stays the active one. */
    if (!current_settings.subsonic_url[0] || (F.index >= 0 && strcmp(current_settings.subsonic_url, F.original_url) == 0))
        gui_subsonic_activate_server(&F.server);
    else
        settings_save(&current_settings);
    show_info_toast(TR("Server saved"));
    nav_pop();
}

static void form_remove_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || F.index < 0) return;
    if (!F.confirm_remove) {
        F.confirm_remove = true;
        lv_async_call(form_render_async, NULL);
        return;
    }
    metadata_db_subsonic_server_remove(F.original_url);
    settings_subsonic_clear_active(&current_settings, F.original_url);
    set_server_name(F.original_url, NULL);
    settings_save(&current_settings);
    show_info_toast(TR("Server removed"));
    nav_pop();
}

static lv_obj_t * form_field(lv_obj_t * parent, const char * label, const char * value, int field) {
    text(parent, label, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_t * f = box(parent, lv_pct(100), BOARD_UI_PX(38), C_CARD, BOARD_UI_PX(10));
    lv_obj_set_style_bg_color(f, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, form_field_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);
    lv_obj_t * v = text(f, value, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(300));
    lv_obj_align(v, LV_ALIGN_LEFT_MID, BOARD_UI_PX(12), 0);
    return f;
}

static lv_obj_t * pill(lv_obj_t * parent, const char * label, bool primary, lv_color_t text_color, lv_event_cb_t cb) {
    lv_obj_t * b = box(parent, 0, BOARD_UI_PX(44), primary ? accent_lv_color() : lv_color_hex(GUI_COLOR_CONTROL), BOARD_UI_PX(22));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(text(b, label, GUI_FONT_ROLE_BODY, primary ? lv_color_hex(GUI_COLOR_ON_ACCENT) : text_color, 0));
    return b;
}

static void render_form(void) {
    lv_label_set_text(form_title, F.index < 0 ? TR("Add a server") : TR("Edit server"));
    lv_obj_clean(form_content);
    char host[96];
    host_of(F.server.base_url, host, sizeof(host));
    form_field(form_content, TR("Name"), F.name[0] ? F.name : (host[0] ? host : TR("Not set")), 0);
    form_field(form_content, TR("Address"), F.server.base_url[0] ? F.server.base_url : TR("Not set"), 1);
    form_field(form_content, TR("Username"), F.server.username[0] ? F.server.username : TR("Not set"), 2);
    form_field(form_content, TR("Password"), F.server.password[0] ? "••••••••" : TR("Not set"), 3);
    lv_obj_t * tls = box(form_content, lv_pct(100), BOARD_UI_PX(34), lv_color_black(), BOARD_UI_PX(10));
    lv_obj_set_style_bg_opa(tls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tls, form_tls_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * tl = text(tls, TR("Verify HTTPS certificate"), GUI_FONT_ROLE_BODY, C_TEXT, 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, BOARD_UI_PX(2), 0);
    lv_obj_t * tv = text(tls, F.server.verify_tls ? TR("On") : TR("Off"), GUI_FONT_ROLE_BODY,
                         F.server.verify_tls ? accent_lv_color() : C_MUTED, 0);
    lv_obj_align(tv, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-4), 0);
    form_result = text(form_content, "", GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_set_width(form_result, lv_pct(100));
    lv_label_set_long_mode(form_result, LV_LABEL_LONG_WRAP);
    if (F.tested_ok) {
        lv_label_set_text(form_result, TR(LV_SYMBOL_OK "  Connected"));
        lv_obj_set_style_text_color(form_result, lv_color_hex(GUI_COLOR_SUCCESS), 0);
    }
    lv_obj_t * row = lv_obj_create(form_content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(52));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BOARD_UI_PX(8), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    pill(row, TR("Test"), false, C_TEXT, form_test_cb);
    if (F.index >= 0)
        pill(row, F.confirm_remove ? TR("Sure?") : TR("Remove"), false, lv_color_hex(GUI_COLOR_ERROR), form_remove_cb);
    pill(row, TR("Save"), true, C_TEXT, form_save_cb);
}

static void open_server_form(int index) {
    memset(&F, 0, sizeof(F));
    F.index = index;
    F.server.verify_tls = true;
    subsonic_server_row_t * rows = NULL;
    int n = 0;
    metadata_db_load_subsonic_servers(&rows, &n);
    if (index >= 0 && index < n) {
        snprintf(F.original_url, sizeof(F.original_url), "%s", rows[index].url);
        snprintf(F.server.base_url, sizeof(F.server.base_url), "%s", rows[index].url);
        snprintf(F.server.username, sizeof(F.server.username), "%s", rows[index].username);
        snprintf(F.server.password, sizeof(F.server.password), "%s", rows[index].password);
        F.server.verify_tls = rows[index].verify_tls;
        char name[96];
        server_name(rows[index].url, name, sizeof(name));
        char host[96];
        host_of(rows[index].url, host, sizeof(host));
        if (strcmp(name, host) != 0) snprintf(F.name, sizeof(F.name), "%s", name);
    } else {
        F.index = -1;
    }
    free(rows);
    if (!form_screen) {
        form_screen = new_screen("", &form_title, &form_content);
        lv_obj_set_style_pad_row(form_content, BOARD_UI_PX(4), 0); /* everything on one screen */
    }
    render_form();
    nav_push(form_screen);
}

/* ---- DLNA: home, folders, playback --------------------------------------------------
 *
 * A DLNA server's home shows its own top folders (inside "Music" when the
 * root splits by media type), a "Recently added" shelf when the server has
 * such a folder, and Continue. Each folder opens as its own screen; screens
 * that left the navigation stack are deleted the next time one unloads. */

#define DL_PAGE 100
#define DL_MAX_LEVELS 12
#define OWNER_DL 10 /* + level slot */


typedef struct {
    lv_obj_t * scr, * content;
    char id[128], name[128], crumb[256], filter[128], art[512];
    dlna_object_t * objs;
    int n, total;
    bool loading, failed;
    int autoplay; /* playable index to start once loaded, -1 none */
    unsigned gen;
    int slot;
} dl_level_t;

static dl_level_t * dl_levels[DL_MAX_LEVELS];
static unsigned dl_gen_seq;

static void dl_render_level(dl_level_t * L);
static void dl_level_load(dl_level_t * L, bool more);

static bool dl_playable(const dlna_object_t * o) {
    return !o->is_container && o->codec != DLNA_CODEC_NONE && o->res_url[0];
}

static void dl_format(const dlna_object_t * o, char * out, size_t size) {
    if (o->codec == DLNA_CODEC_NONE) {
        const char * sub = strchr(o->mime, '/');
        char fmt[24] = "";
        if (sub) {
            size_t i = 0;
            for (sub++; *sub && i + 1 < sizeof(fmt); sub++) fmt[i++] = (char) toupper((unsigned char) (*sub == '-' ? ' ' : *sub));
            fmt[i] = '\0';
        }
        if (fmt[0] == 'X' && fmt[1] == ' ') memmove(fmt, fmt + 2, strlen(fmt + 2) + 1);
        snprintf(out, size, "%s · %s", fmt[0] ? fmt : TR("Not audio"), TR("can't stream"));
        return;
    }
    const char * c = o->codec == DLNA_CODEC_FLAC ? "FLAC" : o->codec == DLNA_CODEC_MP3 ? "MP3" : "AAC";
    if (o->bits && o->sample_rate) {
        unsigned k = o->sample_rate / 1000, f = (o->sample_rate % 1000) / 100;
        if (f) snprintf(out, size, "%s · %u/%u.%u", c, o->bits, k, f);
        else snprintf(out, size, "%s · %u/%u", c, o->bits, k);
    } else if (o->sample_rate) {
        snprintf(out, size, "%s · %u.%u kHz", c, o->sample_rate / 1000, (o->sample_rate % 1000) / 100);
    } else {
        snprintf(out, size, "%s", c);
    }
}

static void dl_symbol_for(const char * title, const char ** symbol, uint32_t * color) {
    *symbol = LV_SYMBOL_DIRECTORY;
    *color = 0xA3A89F;
    if (contains_ci(title, "album")) { *symbol = LV_SYMBOL_AUDIO; *color = 0xF5B457; }
    else if (contains_ci(title, "artist") || contains_ci(title, "interpret")) { *symbol = LV_SYMBOL_LIST; *color = 0x48ADE5; }
    else if (contains_ci(title, "genre")) { *symbol = LV_SYMBOL_SHUFFLE; *color = 0xB9A4F0; }
    else if (contains_ci(title, "folder") || contains_ci(title, "ordner") || contains_ci(title, "browse")) { *color = 0x39BD95; }
    else if (contains_ci(title, "playlist")) { *symbol = LV_SYMBOL_LIST; *color = 0xF0C879; }
    else if (contains_ci(title, "recent") || contains_ci(title, "latest") || contains_ci(title, "newest")) { *symbol = LV_SYMBOL_REFRESH; *color = 0xF08A5D; }
    else if (contains_ci(title, "all") || contains_ci(title, "track") || contains_ci(title, "song")) { *symbol = LV_SYMBOL_AUDIO; *color = 0xA3A89F; }
}

/* A rounded card holding rows (Settings-style, no title). */
static lv_obj_t * plain_group(lv_obj_t * parent) {
    lv_obj_t * g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(g, BOARD_UI_PX(14), 0);
    lv_obj_set_style_clip_corner(g, true, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

/* ---- folder screens ---- */

static dl_level_t * dl_level_by_gen(unsigned gen) {
    for (int i = 0; i < DL_MAX_LEVELS; i++)
        if (dl_levels[i] && dl_levels[i]->gen == gen) return dl_levels[i];
    return NULL;
}

static void dl_level_free(int slot) {
    dl_level_t * L = dl_levels[slot];
    if (!L) return;
    covers_forget(OWNER_DL + slot);
    if (L->scr) {
        gui_navigation_remove_screen_instances(&L->scr, 1);
        lv_obj_delete_async(L->scr);
    }
    free(L->objs);
    free(L);
    dl_levels[slot] = NULL;
}

/* Deletes folder screens that are no longer reachable with Back. */
static void dl_purge(void) {
    for (int i = 0; i < DL_MAX_LEVELS; i++)
        if (dl_levels[i] && dl_levels[i]->scr != lv_screen_active() && !gui_navigation_contains(dl_levels[i]->scr))
            dl_level_free(i);
}

static void dl_level_unloaded_cb(lv_event_t * e) {
    (void) e;
    dl_purge();
}

static void dl_open_level(dl_level_t * parent, const char * id, const char * name, const char * filter, int autoplay) {
    if (!dl_server_ready) { show_info_toast(TR("Still connecting…")); return; }
    dl_purge();
    int slot = -1;
    for (int i = 0; i < DL_MAX_LEVELS && slot < 0; i++)
        if (!dl_levels[i]) slot = i;
    if (slot < 0) { show_info_toast(TR("Folders are nested too deep")); return; }
    dl_level_t * L = calloc(1, sizeof(*L));
    if (!L) return;
    L->slot = slot;
    L->gen = ++dl_gen_seq;
    L->autoplay = autoplay;
    snprintf(L->id, sizeof(L->id), "%s", id);
    utf8_truncate_safe(L->name, name && name[0] ? name : TR("Folder"), sizeof(L->name));
    snprintf(L->filter, sizeof(L->filter), "%s", filter ? filter : "");
    char crumb[sizeof(L->crumb) + sizeof(L->name) + 8];
    if (parent) snprintf(crumb, sizeof(crumb), "%s › %s", parent->crumb, parent->name);
    else snprintf(crumb, sizeof(crumb), "%s", home_dl.name);
    utf8_truncate_safe(L->crumb, crumb, sizeof(L->crumb));
    L->scr = new_screen(L->name, NULL, &L->content);
    lv_obj_add_event_cb(L->scr, dl_level_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    dl_levels[slot] = L;
    nav_push(L->scr);
    dl_level_load(L, false);
    dl_render_level(L);
}

static void dl_level_load(dl_level_t * L, bool more) {
    J.dl = dl_server;
    snprintf(J.dl_id, sizeof(J.dl_id), "%s", L->id);
    J.dl_start = more ? L->n : 0;
    J.dl_count = L->filter[0] ? 200 : DL_PAGE;
    J.dl_gen = L->gen;
    L->loading = start_job(JOB_DL_BROWSE);
    L->failed = !L->loading;
}

static void dl_play(dl_level_t * L, int start, bool shuffle) {
    int np = 0;
    for (int i = 0; i < L->n; i++) np += dl_playable(&L->objs[i]);
    if (!np) { show_info_toast(TR("Nothing here can be streamed")); return; }
    if (np > 500) np = 500;
    remote_track_meta_t * t = calloc((size_t) np, sizeof(*t));
    if (!t) return;
    int k = 0;
    const char * artist = "";
    for (int i = 0; i < L->n && k < np; i++) {
        const dlna_object_t * o = &L->objs[i];
        if (!dl_playable(o)) continue;
        remote_track_meta_t * m = &t[k++];
        snprintf(m->provider, sizeof(m->provider), "dlna");
        snprintf(m->track_id, sizeof(m->track_id), "%08x%08x", hash32(home_dl.location), hash32(o->id));
        snprintf(m->title, sizeof(m->title), "%s", o->title);
        snprintf(m->artist, sizeof(m->artist), "%s", o->artist);
        snprintf(m->album, sizeof(m->album), "%s", o->album);
        m->duration_ms = o->duration_ms;
        snprintf(m->artwork_url, sizeof(m->artwork_url), "%s", o->art_url[0] ? o->art_url : L->art);
        snprintf(m->codec, sizeof(m->codec), "%s", dlna_codec_name(o->codec));
        m->sample_rate = o->sample_rate;
        m->bit_depth = o->bits;
        m->channels = o->channels;
        snprintf(m->stream_url, sizeof(m->stream_url), "%s", o->res_url);
        m->verify_tls = false;
        if (!artist[0]) artist = o->artist;
    }
    if (shuffle) {
        remote_track_meta_t * tmp = malloc(sizeof(*tmp));
        for (int i = k - 1; tmp && i > 0; i--) {
            int j = rand() % (i + 1);
            *tmp = t[i];
            t[i] = t[j];
            t[j] = *tmp;
        }
        free(tmp);
        start = 0;
    }
    if (start < 0 || start >= k) start = 0;
    if (!shuffle) {
        dl_continue_t c = { 0 };
        snprintf(c.location, sizeof(c.location), "%s", home_dl.location);
        snprintf(c.id, sizeof(c.id), "%s", L->id);
        snprintf(c.filter, sizeof(c.filter), "%s", L->filter);
        snprintf(c.title, sizeof(c.title), "%s", L->name);
        snprintf(c.artist, sizeof(c.artist), "%s", artist);
        snprintf(c.art, sizeof(c.art), "%s", L->art);
        c.index = start;
        dl_continue_save(&c);
    }
    gui_plugin_play_remote_tracks(t, k, start);
    free(t);
}

static dl_level_t * dl_level_of(lv_event_t * e) {
    lv_obj_t * scr = lv_obj_get_screen(lv_event_get_target_obj(e));
    for (int i = 0; i < DL_MAX_LEVELS; i++)
        if (dl_levels[i] && dl_levels[i]->scr == scr) return dl_levels[i];
    return NULL;
}

static void dl_item_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dl_level_t * L = dl_level_of(e);
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (!L || i < 0 || i >= L->n) return;
    const dlna_object_t * o = &L->objs[i];
    if (o->is_container) { dl_open_level(L, o->id, o->title, "", -1); return; }
    if (!dl_playable(o)) { show_info_toast(TR("This format can't be streamed")); return; }
    int pi = 0;
    for (int k = 0; k < i; k++) pi += dl_playable(&L->objs[k]);
    dl_play(L, pi, false);
}

static void dl_play_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dl_level_t * L = dl_level_of(e);
    if (L) dl_play(L, 0, lv_event_get_user_data(e) != NULL);
}

static void dl_more_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dl_level_t * L = dl_level_of(e);
    if (L && !L->loading) {
        dl_level_load(L, true);
        lv_async_call((lv_async_cb_t) dl_render_level, L); /* not from inside the button's own event */
    }
}

static lv_obj_t * dl_track_row(lv_obj_t * parent, const dlna_object_t * o, int number, int index) {
    bool ok = dl_playable(o);
    lv_obj_t * row = box(parent, lv_pct(100), BOARD_UI_PX(52), C_CARD, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER), 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, dl_item_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
    char num[12];
    snprintf(num, sizeof(num), "%d", number);
    lv_obj_t * n = text(row, num, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), BOARD_UI_PX(24));
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, BOARD_UI_PX(4), 0);
    lv_obj_t * t = text(row, o->title, GUI_FONT_ROLE_BODY, ok ? C_TEXT : lv_color_hex(GUI_COLOR_TEXT_FAINT), BOARD_UI_PX(226));
    lv_obj_set_pos(t, BOARD_UI_PX(40), BOARD_UI_PX(6));
    char fmt[64];
    dl_format(o, fmt, sizeof(fmt));
    lv_obj_t * f = text(row, fmt, GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(226));
    lv_obj_set_pos(f, BOARD_UI_PX(40), BOARD_UI_PX(28));
    if (o->duration_ms) {
        char d[16];
        unsigned s = o->duration_ms / 1000;
        if (s >= 3600) snprintf(d, sizeof(d), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
        else snprintf(d, sizeof(d), "%u:%02u", s / 60, s % 60);
        lv_obj_t * dl = text(row, d, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
        lv_obj_align(dl, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-12), 0);
    }
    return row;
}

static void dl_render_level(dl_level_t * L) {
    bool alive = false;
    for (int i = 0; i < DL_MAX_LEVELS; i++) alive |= dl_levels[i] == L;
    if (!alive) return;
    covers_forget(OWNER_DL + L->slot);
    lv_obj_clean(L->content);
    lv_obj_t * crumb = text(L->content, L->crumb, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), BOARD_UI_PX(330));
    lv_obj_set_style_pad_left(crumb, BOARD_UI_PX(4), 0);
    if (L->n == 0) {
        if (L->loading) loading(L->content, TR("Loading…"));
        else if (L->failed) {
            loading(L->content, TR("Couldn't load this folder"));
            try_again_row(L->content, dl_more_cb);
        } else {
            loading(L->content, TR("This folder is empty"));
        }
        return;
    }
    int containers = 0, albums = 0, items = 0, playable = 0;
    const dlna_object_t * first_item = NULL;
    for (int i = 0; i < L->n; i++) {
        const dlna_object_t * o = &L->objs[i];
        if (o->is_container) { containers++; albums += o->is_album || o->art_url[0]; }
        else { items++; playable += dl_playable(o); if (!first_item) first_item = o; }
        if (!L->art[0] && !o->is_container && o->art_url[0]) snprintf(L->art, sizeof(L->art), "%s", o->art_url);
    }
    /* An album: cover, what it is, Play and Shuffle. */
    if (items) {
        lv_obj_t * head = box(L->content, lv_pct(100), BOARD_UI_PX(80), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_t * c = cover_image_ex(head, L->art, true, BOARD_UI_PX(72), OWNER_DL + L->slot);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
        const char * artist = first_item && first_item->artist[0] ? first_item->artist : "";
        lv_obj_t * a = text(head, artist, GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(150));
        lv_obj_set_pos(a, BOARD_UI_PX(84), BOARD_UI_PX(12));
        char sub[96], fmt[64];
        dl_format(first_item, fmt, sizeof(fmt));
        if (first_item->codec == DLNA_CODEC_NONE) fmt[0] = '\0';
        snprintf(sub, sizeof(sub), items == 1 ? TR("%d track%s%s") : TR("%d tracks%s%s"), items, fmt[0] ? " · " : "", fmt);
        lv_obj_t * s = text(head, sub, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), BOARD_UI_PX(150));
        lv_obj_set_pos(s, BOARD_UI_PX(84), BOARD_UI_PX(34));
        if (playable) {
            lv_obj_t * sh = box(head, BOARD_UI_PX(40), BOARD_UI_PX(40), C_CARD, LV_RADIUS_CIRCLE);
            lv_obj_align(sh, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-54), 0);
            lv_obj_add_flag(sh, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(sh, dl_play_cb, LV_EVENT_CLICKED, (void *) 1);
            lv_obj_center(text(sh, LV_SYMBOL_SHUFFLE, GUI_FONT_ROLE_SUBTEXT, C_TEXT, 0));
            lv_obj_t * pl = box(head, BOARD_UI_PX(46), BOARD_UI_PX(46), accent_lv_color(), LV_RADIUS_CIRCLE);
            lv_obj_align(pl, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_add_flag(pl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pl, dl_play_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_center(text(pl, LV_SYMBOL_PLAY, GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_ON_ACCENT), 0));
        }
    }
    /* Folders: a cover grid when they are albums, rows otherwise. */
    if (containers) {
        bool grid = albums * 2 >= containers;
        lv_obj_t * wrap;
        if (grid) {
            wrap = lv_obj_create(L->content);
            lv_obj_remove_style_all(wrap);
            lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_column(wrap, BOARD_UI_PX(12), 0);
            lv_obj_set_style_pad_row(wrap, BOARD_UI_PX(12), 0);
            lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        } else {
            wrap = plain_group(L->content);
        }
        int left = containers;
        for (int i = 0; i < L->n; i++) {
            const dlna_object_t * o = &L->objs[i];
            if (!o->is_container) continue;
            left--;
            if (grid) {
                lv_obj_t * card = lv_obj_create(wrap);
                lv_obj_remove_style_all(card);
                lv_obj_set_size(card, GRID_COVER, GRID_COVER + BOARD_UI_PX(42));
                lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_event_cb(card, dl_item_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
                cover_image_ex(card, o->art_url, true, GRID_COVER, OWNER_DL + L->slot);
                lv_obj_t * t = text(card, o->title, GUI_FONT_ROLE_BODY, C_TEXT, GRID_COVER);
                lv_obj_set_pos(t, 0, GRID_COVER + BOARD_UI_PX(4));
                lv_obj_t * a = text(card, o->artist, GUI_FONT_ROLE_SUBTEXT, C_MUTED, GRID_COVER);
                lv_obj_set_pos(a, 0, GRID_COVER + BOARD_UI_PX(24));
            } else {
                const char * sym;
                uint32_t color;
                dl_symbol_for(o->title, &sym, &color);
                if (!strcmp(sym, LV_SYMBOL_DIRECTORY) && color == 0xA3A89F) color = 0x39BD95;
                char count[16] = "";
                if (o->child_count >= 0) snprintf(count, sizeof(count), "%d", o->child_count);
                lv_obj_t * r = set_row(wrap, sym, color, o->title, count, dl_item_cb, i, left == 0);
                (void) r;
            }
        }
    }
    if (items) {
        lv_obj_t * g = plain_group(L->content);
        int number = 0;
        for (int i = 0; i < L->n; i++) {
            const dlna_object_t * o = &L->objs[i];
            if (o->is_container) continue;
            number++;
            dl_track_row(g, o, number, i);
        }
    }
    if (L->loading) {
        loading(L->content, TR("Loading…"));
    } else if (L->n < L->total && !L->filter[0]) {
        lv_obj_t * more = box(L->content, lv_pct(100), BOARD_UI_PX(44), C_CARD, BOARD_UI_PX(22));
        lv_obj_add_flag(more, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(more, dl_more_cb, LV_EVENT_CLICKED, NULL);
        char m[64];
        snprintf(m, sizeof(m), TR("Show more (%d left)"), L->total - L->n);
        lv_obj_center(text(more, m, GUI_FONT_ROLE_BODY, C_TEXT, 0));
    }
}

static int dl_track_cmp(const void * a, const void * b) {
    const dlna_object_t * x = a, * y = b;
    return (x->track_no >= 0 ? x->track_no : 99999) - (y->track_no >= 0 ? y->track_no : 99999);
}

static void dl_apply_browse(void) {
    dl_level_t * L = dl_level_by_gen(J.dl_gen);
    if (!L) return;
    L->loading = false;
    L->failed = !J.ok;
    if (!J.ok) {
        show_error_toast(J.error[0] ? J.error : TR("Couldn't load this folder"));
    } else if (J.obj_n > 0) {
        if (L->filter[0]) {
            /* One album out of a "Recently added" track list. */
            int k = 0;
            for (int i = 0; i < J.obj_n; i++)
                if (!J.objs[i].is_container && !strcmp(J.objs[i].album, L->filter)) J.objs[k++] = J.objs[i];
            /* MiniDLNA's "Recently Added" reports every track number one
             * too low, so track 1 arrives without one: one unnumbered track
             * and the rest numbered 1..k-1 means it is the first. */
            int missing = -1, unnumbered = 0, max = 0;
            for (int i = 0; i < k; i++) {
                if (J.objs[i].track_no < 0) { unnumbered++; missing = i; }
                else if (J.objs[i].track_no > max) max = J.objs[i].track_no;
            }
            if (unnumbered == 1 && k > 1 && max == k - 1) J.objs[missing].track_no = 0;
            qsort(J.objs, (size_t) k, sizeof(J.objs[0]), dl_track_cmp);
            J.obj_n = J.obj_total = k;
        }
        dlna_object_t * grown = realloc(L->objs, sizeof(dlna_object_t) * (size_t) (L->n + J.obj_n));
        if (grown) {
            memcpy(grown + L->n, J.objs, sizeof(dlna_object_t) * (size_t) J.obj_n);
            L->objs = grown;
            L->n += J.obj_n;
        }
        L->total = L->filter[0] ? L->n : J.obj_total;
    } else {
        L->total = L->n;
    }
    dl_render_level(L);
    if (L->autoplay >= 0 && L->n > 0) {
        int a = L->autoplay;
        L->autoplay = -1;
        dl_play(L, a, false);
    }
}

/* ---- home ---- */

static void dl_shelf_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= home_dl_recent_n) return;
    const dlna_object_t * o = &home_dl_recent[i];
    if (o->is_container) dl_open_level(NULL, o->id, o->title, "", -1);
    else dl_open_level(NULL, home_dl_recent_id, o->album[0] ? o->album : o->title, o->album, -1);
}

static void dl_top_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < home_dl_top_n) dl_open_level(NULL, home_dl_top[i].id, home_dl_top[i].title, "", -1);
}

static void dl_continue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool play = lv_event_get_user_data(e) != NULL;
    dl_open_level(NULL, home_dl_cont.id, home_dl_cont.title, home_dl_cont.filter, play ? home_dl_cont.index : -1);
}

static void dl_retry_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) refresh_home();
}

static void dl_render_home(void) {
    bool busy = atomic_load(&job_running) == JOB_DL_HOME;
    /* Recently added: one card per album. */
    int shelf_n = 0;
    for (int i = 0; i < home_dl_recent_n && shelf_n < 12; i++) {
        const dlna_object_t * o = &home_dl_recent[i];
        bool seen = false;
        for (int k = 0; k < i && !seen && !o->is_container; k++)
            seen = !home_dl_recent[k].is_container && !strcmp(home_dl_recent[k].album, o->album);
        if (seen) continue;
        if (shelf_n == 0) {
            lv_obj_t * head = lv_obj_create(home_content);
            lv_obj_remove_style_all(head);
            lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_t * h = text(head, TR("Recently added"), GUI_FONT_ROLE_SUBTEXT, C_TEXT, 0);
            lv_obj_align(h, LV_ALIGN_LEFT_MID, BOARD_UI_PX(4), 0);
            lv_obj_t * shelf = lv_obj_create(home_content);
            lv_obj_remove_style_all(shelf);
            lv_obj_set_size(shelf, lv_pct(100), SHELF_COVER + BOARD_UI_PX(44));
            lv_obj_set_flex_flow(shelf, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(shelf, BOARD_UI_PX(10), 0);
            lv_obj_set_scroll_dir(shelf, LV_DIR_HOR);
            lv_obj_set_scrollbar_mode(shelf, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(shelf, LV_OBJ_FLAG_GESTURE_BUBBLE);
            register_swipe_dead_zone(shelf);
            home_shelf = shelf;
        }
        lv_obj_t * card = lv_obj_create(home_shelf);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, SHELF_COVER, SHELF_COVER + BOARD_UI_PX(40));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, dl_shelf_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        cover_image_ex(card, o->art_url, true, SHELF_COVER, OWNER_HOME);
        lv_obj_t * t = text(card, o->is_container || !o->album[0] ? o->title : o->album, GUI_FONT_ROLE_SUBTEXT, C_TEXT,
                            SHELF_COVER);
        lv_obj_set_pos(t, 0, SHELF_COVER + BOARD_UI_PX(4));
        lv_obj_t * a = text(card, o->artist, GUI_FONT_ROLE_SUBTEXT, C_MUTED, SHELF_COVER);
        lv_obj_set_pos(a, 0, SHELF_COVER + BOARD_UI_PX(22));
        shelf_n++;
    }
    /* The server's folders. */
    int folders = 0;
    for (int i = 0; i < home_dl_top_n; i++) folders += home_dl_top[i].is_container;
    if (folders) {
        lv_obj_t * g = plain_group(home_content);
        int left = folders;
        for (int i = 0; i < home_dl_top_n; i++) {
            if (!home_dl_top[i].is_container) continue;
            left--;
            const char * sym;
            uint32_t color;
            dl_symbol_for(home_dl_top[i].title, &sym, &color);
            char count[16] = "";
            if (home_dl_top[i].child_count >= 0) snprintf(count, sizeof(count), "%d", home_dl_top[i].child_count);
            set_row(g, sym, color, home_dl_top[i].title, count, dl_top_cb, i, left == 0);
        }
    } else if (busy) {
        loading(home_content, TR("Loading…"));
    } else if (!dl_server_ready) {
        loading(home_content, wifi_ok() ? TR("Couldn't reach the server") : TR("Connect to Wi-Fi to browse."));
        try_again_row(home_content, dl_retry_cb);
    } else {
        loading(home_content, TR("This server shares no music folders"));
    }
    /* Continue */
    if (home_dl_cont_ok) {
        lv_obj_t * card = box(home_content, lv_pct(100), BOARD_UI_PX(64), C_PANEL, BOARD_UI_PX(14));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, dl_continue_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * c = cover_image_ex(card, home_dl_cont.art, true, ROW_COVER, OWNER_HOME);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, BOARD_UI_PX(10), 0);
        lv_obj_t * k = text(card, TR("Continue"), GUI_FONT_ROLE_SUBTEXT, accent_lv_color(), 0);
        lv_obj_set_pos(k, BOARD_UI_PX(64), BOARD_UI_PX(4));
        lv_obj_t * t = text(card, home_dl_cont.title, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(200));
        lv_obj_set_pos(t, BOARD_UI_PX(64), BOARD_UI_PX(22));
        char sub[180];
        snprintf(sub, sizeof(sub), TR("%s · track %d"), home_dl_cont.artist, home_dl_cont.index + 1);
        lv_obj_t * s = text(card, sub, GUI_FONT_ROLE_SUBTEXT, C_MUTED, BOARD_UI_PX(200));
        lv_obj_set_pos(s, BOARD_UI_PX(64), BOARD_UI_PX(42));
        lv_obj_t * play = box(card, BOARD_UI_PX(40), BOARD_UI_PX(40), accent_lv_color(), LV_RADIUS_CIRCLE);
        lv_obj_align(play, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-10), 0);
        lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(play, dl_continue_cb, LV_EVENT_CLICKED, (void *) 1);
        lv_obj_center(text(play, LV_SYMBOL_PLAY, GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_ON_ACCENT), 0));
    }
}

static void dl_refresh_home(void) {
    free(home_dl_top);
    free(home_dl_recent);
    home_dl_top = home_dl_recent = NULL;
    home_dl_top_n = home_dl_recent_n = 0;
    dl_server_ready = false;
    lv_label_set_text(home_name, home_dl.name);
    home_dl_cont_ok = dl_continue_load(&home_dl_cont) && !strcmp(home_dl_cont.location, home_dl.location);
    if (!wifi_ok()) {
        set_status(TR("Wi-Fi not connected"), lv_color_hex(GUI_COLOR_TEXT_DIM));
        render_home();
        return;
    }
    set_status(TR("DLNA · Connecting…"), accent_lv_color());
    memset(&J.dl, 0, sizeof(J.dl));
    snprintf(J.dl.location, sizeof(J.dl.location), "%s", home_dl.location);
    snprintf(J.dl.udn, sizeof(J.dl.udn), "%s", home_dl.udn);
    start_job(JOB_DL_HOME);
    render_home();
}

static void dl_apply_home(void) {
    if (J.ok) {
        dl_server = J.dl;
        dl_server_ready = true;
        if (J.dl_moved) {
            /* Same device, new address: keep the saved entry and Continue pointing at it. */
            dl_saved_t s = home_dl;
            snprintf(s.location, sizeof(s.location), "%s", J.dl.location);
            dl_upsert(&s, home_dl.location);
            dl_set_active(s.location);
            if (home_dl_cont_ok) {
                snprintf(home_dl_cont.location, sizeof(home_dl_cont.location), "%s", s.location);
                dl_continue_save(&home_dl_cont);
            }
            home_dl = s;
        }
        home_dl_top = J.top;
        home_dl_top_n = J.top_n;
        home_dl_recent = J.recent;
        home_dl_recent_n = J.recent_n;
        snprintf(home_dl_recent_id, sizeof(home_dl_recent_id), "%s", J.recent_id);
        J.top = J.recent = NULL;
        J.top_n = J.recent_n = 0;
        set_status(TR("DLNA · Online"), lv_color_hex(GUI_COLOR_SUCCESS));
    } else {
        char msg[200];
        snprintf(msg, sizeof(msg), "%s · %s", TR("Offline"), J.error);
        set_status(msg, lv_color_hex(GUI_COLOR_ERROR));
    }
    render_home();
}

/* ---- Add a server: type, discovery, address ---- */

static lv_obj_t * type_screen, * disc_screen, * disc_content;
static dlna_server_t * disc_found;
static int disc_found_n;
static bool disc_failed;
static char disc_error[160];
static void render_discovery(void);

static void dl_added(const dlna_server_t * d) {
    dl_saved_t s = { 0 };
    dl_saved_t old;
    snprintf(s.location, sizeof(s.location), "%s", d->location);
    snprintf(s.udn, sizeof(s.udn), "%s", d->udn);
    snprintf(s.name, sizeof(s.name), "%s", d->name);
    snprintf(s.model, sizeof(s.model), "%s", d->model);
    if (dl_find(d->location, &old) && old.name[0]) snprintf(s.name, sizeof(s.name), "%s", old.name);
    dl_upsert(&s, NULL);
    dl_set_active(s.location);
    char msg[160];
    snprintf(msg, sizeof(msg), TR("Added %s"), s.name);
    show_info_toast(msg);
}

static void disc_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= disc_found_n) return;
    dl_added(&disc_found[i]);
    nav_pop();
}

static void disc_search(void) {
    free(disc_found);
    disc_found = NULL;
    disc_found_n = 0;
    disc_failed = false;
    disc_error[0] = '\0';
    if (!wifi_ok()) {
        disc_failed = true;
        snprintf(disc_error, sizeof(disc_error), "%s", TR("Connect to Wi-Fi first"));
        return;
    }
    if (!start_job(JOB_DL_DISCOVER)) disc_failed = true;
}

static void disc_again_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || atomic_load(&job_running) != JOB_NONE) return;
    disc_search();
    lv_async_call((lv_async_cb_t) render_discovery, NULL);
}

static void disc_address_done(const char * value, void * user) {
    (void) user;
    if (!value || !value[0]) return;
    memset(&J.dl, 0, sizeof(J.dl));
    if (strstr(value, "://")) snprintf(J.dl.location, sizeof(J.dl.location), "%s", value);
    else snprintf(J.dl.location, sizeof(J.dl.location), "http://%s", value);
    J.dl_purpose = 0;
    if (start_job(JOB_DL_DESCRIBE)) {
        disc_failed = false;
        snprintf(disc_error, sizeof(disc_error), "%s", TR("Checking the address…"));
        render_discovery();
    }
}

/* "192.168.2." from the player's own Wi-Fi address, so only the last part is typed. */
static void local_prefix(char * out, size_t size) {
    out[0] = '\0';
    struct ifaddrs * ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs * i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, ip, sizeof(ip));
        char * last = strrchr(ip, '.');
        if (last) { last[1] = '\0'; snprintf(out, size, "%s", ip); }
        break;
    }
    freeifaddrs(ifs);
}

static void disc_address_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Digits and dots only: the usual server ports are tried for a bare address. */
    char prefix[24];
    local_prefix(prefix, sizeof(prefix));
    show_text_entry(TR("Server IP address"), prefix, false, true, sizeof(J.dl.location) - strlen("http://"),
                    disc_address_done, NULL);
}

static void render_discovery(void) {
    if (!disc_content) return;
    lv_obj_clean(disc_content);
    bool busy = atomic_load(&job_running) == JOB_DL_DISCOVER;
    const char * status = busy ? TR("Searching your network…")
                          : disc_error[0] ? disc_error
                          : disc_found_n ? TR("Tap a server to add it.")
                                         : TR("No media servers answered.");
    lv_obj_t * st = text(disc_content, status, GUI_FONT_ROLE_SUBTEXT, disc_failed ? lv_color_hex(GUI_COLOR_ERROR) : C_MUTED, 0);
    lv_obj_set_width(st, lv_pct(100));
    lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_left(st, BOARD_UI_PX(4), 0);
    if (disc_found_n) {
        dl_saved_t saved[DL_MAX_SAVED];
        int ns = dl_load(saved);
        for (int i = 0; i < disc_found_n; i++) {
            char host[96], sub[240];
            host_of(disc_found[i].location, host, sizeof(host));
            bool added = false;
            for (int k = 0; k < ns && !added; k++)
                added = !strcmp(saved[k].location, disc_found[i].location) ||
                        (disc_found[i].udn[0] && !strcmp(saved[k].udn, disc_found[i].udn));
            snprintf(sub, sizeof(sub), "%s%s%s · %s", added ? TR("Added") : "", added ? " · " : "", host,
                     disc_found[i].model);
            lv_obj_t * row = list_row(disc_content, NULL, disc_found[i].name, sub, disc_pick_cb, i);
            lv_obj_set_style_bg_color(row, C_CARD, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
        }
    }
    if (!busy) {
        lv_obj_t * again = box(disc_content, lv_pct(100), BOARD_UI_PX(44), C_CARD, BOARD_UI_PX(22));
        lv_obj_add_flag(again, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(again, disc_again_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(text(again, TR("Search again"), GUI_FONT_ROLE_BODY, C_TEXT, 0));
    }
    lv_obj_t * addr = box(disc_content, lv_pct(100), BOARD_UI_PX(44), lv_color_black(), BOARD_UI_PX(14));
    lv_obj_set_style_bg_opa(addr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(addr, BOARD_UI_PX(2), 0);
    lv_obj_set_style_border_color(addr, lv_color_hex(0x3A3F3C), 0);
    lv_obj_add_flag(addr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(addr, disc_address_cb, LV_EVENT_CLICKED, NULL);
    char add[96];
    snprintf(add, sizeof(add), LV_SYMBOL_PLUS "  %s", TR("Enter address instead"));
    lv_obj_center(text(addr, add, GUI_FONT_ROLE_BODY, C_MUTED, 0));
}

static void open_discovery(void) {
    if (!disc_screen) disc_screen = new_screen(TR("DLNA servers"), NULL, &disc_content);
    nav_push(disc_screen);
    disc_search();
    render_discovery();
}

static void type_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool dlna = lv_event_get_user_data(e) != NULL;
    nav_pop_stack_only(); /* Back from the next screen returns to Settings */
    if (dlna) open_discovery();
    else open_server_form(-1);
}

static void type_card(lv_obj_t * parent, const char * symbol, uint32_t color, const char * title, const char * body,
                      bool dlna) {
    lv_obj_t * c = box(parent, lv_pct(100), LV_SIZE_CONTENT, C_CARD, BOARD_UI_PX(16));
    lv_obj_set_style_pad_all(c, BOARD_UI_PX(14), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, type_pick_cb, LV_EVENT_CLICKED, dlna ? (void *) 1 : NULL);
    lv_obj_t * badge = box(c, BOARD_UI_PX(40), BOARD_UI_PX(40), C_PANEL, BOARD_UI_PX(12));
    lv_obj_center(text(badge, symbol, GUI_FONT_ROLE_BODY, lv_color_hex(color), 0));
    lv_obj_set_pos(badge, 0, 0);
    lv_obj_t * t = text(c, title, GUI_FONT_ROLE_BODY, C_TEXT, 0);
    lv_obj_set_pos(t, BOARD_UI_PX(54), 0);
    lv_obj_t * b = text(c, body, GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_set_width(b, BOARD_UI_PX(250));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(b, BOARD_UI_PX(54), BOARD_UI_PX(24));
}

static void open_add_type(void) {
    if (!type_screen) {
        lv_obj_t * content;
        type_screen = new_screen(TR("Add a server"), NULL, &content);
        lv_obj_t * q = text(content, TR("What kind of server?"), GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
        lv_obj_set_style_pad_left(q, BOARD_UI_PX(4), 0);
        type_card(content, LV_SYMBOL_WIFI, 0x48ADE5, TR("Subsonic server"),
                  TR("Navidrome, Gonic, Airsonic. Login, search, random mix."), false);
        type_card(content, LV_SYMBOL_DRIVE, 0x39BD95, TR("DLNA media server"),
                  TR("Built into most NAS boxes, Jellyfin, Plex. Found automatically, no login, home network only."),
                  true);
        lv_obj_t * hint = text(content, TR("Not sure? If your NAS has \"Media Server\" or \"DLNA\" turned on, pick DLNA."),
                               GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), 0);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_left(hint, BOARD_UI_PX(4), 0);
    }
    nav_push(type_screen);
}

/* ---- edit a DLNA server ---- */

static lv_obj_t * dlf_screen, * dlf_content, * dlf_result;
static struct {
    dl_saved_t s;
    char original[512];
    bool confirm_remove;
    int tested; /* 1 ok, -1 failed, 0 not yet */
    char result[200];
} DF;
static void render_dlna_form(void);
static void dlf_render_async(void * p) { (void) p; render_dlna_form(); }

static void dlf_text_done(const char * value, void * user) {
    if (!value) return;
    if (user) snprintf(DF.s.location, sizeof(DF.s.location), "%s", value);
    else snprintf(DF.s.name, sizeof(DF.s.name), "%s", value);
    if (user) DF.tested = 0;
    render_dlna_form();
}

static void dlf_field_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool addr = lv_event_get_user_data(e) != NULL;
    show_text_entry(addr ? TR("Description address") : TR("Name"), addr ? DF.s.location : DF.s.name, false, false,
                    addr ? sizeof(DF.s.location) : sizeof(DF.s.name), dlf_text_done, addr ? (void *) 1 : NULL);
}

static void dlf_test_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_ok()) { show_error_toast(TR("Connect to Wi-Fi first")); return; }
    memset(&J.dl, 0, sizeof(J.dl));
    snprintf(J.dl.location, sizeof(J.dl.location), "%s", DF.s.location);
    J.dl_purpose = 1;
    if (start_job(JOB_DL_DESCRIBE)) {
        lv_label_set_text(dlf_result, TR("Testing…"));
        lv_obj_set_style_text_color(dlf_result, C_MUTED, 0);
    }
}

static void dlf_save_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!DF.s.name[0]) host_of(DF.s.location, DF.s.name, sizeof(DF.s.name));
    char active[600];
    bool was_active = dl_active_location(active, sizeof(active)) && !strcmp(active, DF.original);
    dl_upsert(&DF.s, DF.original);
    if (was_active) dl_set_active(DF.s.location);
    show_info_toast(TR("Server saved"));
    nav_pop();
}

static void dlf_remove_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!DF.confirm_remove) {
        DF.confirm_remove = true;
        lv_async_call(dlf_render_async, NULL);
        return;
    }
    char active[600];
    if (dl_active_location(active, sizeof(active)) && !strcmp(active, DF.original)) dl_set_active(NULL);
    dl_remove(DF.original);
    show_info_toast(TR("Server removed"));
    nav_pop();
}

static void render_dlna_form(void) {
    if (!dlf_content) return;
    lv_obj_clean(dlf_content);
    text(dlf_content, TR("Name"), GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    lv_obj_t * f = box(dlf_content, lv_pct(100), BOARD_UI_PX(38), C_CARD, BOARD_UI_PX(10));
    lv_obj_set_style_bg_color(f, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, dlf_field_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(text(f, DF.s.name, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(300)), LV_ALIGN_LEFT_MID, BOARD_UI_PX(12), 0);
    text(dlf_content, TR("Address"), GUI_FONT_ROLE_SUBTEXT, C_MUTED, 0);
    f = box(dlf_content, lv_pct(100), BOARD_UI_PX(38), C_CARD, BOARD_UI_PX(10));
    lv_obj_set_style_bg_color(f, lv_color_hex(GUI_COLOR_CONTROL), LV_STATE_PRESSED);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, dlf_field_cb, LV_EVENT_CLICKED, (void *) 1);
    lv_obj_align(text(f, DF.s.location, GUI_FONT_ROLE_BODY, C_TEXT, BOARD_UI_PX(300)), LV_ALIGN_LEFT_MID, BOARD_UI_PX(12), 0);
    char info[220];
    snprintf(info, sizeof(info), "%s%s%s", TR("DLNA media server"), DF.s.model[0] ? " · " : "", DF.s.model);
    lv_obj_t * i = text(dlf_content, info, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_FAINT), 0);
    lv_obj_set_width(i, lv_pct(100));
    lv_label_set_long_mode(i, LV_LABEL_LONG_WRAP);
    dlf_result = text(dlf_content, DF.result, GUI_FONT_ROLE_SUBTEXT,
                      DF.tested > 0 ? lv_color_hex(GUI_COLOR_SUCCESS) : DF.tested < 0 ? lv_color_hex(GUI_COLOR_ERROR) : C_MUTED, 0);
    lv_obj_set_width(dlf_result, lv_pct(100));
    lv_label_set_long_mode(dlf_result, LV_LABEL_LONG_WRAP);
    if (!DF.tested) lv_label_set_text(dlf_result, "");
    lv_obj_t * row = lv_obj_create(dlf_content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(52));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BOARD_UI_PX(8), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    pill(row, TR("Test"), false, C_TEXT, dlf_test_cb);
    pill(row, DF.confirm_remove ? TR("Sure?") : TR("Remove"), false, lv_color_hex(GUI_COLOR_ERROR), dlf_remove_cb);
    pill(row, TR("Save"), true, C_TEXT, dlf_save_cb);
}

static void open_dlna_form(const dl_saved_t * s) {
    memset(&DF, 0, sizeof(DF));
    DF.s = *s;
    snprintf(DF.original, sizeof(DF.original), "%s", s->location);
    if (!dlf_screen) {
        lv_obj_t * title;
        dlf_screen = new_screen(TR("Edit server"), &title, &dlf_content);
        lv_obj_set_style_pad_row(dlf_content, BOARD_UI_PX(4), 0);
    }
    render_dlna_form();
    nav_push(dlf_screen);
}

static void dl_apply_describe(void) {
    if (J.dl_purpose == 1) {
        DF.tested = J.ok ? 1 : -1;
        if (J.ok) {
            snprintf(DF.result, sizeof(DF.result), LV_SYMBOL_OK "  %s · %s", TR("Connected"), J.dl.name);
            snprintf(DF.s.udn, sizeof(DF.s.udn), "%s", J.dl.udn);
            snprintf(DF.s.model, sizeof(DF.s.model), "%s", J.dl.model);
            snprintf(DF.s.location, sizeof(DF.s.location), "%s", J.dl.location); /* the full description URL */
        } else {
            snprintf(DF.result, sizeof(DF.result), "%s: %s", TR("Could not connect"), J.error);
        }
        if (dlf_screen) render_dlna_form();
        return;
    }
    if (J.ok) {
        dl_added(&J.dl);
        disc_error[0] = '\0';
        /* The keyboard may still be sliding away: test the stack, not the active screen. */
        if (disc_screen && gui_navigation_is_top(disc_screen)) nav_pop();
        else render_discovery();
    } else {
        disc_failed = true;
        snprintf(disc_error, sizeof(disc_error), "%s", J.error);
        render_discovery();
    }
}

static void dl_apply_job(int done) {
    switch (done) {
        case JOB_DL_HOME: dl_apply_home(); break;
        case JOB_DL_BROWSE: dl_apply_browse(); break;
        case JOB_DL_DISCOVER:
            disc_found = J.found;
            disc_found_n = J.found_n;
            J.found = NULL;
            J.found_n = 0;
            if (!J.ok) {
                disc_failed = false; /* "nobody answered" is not an error in red */
                snprintf(disc_error, sizeof(disc_error), "%s", J.error);
            }
            render_discovery();
            break;
        case JOB_DL_DESCRIBE: dl_apply_describe(); break;
    }
}

static void dl_teardown(void) {
    for (int i = 0; i < DL_MAX_LEVELS; i++) {
        dl_level_t * L = dl_levels[i];
        if (!L) continue;
        if (L->scr) lv_obj_delete(L->scr);
        free(L->objs);
        free(L);
        dl_levels[i] = NULL;
    }
    lv_obj_t ** screens[] = { &type_screen, &disc_screen, &dlf_screen };
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++)
        if (*screens[i]) { lv_obj_delete(*screens[i]); *screens[i] = NULL; }
    disc_content = dlf_content = dlf_result = NULL;
    home_search = NULL;
}

/* ---- polling ----------------------------------------------------------------- */

static void poll_cb(lv_timer_t * t) {
    (void) t;
    covers_apply();
    int done = atomic_exchange(&job_done, JOB_NONE);
    if (done == JOB_NONE) return;
    http_cancel_token_destroy(&job_cancel);
    atomic_store(&job_running, JOB_NONE);
    switch (done) {
        case JOB_HOME:
            if (J.ok) {
                set_status(TR("Online"), lv_color_hex(GUI_COLOR_SUCCESS));
                free(home_recent);
                home_recent = J.albums;
                home_recent_count = J.album_count;
                J.albums = NULL;
            } else {
                char msg[200];
                snprintf(msg, sizeof(msg), "%s · %s", TR("Offline"), J.error);
                set_status(msg, lv_color_hex(GUI_COLOR_ERROR));
            }
            render_home();
            break;
        case JOB_ALBUMS:
            if (J.ok && J.album_count > 0) {
                subsonic_album_t * grown = realloc(grid_albums, sizeof(subsonic_album_t) * (size_t) (grid_count + J.album_count));
                if (grown) {
                    memcpy(grown + grid_count, J.albums, sizeof(subsonic_album_t) * (size_t) J.album_count);
                    grid_albums = grown;
                    grid_count += J.album_count;
                }
            }
            grid_more = J.ok && J.album_count == PAGE_SIZE && grid_sort != 3;
            grid_failed = !J.ok;
            if (!J.ok) show_error_toast(J.error);
            if (grid_screen) render_grid();
            break;
        case JOB_RANDOM:
            if (J.ok && J.song_count > 0) {
                gui_subsonic_play_songs(J.songs, J.song_count, 0);
                J.songs = NULL;
            } else {
                show_error_toast(J.error[0] ? J.error : TR("No songs"));
            }
            break;
        case JOB_SEARCH:
            search_albums = J.albums;
            search_album_count = J.album_count;
            search_songs = J.songs;
            search_song_count = J.song_count;
            J.albums = NULL;
            J.songs = NULL;
            search_failed = !J.ok;
            if (!J.ok) show_error_toast(J.error);
            if (search_screen) render_search();
            break;
        case JOB_DL_HOME:
        case JOB_DL_BROWSE:
        case JOB_DL_DISCOVER:
        case JOB_DL_DESCRIBE:
            dl_apply_job(done);
            break;
        case JOB_TEST:
            F.tested_ok = J.ok;
            if (form_screen) render_form();
            if (!J.ok && form_result) {
                char msg[200];
                snprintf(msg, sizeof(msg), "%s: %s", TR("Could not connect"), J.error);
                lv_label_set_text(form_result, msg);
                lv_obj_set_style_text_color(form_result, lv_color_hex(GUI_COLOR_ERROR), 0);
            }
            break;
    }
    /* An apply that started the next job already freed these in start_job(). */
    if (atomic_load(&job_running) == JOB_NONE) job_free_results();
}

static void ensure_poll(void) {
    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 150, NULL);
}

void gui_stream_home_open(void) {
    if (!home_screen) build_home();
    ensure_poll();
    home_opening = true; /* refresh_home() below covers what home_loaded_cb() would */
    nav_push(home_screen);
    home_opening = false;
    refresh_home();
}

void gui_stream_home_teardown(void) {
    sheet_close();
    dl_teardown();
    if (home_shelf) { unregister_swipe_dead_zone(home_shelf); home_shelf = NULL; }
    for (int i = 0; i < COVER_SLOTS; i++) cover_slots[i].owner = 0;
    lv_obj_t ** screens[] = { &home_screen, &grid_screen, &search_screen, &set_screen, &form_screen };
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++)
        if (*screens[i]) { lv_obj_delete(*screens[i]); *screens[i] = NULL; }
    home_content = home_name = home_status = home_dot = NULL;
    grid_content = grid_sort_label = search_content = search_title = set_content = NULL;
    form_content = form_title = form_result = NULL;
}
