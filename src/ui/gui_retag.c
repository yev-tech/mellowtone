/* "Fix tags from MusicBrainz" for one album.
 *
 * Flow: a worker looks the album up (musicbrainz.c), fetches the best few
 * releases and scores them against the files (retag_match.c); the user picks
 * a release, reviews one row per field and writes. Each file is written by
 * tag_writer.c (temp file, verify, rename) after its original tag region is
 * saved under <card>/.mellowtone/backup/tag_backup/<session>/. The result screen
 * offers Keep (the backup is deleted) or Undo (tags restored, backup
 * deleted). A session neither kept nor undone stays on the card and is
 * offered again when the album's fix is opened.
 *
 * Workers never touch LVGL; a 200 ms timer picks up their results. */
#include "board_config.h"
#include "storage_paths.h"


#include "gui_retag.h"
#include "gui.h"
#include "gui_library.h"
#include "gui_navigation.h"
#include "gui_player.h"
#include "gui_text_input.h"
#include "screen_builders.h"
#include "metadata_db.h"
#include "metadata.h"
#include "audio.h"
#include "i18n.h"
#include "musicbrainz.h"
#include "retag_match.h"
#include "tag_writer.h"
#include "wifi_status.h"
#include "http_client.h"
#include "cue_parser.h"

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RETAG_MAX_FILES 200
#define RETAG_MAX_CANDIDATES 4
#define RETAG_BACKUP_ROOT MT_TAG_BACKUP_DIR
#define RETAG_MANIFEST "manifest.txt"

extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern lv_color_t accent_lv_color(void);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_muted;

typedef enum {
    STAGE_IDLE,
    STAGE_SEARCHING,
    STAGE_PICK,
    STAGE_REVIEW,
    STAGE_WRITING,
    STAGE_RESULT,
    STAGE_RESTORING,
    STAGE_MESSAGE,
} retag_stage_t;

typedef enum { JOB_NONE, JOB_SEARCH, JOB_WRITE, JOB_RESTORE } job_kind_t;

static struct {
    char album[256];
    char album_artist[128];
    char query_album[256];
    char query_artist[256];
    song_row_t * rows;
    retag_local_t * files;
    int file_count;
    int row_count; /* library rows; file_count counts CUE tracks in cue_mode */

    /* CUE image: one audio file split into tracks by a sheet (beside it or
     * embedded). Each sheet track is one entry of `files` for matching;
     * album fields go to the audio file, titles and artists to the sheet. */
    bool cue_mode;
    cue_source_t cue_src;
    cue_sheet_t cue;
    char cue_path[PATH_MAX]; /* the .cue read (CUE_SOURCE_FILE), else "" */
    char * cue_text;         /* the sheet as UTF-8 */
    char cue_paths[4][1024]; /* every .cue for the file (some rips have two); all get the edit */
    int cue_path_count;
    retag_local_t flac_local; /* the audio file's own tags */

    mb_release_detail_t cand[RETAG_MAX_CANDIDATES];
    retag_match_t match[RETAG_MAX_CANDIDATES];
    int cand_count;
    int selected;
    mb_result_t search_result;

    retag_change_t changes[RETAG_FIELD_COUNT];
    int change_count;
    bool enabled[RETAG_FIELD_COUNT];

    char session[128]; /* RETAG_BACKUP_ROOT/<epoch> */
    char playing[PATH_MAX]; /* left alone: it is open for playback */
    char ** written_paths;
    int written;
    int skipped;
    int failed;
    int in_place;  /* files whose tags were rewritten in place (no full copy) */
    int cue_written; /* the .cue sheet was rewritten (not a library file) */
    bool pending; /* result screen for a session found on the card */

    /* Cover Art Archive front cover, saved as cover.jpg in the album folder
     * (never embedded: no music file is touched for it). */
    bool cover_enabled;
    bool cover_embedded;        /* the first file carries a picture */
    char cover_existing[PATH_MAX + 16]; /* an existing folder image, "" if none */
    int cover_result;           /* 0 not tried, 1 saved, 2 none available, 3 failed */

    retag_stage_t stage;
    bool offer_search_again; /* message screen: also offer Search again */
    char message_title[96];
    char message_text[256];
} R;

static pthread_t job_thread;
/* Cancels the lookup in flight (JOB_SEARCH only). Initialized once, reset
 * only by start_job() while no worker runs. */
static http_cancel_token_t search_cancel;
static bool search_cancel_ready;

/* An open requested while a cancelled lookup was still finishing: it starts
 * once that worker has let go of R (poll_cb). */
static struct {
    bool pending;
    bool cue_image;
    char album[256];       /* album name, or the CUE image's audio path */
    char album_artist[128];
} deferred_open;
static atomic_int job_running; /* job_kind_t while running, JOB_NONE when idle */
static atomic_int job_done;    /* job_kind_t that finished, picked up by the timer */
static atomic_int job_progress;
static int job_total;

static lv_obj_t * screen;
static lv_obj_t * title_label;
static lv_obj_t * content;
static lv_obj_t * footer;
static lv_timer_t * poll_timer;

static void render(void);
static bool render_pending;
static void render_now(void);

/* Every caller is a tap on, or a timer next to, widgets render_now()
 * deletes; rebuilding after the event avoids freeing the object whose
 * event is still being dispatched. */
static void render_async_cb(void * p) {
    (void) p;
    render_pending = false;
    render_now();
}

/* ---- data ---------------------------------------------------------------- */

static void free_candidates(void) {
    for (int i = 0; i < R.cand_count; i++) {
        mb_release_detail_free(&R.cand[i]);
        retag_match_free(&R.match[i]);
    }
    memset(R.cand, 0, sizeof(R.cand));
    memset(R.match, 0, sizeof(R.match));
    R.cand_count = 0;
    R.selected = 0;
}

static void free_written(void) {
    for (int i = 0; i < R.written; i++) free(R.written_paths[i]);
    free(R.written_paths);
    R.written_paths = NULL;
    R.written = 0;
}

static void free_files(void) {
    free(R.rows);
    free(R.files);
    R.rows = NULL;
    R.files = NULL;
    R.file_count = R.row_count = 0;
    cue_sheet_free(&R.cue);
    free(R.cue_text);
    R.cue_text = NULL;
    R.cue_mode = false;
    R.cue_src = CUE_SOURCE_NONE;
    R.cue_path[0] = '\0';
    R.cue_path_count = 0;
}

/* An album that is one file with a sheet becomes one entry per sheet track. */
static void load_cue_tracks(void) {
    if (R.row_count != 1) return;
    char * text = NULL;
    R.cue_src = cue_find_for_audio(R.rows[0].path, &R.cue, R.cue_path, sizeof(R.cue_path), &text);
    if (R.cue_src == CUE_SOURCE_NONE || R.cue.track_count < 2 || R.cue.track_count > RETAG_MAX_FILES || !text) {
        free(text);
        cue_sheet_free(&R.cue);
        R.cue_src = CUE_SOURCE_NONE;
        return;
    }
    R.cue_text = text;
    R.cue_path_count = R.cue_src == CUE_SOURCE_FILE ? cue_sheets_for_audio(R.rows[0].path, R.cue_paths, 4) : 0;
    R.flac_local = R.files[0];
    const cached_tags_t * t = &R.rows[0].tags;
    for (int i = 0; i < R.cue.track_count; i++) {
        const cue_track_t * c = &R.cue.tracks[i];
        retag_local_t * f = &R.files[i];
        *f = R.flac_local;
        f->title = c->title;
        f->artist = c->performer[0] ? c->performer : R.cue.performer[0] ? R.cue.performer : t->artist;
        f->album = R.cue.title[0] ? R.cue.title : t->album;
        f->track_number = c->number;
        f->disc_number = -1;
        f->duration_s = 0;
    }
    R.file_count = R.cue.track_count;
    R.cue_mode = true;
}

static bool load_album_files(void) {
    free_files();
    R.rows = calloc(RETAG_MAX_FILES, sizeof(song_row_t));
    R.files = calloc(RETAG_MAX_FILES, sizeof(retag_local_t));
    if (!R.rows || !R.files) { free_files(); return false; }
    int n = 0;
    while (n < RETAG_MAX_FILES) {
        int got = metadata_db_get_album_songs(R.album, R.album_artist, n, R.rows + n, RETAG_MAX_FILES - n);
        if (got <= 0) break;
        n += got;
    }
    for (int i = 0; i < n; i++) {
        const cached_tags_t * t = &R.rows[i].tags;
        retag_local_t * f = &R.files[i];
        f->path = R.rows[i].path;
        f->title = t->title;
        f->artist = t->artist;
        f->album = t->album;
        /* The library stores the track artist when the file has no album
         * artist; report that as empty so a missing tag is offered. */
        f->album_artist = strcmp(t->album_artist, t->artist) == 0 ? "" : t->album_artist;
        f->track_number = t->track_number;
        f->disc_number = t->disc_number;
        f->year = t->release_year;
        f->genre = t->genre;
        f->duration_s = 0;
    }
    R.file_count = R.row_count = n;
    load_cue_tracks();
    return n > 0;
}

/* ---- cover art ------------------------------------------------------------ */

#define COVER_NAME "cover.jpg"

static void album_dir(char * out, size_t size) {
    out[0] = '\0';
    if (R.file_count <= 0) return;
    snprintf(out, size, "%s", R.files[0].path);
    char * slash = strrchr(out, '/');
    if (slash) *slash = '\0';
}

/* An image the library already uses for this folder (albumart.c names). */
static void find_folder_cover(void) {
    static const char * const names[] = { "cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "front.png" };
    char dir[PATH_MAX], path[PATH_MAX + 16];
    album_dir(dir, sizeof(dir));
    R.cover_existing[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (access(path, F_OK) == 0) {
            snprintf(R.cover_existing, sizeof(R.cover_existing), "%s", path);
            return;
        }
    }
}

static bool copy_file(const char * from, const char * to) {
    FILE * in = fopen(from, "rb");
    if (!in) return false;
    FILE * out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[16384];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) ok = ok && fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    return ok;
}

/* Downloads the release's front cover (500 px) to the album folder as
 * cover.jpg via a temp file. An existing cover.jpg is copied into the
 * session first so Undo can put it back. Records the step in the manifest. */
static void save_cover(FILE * manifest) {
    char dir[PATH_MAX], dest[PATH_MAX + 16], tmp[PATH_MAX + 32], url[160];
    album_dir(dir, sizeof(dir));
    snprintf(dest, sizeof(dest), "%s/" COVER_NAME, dir);
    snprintf(tmp, sizeof(tmp), "%s/." COVER_NAME ".part", dir);
    /* This edition's front cover, else the release group's (the archive's
     * pick across all editions). */
    const mb_release_detail_t * d = &R.cand[R.selected];
    int status = 0;
    snprintf(url, sizeof(url), "https://coverartarchive.org/release/%s/front-500", d->release.id);
    bool ok = http_get_to_file_redirects(url, true, tmp, 4u * 1024u * 1024u, NULL, NULL, 10000, 15000, NULL, 4, &status);
    if ((!ok || status != 200) && d->release_group_id[0]) {
        unlink(tmp);
        snprintf(url, sizeof(url), "https://coverartarchive.org/release-group/%s/front-500", d->release_group_id);
        ok = http_get_to_file_redirects(url, true, tmp, 4u * 1024u * 1024u, NULL, NULL, 10000, 15000, NULL, 4, &status);
    }
    if (!ok || status != 200) {
        unlink(tmp);
        R.cover_result = (status == 404) ? 2 : 3;
        return;
    }
    char saved[sizeof(R.session) + 16] = "-";
    if (access(dest, F_OK) == 0) {
        snprintf(saved, sizeof(saved), "%s/cover.prev", R.session);
        if (!copy_file(dest, saved)) { unlink(tmp); R.cover_result = 3; return; }
    }
    if (rename(tmp, dest) != 0) { unlink(tmp); R.cover_result = 3; return; }
    fprintf(manifest, "cover\t%s\t%s\n", dest, saved);
    fflush(manifest);
    fsync(fileno(manifest));
    R.cover_result = 1;
}

/* Undo for the cover step: remove the downloaded file or put the old one back. */
static void undo_cover(const char * session) {
    char mpath[sizeof(R.session) + 32];
    snprintf(mpath, sizeof(mpath), "%s/" RETAG_MANIFEST, session);
    FILE * f = fopen(mpath, "r");
    if (!f) return;
    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cover\t", 6) != 0) continue;
        line[strcspn(line, "\r\n")] = '\0';
        char * dest = line + 6;
        char * saved = strchr(dest, '\t');
        if (!saved) continue;
        *saved++ = '\0';
        if (strcmp(saved, "-") == 0) unlink(dest);
        else if (copy_file(saved, dest)) unlink(saved);
    }
    fclose(f);
}

/* ---- backup sessions ----------------------------------------------------- */

static bool remove_session(const char * dir) {
    DIR * d = opendir(dir);
    if (!d) return errno == ENOENT;
    struct dirent * e;
    char path[PATH_MAX + 512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    return rmdir(dir) == 0;
}

/* Original modification time of the entry being handed to the callback. */
static long long manifest_mtime = -1;

/* Calls cb(path, backup) for each manifest entry; returns entries read. */
static int read_manifest(const char * dir, void (*cb)(const char * path, const char * backup, void * ctx), void * ctx) {
    char mpath[PATH_MAX + 32];
    snprintf(mpath, sizeof(mpath), "%s/" RETAG_MANIFEST, dir);
    FILE * f = fopen(mpath, "r");
    if (!f) return -1;
    char line[PATH_MAX * 2];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char * tab = strchr(line, '\t');
        if (!tab || line[0] != '/') continue; /* header lines are key=value */
        *tab = '\0';
        char * mtime = strchr(tab + 1, '\t');
        if (mtime) *mtime++ = '\0';
        manifest_mtime = mtime ? atoll(mtime) : -1;
        if (cb) cb(line, tab + 1, ctx);
        n++;
    }
    fclose(f);
    return n;
}

typedef struct { bool found; } pending_probe_t;

static void pending_probe_cb(const char * path, const char * backup, void * ctx) {
    (void) backup;
    pending_probe_t * p = ctx;
    for (int i = 0; i < R.row_count && !p->found; i++)
        if (strcmp(R.rows[i].path, path) == 0) p->found = true;
    for (int i = 0; i < R.cue_path_count && !p->found; i++)
        if (strcmp(R.cue_paths[i], path) == 0) p->found = true;
}

/* A session on the card that covers one of this album's files. */
static bool find_pending_session(void) {
    DIR * d = opendir(RETAG_BACKUP_ROOT);
    if (!d) return false;
    struct dirent * e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 32) continue;
        char dir[sizeof(R.session)];
        snprintf(dir, sizeof(dir), RETAG_BACKUP_ROOT "/%s", e->d_name);
        pending_probe_t probe = { false };
        if (read_manifest(dir, pending_probe_cb, &probe) > 0 && probe.found) {
            snprintf(R.session, sizeof(R.session), "%s", dir);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* ---- workers --------------------------------------------------------------- */

/* Equal fits prefer the editions people usually own: CD, then digital. */
static int format_rank(const char * format) {
    if (strcmp(format, "CD") == 0) return 0;
    if (strncmp(format, "Digital", 7) == 0) return 1;
    if (strstr(format, "CD")) return 2;
    return 3;
}

static int cmp_candidates(const void * a, const void * b) {
    const int ia = *(const int *) a, ib = *(const int *) b;
    if (R.match[ib].score != R.match[ia].score) return R.match[ib].score - R.match[ia].score;
    return format_rank(R.cand[ia].release.format) - format_rank(R.cand[ib].release.format);
}

static void * search_job(void * arg) {
    (void) arg;
    if (R.cue_mode) {
        /* Track lengths from the sheet's start times; the last runs to the end. */
        audio_current_format_info_t probe;
        double total = audio_probe_file_format(R.rows[0].path, &probe) ? probe.duration_seconds : 0;
        for (int i = 0; i < R.file_count; i++) {
            double end = i + 1 < R.cue.track_count ? R.cue.tracks[i + 1].start_seconds : total;
            R.files[i].duration_s = end > R.cue.tracks[i].start_seconds ? end - R.cue.tracks[i].start_seconds : 0;
        }
        track_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        metadata_read_without_artwork(R.rows[0].path, &meta);
        if (!meta.has_album_artist) R.rows[0].tags.album_artist[0] = '\0';
        R.flac_local.album_artist = R.rows[0].tags.album_artist;
        for (int i = 0; i < R.file_count; i++) R.files[i].album_artist = R.rows[0].tags.album_artist;
        free(meta.picture_data);
        free(meta.lyrics);
        atomic_store(&job_progress, R.file_count);
    }
    for (int i = 0; i < R.file_count && !R.cue_mode; i++) {
        if (http_cancel_token_is_cancelled(&search_cancel)) break;
        audio_current_format_info_t probe;
        if (audio_probe_file_format(R.files[i].path, &probe)) R.files[i].duration_s = probe.duration_seconds;
        /* The library may file a song under an album artist it does not
         * carry (compilation grouping, or the track-artist fallback); the
         * review compares against what is really in the file. */
        track_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        metadata_read_without_artwork(R.files[i].path, &meta);
        if (!meta.has_album_artist) R.rows[i].tags.album_artist[0] = '\0';
        R.files[i].album_artist = R.rows[i].tags.album_artist;
        free(meta.picture_data);
        free(meta.lyrics);
        atomic_store(&job_progress, i + 1);
    }
    R.cover_embedded = false;
    if (R.file_count > 0) {
        track_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        metadata_read(R.files[0].path, &meta);
        R.cover_embedded = meta.picture_size > 0;
        free(meta.picture_data);
        free(meta.lyrics);
    }
    mb_release_t found[8];
    int count = 0;
    mb_result_t r = MB_CANCELLED;
    if (!http_cancel_token_is_cancelled(&search_cancel))
        r = mb_search_releases(R.query_album, R.query_artist[0] ? R.query_artist : NULL, R.file_count,
                               found, 8, &count, &search_cancel);
    if (r == MB_OK && count == 0 && R.query_artist[0])
        r = mb_search_releases(R.query_album, NULL, R.file_count, found, 8, &count, &search_cancel);
    R.search_result = r;
    if (r == MB_OK) {
        mb_release_detail_t details[RETAG_MAX_CANDIDATES];
        retag_match_t matches[RETAG_MAX_CANDIDATES];
        int n = 0;
        for (int i = 0; i < count && n < RETAG_MAX_CANDIDATES; i++) {
            memset(&details[n], 0, sizeof(details[n]));
            mb_result_t got = mb_get_release(found[i].id, &details[n], &search_cancel);
            if (got == MB_CANCELLED) {
                R.search_result = MB_CANCELLED;
                break;
            }
            if (got != MB_OK) continue;
            memset(&matches[n], 0, sizeof(matches[n]));
            retag_match_score(R.files, R.file_count, &details[n], &matches[n]);
            n++;
        }
        int order[RETAG_MAX_CANDIDATES];
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 0; i < n; i++) { R.cand[i] = details[i]; R.match[i] = matches[i]; }
        R.cand_count = n;
        qsort(order, (size_t) n, sizeof(int), cmp_candidates);
        for (int i = 0; i < n; i++) { R.cand[i] = details[order[i]]; R.match[i] = matches[order[i]]; }
        if (R.search_result == MB_CANCELLED) {
            /* Nobody will look at these: free them here. */
            for (int i = 0; i < n; i++) {
                mb_release_detail_free(&R.cand[i]);
                retag_match_free(&R.match[i]);
            }
            memset(R.cand, 0, sizeof(R.cand));
            memset(R.match, 0, sizeof(R.match));
            R.cand_count = 0;
        } else if (n == 0) {
            R.search_result = MB_NOT_FOUND;
        }
    }
    atomic_store(&job_done, JOB_SEARCH);
    atomic_store(&job_running, JOB_NONE);
    return NULL;
}

/* ---- CUE images ------------------------------------------------------------------ */

/* What a fix of a CUE image writes: the album fields for the audio file
 * (plus the sheet itself when it is embedded) and one edit applied to
 * every .cue of the file. The struct holds the edit's storage: build it in
 * place and free it with cue_plan_free(). */
typedef struct {
    retag_kv_t kv[28];
    int kv_n;
    cue_edit_t edit;
    bool edit_any;
    const char ** titles, ** perfs;
    char year[8];
    char * embedded_text; /* the new embedded sheet (malloc'd), NULL when unchanged or not embedded */
} cue_plan_t;

static void cue_plan_free(cue_plan_t * p) {
    free(p->titles);
    free(p->perfs);
    free(p->embedded_text);
    memset(p, 0, sizeof(*p));
}

static void cue_plan_build(cue_plan_t * p, char * buf, size_t buf_size) {
    memset(p, 0, sizeof(*p));
    const mb_release_detail_t * d = &R.cand[R.selected];
    const retag_match_t * m = &R.match[R.selected];
    int n = R.cue.track_count;
    p->titles = calloc((size_t) n, sizeof(char *));
    p->perfs = calloc((size_t) n, sizeof(char *));
    bool any = false;
    for (int i = 0; p->titles && p->perfs && i < n; i++) {
        int t = m->track_for_file ? m->track_for_file[i] : -1;
        if (t < 0) continue;
        const mb_track_t * mt = &d->tracks[t];
        if (R.enabled[RETAG_FIELD_TITLE] && mt->title[0] && strcmp(mt->title, R.cue.tracks[i].title) != 0) {
            p->titles[i] = mt->title;
            any = true;
        }
        /* A per-track PERFORMER only where the track artist is not the album
         * artist, or where the sheet already names one. */
        if (R.enabled[RETAG_FIELD_ARTIST] && mt->artist[0] &&
            (strcmp(mt->artist, d->release.artist) != 0 || R.cue.tracks[i].performer[0]) &&
            strcmp(mt->artist, R.cue.tracks[i].performer) != 0) {
            p->perfs[i] = mt->artist;
            any = true;
        }
    }
    snprintf(p->year, sizeof(p->year), "%.4s", d->release.date);
    cue_edit_t * e = &p->edit;
    e->title = R.enabled[RETAG_FIELD_ALBUM] && strcmp(d->release.title, R.cue.title) ? d->release.title : NULL;
    e->performer = R.enabled[RETAG_FIELD_ALBUMARTIST] && strcmp(d->release.artist, R.cue.performer) ? d->release.artist : NULL;
    e->date = R.enabled[RETAG_FIELD_DATE] && p->year[0] && strcmp(p->year, R.cue.date) ? p->year : NULL;
    e->genre = R.enabled[RETAG_FIELD_GENRE] && d->genre[0] && strcmp(d->genre, R.cue.genre) ? d->genre : NULL;
    e->track_titles = p->titles;
    e->track_performers = p->perfs;
    e->track_count = n;
    p->edit_any = any || e->title || e->performer || e->date || e->genre;
    int k = retag_fields_for_file(&R.flac_local, d, NULL, R.enabled, buf, buf_size, p->kv, 24);
    p->kv_n = k > 0 ? k : 0;
    /* An untagged image also gets ARTIST and TITLE (the album's), so the
     * library lists it as the album rather than by file name. */
    if (R.enabled[RETAG_FIELD_ALBUMARTIST] && !R.flac_local.artist[0] && d->release.artist[0]) {
        p->kv[p->kv_n].key = "ARTIST";
        p->kv[p->kv_n++].value = d->release.artist;
    }
    if (R.enabled[RETAG_FIELD_ALBUM] && !R.flac_local.title[0] && d->release.title[0]) {
        p->kv[p->kv_n].key = "TITLE";
        p->kv[p->kv_n++].value = d->release.title;
    }
    if (p->edit_any && R.cue_src != CUE_SOURCE_FILE && R.cue_text) {
        char * t = cue_rewrite(R.cue_text, e);
        if (t && strcmp(t, R.cue_text) != 0) {
            p->embedded_text = t;
            p->kv[p->kv_n].key = "CUESHEET";
            p->kv[p->kv_n].value = t;
            p->kv_n++;
        } else {
            free(t);
        }
    }
}

/* The edited text of one .cue (malloc'd), or NULL when it would not change.
 * *was_utf8 tells whether the file on the card already was UTF-8. */
static char * cue_file_edited(const char * path, const cue_edit_t * e, bool * was_utf8) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * raw = malloc(256 * 1024 + 1);
    size_t n = raw ? fread(raw, 1, 256 * 1024 + 1, f) : 0;
    fclose(f);
    if (!raw || n > 256 * 1024) { free(raw); return NULL; }
    bool converted = false;
    char * text = cue_text_to_utf8(raw, n, &converted);
    free(raw);
    if (!text) return NULL;
    char * out = cue_rewrite(text, e);
    bool same = out && strcmp(out, text) == 0 && !converted;
    free(text);
    if (same) { free(out); return NULL; }
    if (was_utf8) *was_utf8 = !converted;
    return out;
}

static int cue_files_changing(const cue_plan_t * p) {
    int n = 0;
    for (int i = 0; p->edit_any && i < R.cue_path_count; i++) {
        char * t = cue_file_edited(R.cue_paths[i], &p->edit, NULL);
        n += t != NULL;
        free(t);
    }
    return n;
}

/* The new sheet over a .cue (temp file, fsync, rename); its original is
 * copied into the session first. Sheets that were not UTF-8 (Windows-1251 /
 * -1252) are written as UTF-8 with a BOM so other players read them right. */
static bool write_cue_file(const char * path, int index, const char * text, bool was_utf8, FILE * manifest) {
    char backup[sizeof(R.session) + 32], tmp[PATH_MAX + 16];
    snprintf(backup, sizeof(backup), "%s/sheet%d.copy", R.session, index);
    struct stat before;
    bool have_before = stat(path, &before) == 0;
    if (!copy_file(path, backup)) return false;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';
    snprintf(tmp, sizeof(tmp), "%s/.retag.cue.tmp", dir);
    FILE * f = fopen(tmp, "wb");
    if (!f) { unlink(backup); return false; }
    bool ok = true;
    if (!was_utf8 && strncmp(text, "\xEF\xBB\xBF", 3) != 0) ok = fwrite("\xEF\xBB\xBF", 1, 3, f) == 3;
    size_t len = strlen(text);
    ok = ok && fwrite(text, 1, len, f) == len;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        unlink(backup);
        return false;
    }
    fprintf(manifest, "%s\t%s\t%lld\n", path, backup, have_before ? (long long) before.st_mtime : -1LL);
    fflush(manifest);
    fsync(fileno(manifest));
    return true;
}

static void write_cue_image(FILE * manifest) {
    static char buf[16384];
    cue_plan_t p;
    cue_plan_build(&p, buf, sizeof(buf));
    for (int i = 0; p.edit_any && i < R.cue_path_count; i++) {
        bool was_utf8 = true;
        char * t = cue_file_edited(R.cue_paths[i], &p.edit, &was_utf8);
        if (!t) continue;
        if (write_cue_file(R.cue_paths[i], i, t, was_utf8, manifest)) R.cue_written++;
        else R.failed++;
        free(t);
    }
    const char * path = R.rows[0].path;
    if (p.kv_n > 0) {
        if (!tag_writer_supports(path) || strcmp(path, R.playing) == 0) {
            R.skipped++;
        } else {
            char backup[sizeof(R.session) + 32];
            snprintf(backup, sizeof(backup), "%s/000.bak", R.session);
            struct stat before;
            bool have_before = stat(path, &before) == 0;
            tag_write_mode_t mode = TAG_WRITE_MODE_NONE;
            tag_write_result_t r = tag_writer_apply_ex(path, (const tag_field_t *) p.kv, p.kv_n, backup, &mode);
            if (r == TAG_WRITE_OK && mode == TAG_WRITE_MODE_NONE) {
                unlink(backup);
            } else if (r == TAG_WRITE_OK) {
                if (mode == TAG_WRITE_MODE_IN_PLACE) R.in_place++;
                fprintf(manifest, "%s\t%s\t%lld\n", path, backup, have_before ? (long long) before.st_mtime : -1LL);
                fflush(manifest);
                fsync(fileno(manifest));
                if (R.written_paths) R.written_paths[R.written++] = strdup(path);
            } else {
                R.failed++;
                unlink(backup);
            }
        }
    }
    cue_plan_free(&p);
}

static bool write_manifest_header(FILE * f) {
    const mb_release_detail_t * d = &R.cand[R.selected];
    return fprintf(f, "album=%s\nalbum_artist=%s\nrelease=%s\n", R.album, R.album_artist, d->release.id) > 0;
}

static void * write_job(void * arg) {
    (void) arg;
    const mb_release_detail_t * d = &R.cand[R.selected];
    const retag_match_t * m = &R.match[R.selected];
    char mpath[sizeof(R.session) + 32];
    snprintf(mpath, sizeof(mpath), "%s/" RETAG_MANIFEST, R.session);
    FILE * manifest = fopen(mpath, "w");
    if (!manifest || !write_manifest_header(manifest)) {
        if (manifest) fclose(manifest);
        R.failed = R.file_count;
        goto done;
    }
    R.written_paths = calloc((size_t) R.file_count, sizeof(char *));
    static char buf[16384];
    if (R.cue_mode) write_cue_image(manifest);
    for (int i = 0; i < R.file_count && !R.cue_mode; i++) {
        atomic_store(&job_progress, i);
        int t = m->track_for_file ? m->track_for_file[i] : -1;
        if (t < 0) continue;
        if (!tag_writer_supports(R.files[i].path) || strcmp(R.files[i].path, R.playing) == 0) {
            R.skipped++;
            continue;
        }
        retag_kv_t kv[24];
        int n = retag_fields_for_file(&R.files[i], d, &d->tracks[t], R.enabled, buf, sizeof(buf), kv, 24);
        if (n <= 0) continue;
        char backup[sizeof(R.session) + 32];
        snprintf(backup, sizeof(backup), "%s/%03d.bak", R.session, i);
        struct stat before;
        bool have_before = stat(R.files[i].path, &before) == 0;
        tag_write_mode_t mode = TAG_WRITE_MODE_NONE;
        tag_write_result_t r = tag_writer_apply_ex(R.files[i].path, (const tag_field_t *) kv, n, backup, &mode);
        if (r == TAG_WRITE_OK && mode == TAG_WRITE_MODE_NONE) {
            /* Already had these values: nothing written, no backup made. */
            unlink(backup);
            continue;
        }
        if (mode == TAG_WRITE_MODE_IN_PLACE) R.in_place++;
        if (r == TAG_WRITE_OK) {
            /* Recorded only once written, so Undo restores exactly these,
             * modification time included. */
            fprintf(manifest, "%s\t%s\t%lld\n", R.files[i].path, backup,
                    have_before ? (long long) before.st_mtime : -1LL);
            fflush(manifest);
            fsync(fileno(manifest));
            if (R.written_paths) R.written_paths[R.written++] = strdup(R.files[i].path);
        } else if (r == TAG_WRITE_UNSUPPORTED) {
            R.skipped++;
            unlink(backup);
        } else {
            R.failed++;
            unlink(backup);
        }
    }
    R.cover_result = 0;
    if (R.cover_enabled) save_cover(manifest);
    fclose(manifest);
done:
    atomic_store(&job_progress, R.file_count);
    atomic_store(&job_done, JOB_WRITE);
    atomic_store(&job_running, JOB_NONE);
    return NULL;
}

typedef struct { int ok, failed; char ** paths; int count; } restore_ctx_t;

static void restore_cb(const char * path, const char * backup, void * vctx) {
    restore_ctx_t * c = vctx;
    size_t bl = strlen(backup);
    bool whole = bl > 5 && strcmp(backup + bl - 5, ".copy") == 0; /* a .cue sheet, copied whole */
    if (whole ? copy_file(backup, path) : tag_writer_restore(path, backup) == TAG_WRITE_OK) {
        c->ok++;
        if (manifest_mtime >= 0) {
            struct timespec times[2] = { { 0, UTIME_OMIT }, { (time_t) manifest_mtime, 0 } };
            utimensat(AT_FDCWD, path, times, 0);
        }
    } else {
        c->failed++;
    }
    atomic_store(&job_progress, c->ok + c->failed);
    if (whole) return; /* not a library file */
    char ** grown = realloc(c->paths, sizeof(char *) * (size_t) (c->count + 1));
    if (grown) { c->paths = grown; c->paths[c->count++] = strdup(path); }
}

static restore_ctx_t restore_result;

static void * restore_job(void * arg) {
    (void) arg;
    memset(&restore_result, 0, sizeof(restore_result));
    read_manifest(R.session, restore_cb, &restore_result);
    undo_cover(R.session);
    atomic_store(&job_done, JOB_RESTORE);
    atomic_store(&job_running, JOB_NONE);
    return NULL;
}

static bool start_job(job_kind_t kind, void * (*fn)(void *)) {
    if (atomic_load(&job_running) != JOB_NONE) return false;
    if (!search_cancel_ready) {
        http_cancel_token_init(&search_cancel);
        search_cancel_ready = true;
    } else {
        /* No worker holds the token now: clear the last cancel. */
        pthread_mutex_lock(&search_cancel.mutex);
        search_cancel.cancel_requested = false;
        search_cancel.fd = -1;
        pthread_mutex_unlock(&search_cancel.mutex);
    }
    atomic_store(&job_progress, 0);
    atomic_store(&job_done, JOB_NONE);
    atomic_store(&job_running, kind);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    bool ok = pthread_create(&job_thread, &attr, fn, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) atomic_store(&job_running, JOB_NONE);
    return ok;
}

/* ---- widgets ------------------------------------------------------------- */

static lv_obj_t * label(lv_obj_t * parent, const char * text, gui_font_role_t role, lv_color_t color) {
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, gui_theme_font(role), 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t * pill_button(const char * text, bool primary, lv_event_cb_t cb) {
    lv_obj_t * b = lv_obj_create(footer);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, BOARD_UI_PX(44));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, BOARD_UI_PX(22), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, primary ? accent_lv_color() : lv_color_hex(GUI_COLOR_CONTROL), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = label(b, text, GUI_FONT_ROLE_BODY, primary ? lv_color_hex(GUI_COLOR_ON_ACCENT) : lv_color_hex(GUI_COLOR_TEXT));
    lv_obj_center(l);
    return b;
}

static void centered_message(const char * title, const char * text) {
    lv_obj_t * box = lv_obj_create(content);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(box, BOARD_UI_PX(60), 0);
    lv_obj_set_style_pad_row(box, BOARD_UI_PX(10), 0);
    lv_obj_t * t = label(box, title, GUI_FONT_ROLE_ROW, lv_color_hex(GUI_COLOR_TEXT));
    lv_obj_set_width(t, lv_pct(90));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if (text && text[0]) {
        lv_obj_t * d = label(box, text, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
        lv_obj_set_width(d, lv_pct(90));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void show_message(const char * title, const char * text) {
    snprintf(R.message_title, sizeof(R.message_title), "%s", title);
    snprintf(R.message_text, sizeof(R.message_text), "%s", text ? text : "");
    R.stage = STAGE_MESSAGE;
    render();
}

/* ---- actions ------------------------------------------------------------- */

/* Stops a lookup in flight; the worker returns soon with MB_CANCELLED. */
static void cancel_search(void) {
    if (atomic_load(&job_running) == JOB_SEARCH && search_cancel_ready) http_cancel_token_cancel(&search_cancel);
}

bool gui_retag_card_idle(void) {
    cancel_search();
    return atomic_load(&job_running) == JOB_NONE;
}

static void close_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (atomic_load(&job_running) == JOB_WRITE || atomic_load(&job_running) == JOB_RESTORE) return;
    cancel_search();
    deferred_open.pending = false;
    nav_pop();
}

static void start_search(void) {
    R.offer_search_again = false;
    free_candidates();
    R.stage = STAGE_SEARCHING;
    render();
    job_total = R.file_count;
    if (!start_job(JOB_SEARCH, search_job)) show_message(TR("Could not start the lookup"), "");
}

static void search_again_done(const char * text, void * user_data) {
    (void) user_data;
    if (!text || !text[0]) return;
    snprintf(R.query_album, sizeof(R.query_album), "%s", text);
    R.query_artist[0] = '\0'; /* a typed title searches all artists */
    start_search();
}

static void search_again_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Album to search"), R.query_album, false, false, sizeof(R.query_album), search_again_done, NULL);
}

static void candidate_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    R.selected = (int) (intptr_t) lv_event_get_user_data(e);
    render();
}

static void review_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || R.cand_count == 0) return;
    R.change_count = retag_summarize(R.files, R.file_count, &R.cand[R.selected], &R.match[R.selected],
                                     R.changes, RETAG_FIELD_COUNT);
    if (R.cue_mode) {
        /* A sheet numbers its tracks by position: nothing to renumber. */
        int k = 0;
        for (int i = 0; i < R.change_count; i++)
            if (R.changes[i].field != RETAG_FIELD_TRACK && R.changes[i].field != RETAG_FIELD_DISC) R.changes[k++] = R.changes[i];
        R.change_count = k;
    }
    memset(R.enabled, 0, sizeof(R.enabled));
    for (int i = 0; i < R.change_count; i++) R.enabled[R.changes[i].field] = R.changes[i].default_on;
    find_folder_cover();
    /* Pre-ticked only when the album shows no cover at all. */
    R.cover_enabled = !R.cover_existing[0] && !R.cover_embedded;
    R.stage = STAGE_REVIEW;
    render();
}

static void cover_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    R.cover_enabled = !R.cover_enabled;
    render();
}

static void change_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int field = (int) (intptr_t) lv_event_get_user_data(e);
    R.enabled[field] = !R.enabled[field];
    render();
}

/* Files that get at least one change with the current switches. */
static int files_to_write(void) {
    if (R.cue_mode) {
        static char cbuf[16384];
        cue_plan_t p;
        cue_plan_build(&p, cbuf, sizeof(cbuf));
        int n = (p.kv_n > 0) + cue_files_changing(&p);
        cue_plan_free(&p);
        return n;
    }
    const mb_release_detail_t * d = &R.cand[R.selected];
    const retag_match_t * m = &R.match[R.selected];
    static char buf[16384];
    int n = 0;
    for (int i = 0; i < R.file_count; i++) {
        int t = m->track_for_file ? m->track_for_file[i] : -1;
        if (t < 0) continue;
        retag_kv_t kv[24];
        if (retag_fields_for_file(&R.files[i], d, &d->tracks[t], R.enabled, buf, sizeof(buf), kv, 24) > 0) n++;
    }
    return n;
}

static void write_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || (files_to_write() == 0 && !R.cover_enabled)) return;
    (void) mt_state_mkdirs(RETAG_BACKUP_ROOT);
    snprintf(R.session, sizeof(R.session), RETAG_BACKUP_ROOT "/%ld", (long) time(NULL));
    if (mkdir(R.session, 0755) != 0) {
        show_error_toast(TR("Cannot create the backup folder on the SD card"));
        return;
    }
    free_written();
    R.skipped = R.failed = R.in_place = R.cue_written = 0;
    R.pending = false;
    /* The playing file is open; replacing it under the decoder on exFAT can
     * make reads fail. It is reported as skipped. */
    const char * now = gui_player_get_current_track_path();
    snprintf(R.playing, sizeof(R.playing), "%s", now && audio_is_playing() ? now : "");
    R.stage = STAGE_WRITING;
    job_total = R.file_count;
    render();
    if (!start_job(JOB_WRITE, write_job)) show_message(TR("Could not start writing"), "");
}

static void back_to_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    R.stage = STAGE_PICK;
    render();
}

static void keep_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Accepted: the backup has done its job. */
    remove_session(R.session);
    R.session[0] = '\0';
    show_info_toast(TR("Changes kept"));
    nav_pop();
}

static void undo_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    R.stage = STAGE_RESTORING;
    job_total = R.written > 0 ? R.written : R.file_count;
    render();
    if (!start_job(JOB_RESTORE, restore_job)) show_message(TR("Could not start the undo"), "");
}

/* ---- screens ------------------------------------------------------------- */

static void render_searching(void) {
    lv_label_set_text(title_label, TR("Fix tags"));
    char text[160];
    /* While a cancelled lookup finishes, R.query_album is still its worker's. */
    snprintf(text, sizeof(text), TR("Looking up \"%s\" on MusicBrainz…"),
             deferred_open.pending && !deferred_open.cue_image ? deferred_open.album : R.query_album);
    centered_message(text, TR("Checking the length of each file, then asking MusicBrainz for matching releases."));
    pill_button(TR("Cancel"), false, close_cb);
}

static void render_pick(void) {
    lv_label_set_text(title_label, TR("Choose the release"));
    lv_obj_t * intro = label(content, "", GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
    if (R.cue_mode)
        lv_label_set_text_fmt(intro, TR("Best match first, scored against the %d tracks of the CUE sheet. Nothing is written yet."),
                              R.file_count);
    else
        lv_label_set_text_fmt(intro, TR("Best match first, scored against your %d files. Nothing is written yet."),
                              R.file_count);
    lv_obj_set_width(intro, lv_pct(100));
    lv_label_set_long_mode(intro, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < R.cand_count; i++) {
        const mb_release_t * r = &R.cand[i].release;
        bool sel = i == R.selected;
        lv_obj_t * row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(60));
        lv_obj_set_style_radius(row, BOARD_UI_PX(12), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_set_style_border_width(row, sel ? 1 : 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, candidate_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * t = label(row, "", GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_TEXT));
        lv_obj_set_width(t, BOARD_UI_PX(250));
        lv_obj_set_height(t, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
        label_set_text_single_line(t, r->title);
        lv_obj_set_pos(t, BOARD_UI_PX(12), BOARD_UI_PX(9));
        char meta[200];
        char year[8] = "";
        if (r->date[0]) snprintf(year, sizeof(year), "%.4s", r->date);
        snprintf(meta, sizeof(meta), "%s%s%s%s%s · %d %s", r->artist, year[0] ? " · " : "", year,
                 r->format[0] ? " · " : "", r->format, r->track_count, TR("tracks"));
        lv_obj_t * s = label(row, "", GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
        lv_obj_set_width(s, BOARD_UI_PX(250));
        lv_obj_set_height(s, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
        label_set_text_single_line(s, meta);
        lv_obj_set_pos(s, BOARD_UI_PX(12), BOARD_UI_PX(33));
        lv_obj_t * badge = lv_obj_create(row);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, BOARD_UI_PX(48), BOARD_UI_PX(24));
        lv_obj_set_style_radius(badge, BOARD_UI_PX(12), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(badge, sel ? lv_color_hex(0x2A2717) : lv_color_hex(GUI_COLOR_CARD), 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-10), 0);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t * bl = label(badge, "", GUI_FONT_ROLE_SUBTEXT, sel ? accent_lv_color() : lv_color_hex(GUI_COLOR_TEXT_SOFT));
        lv_label_set_text_fmt(bl, "%d%%", R.match[i].score);
        lv_obj_center(bl);
    }
    pill_button(TR("Search again"), false, search_again_cb);
    pill_button(TR("Review"), true, review_cb);
}

static const char * field_name(retag_field_t f) {
    switch (f) {
        case RETAG_FIELD_ALBUM: return TR("Album");
        case RETAG_FIELD_ALBUMARTIST: return TR("Album artist");
        case RETAG_FIELD_ARTIST: return TR("Track artists");
        case RETAG_FIELD_TITLE: return TR("Titles");
        case RETAG_FIELD_TRACK: return TR("Track numbers");
        case RETAG_FIELD_DISC: return TR("Disc numbers");
        case RETAG_FIELD_DATE: return TR("Year");
        case RETAG_FIELD_GENRE: return TR("Genre");
        case RETAG_FIELD_MBIDS: return TR("MusicBrainz IDs");
        default: return "";
    }
}

static void render_review(void) {
    lv_label_set_text(title_label, TR("Review changes"));
    const mb_release_t * r = &R.cand[R.selected].release;
    lv_obj_t * card = lv_obj_create(content);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), BOARD_UI_PX(52));
    lv_obj_set_style_radius(card, BOARD_UI_PX(14), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(GUI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * t = label(card, "", GUI_FONT_ROLE_BODY, lv_color_hex(GUI_COLOR_TEXT));
    lv_obj_set_width(t, BOARD_UI_PX(310));
    lv_obj_set_height(t, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
    label_set_text_single_line(t, r->title);
    lv_obj_set_pos(t, BOARD_UI_PX(12), BOARD_UI_PX(6));
    lv_obj_t * s = label(card, "", GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
    /* TRANSLATORS: release date (year), format such as "CD", how well it fits the files. */
    lv_label_set_text_fmt(s, TR("MusicBrainz · %.4s · %s · %d%% match"), r->date, r->format, R.match[R.selected].score);
    lv_obj_set_pos(s, BOARD_UI_PX(12), BOARD_UI_PX(29));

    /* Cover row: saved as cover.jpg in the folder, never embedded. */
    {
        bool on = R.cover_enabled;
        lv_obj_t * row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(50));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, cover_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * box = lv_obj_create(row);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, BOARD_UI_PX(20), BOARD_UI_PX(20));
        lv_obj_set_style_radius(box, BOARD_UI_PX(6), 0);
        lv_obj_set_style_bg_opa(box, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(box, accent_lv_color(), 0);
        lv_obj_set_style_border_width(box, on ? 0 : 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(GUI_COLOR_TEXT_HINT), 0);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, BOARD_UI_PX(8), 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
        if (on) lv_obj_center(label(box, LV_SYMBOL_OK, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_ON_ACCENT)));
        lv_obj_t * name = label(row, TR("Cover · Cover Art Archive, 500 px"), GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
        lv_obj_set_pos(name, BOARD_UI_PX(40), BOARD_UI_PX(5));
        const char * slash = strrchr(R.cover_existing, '/');
        char change[200];
        snprintf(change, sizeof(change), "%.120s  " LV_SYMBOL_RIGHT "  " COVER_NAME,
                 R.cover_existing[0] ? (slash ? slash + 1 : R.cover_existing)
                 : R.cover_embedded ? TR("embedded only") : TR("none"));
        lv_obj_t * v = label(row, "", GUI_FONT_ROLE_BODY, on ? lv_color_hex(GUI_COLOR_TEXT) : lv_color_hex(GUI_COLOR_TEXT_DIM));
        lv_obj_set_width(v, BOARD_UI_PX(290));
        lv_obj_set_height(v, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
        label_set_text_single_line(v, change);
        lv_obj_set_pos(v, BOARD_UI_PX(40), BOARD_UI_PX(24));
    }
    if (R.change_count == 0)
        centered_message(TR("No tag changes"), TR("These files already match the release."));
    for (int i = 0; i < R.change_count; i++) {
        const retag_change_t * c = &R.changes[i];
        bool on = R.enabled[c->field];
        lv_obj_t * row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(50));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, change_toggle_cb, LV_EVENT_CLICKED, (void *) (intptr_t) c->field);
        lv_obj_t * box = lv_obj_create(row);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, BOARD_UI_PX(20), BOARD_UI_PX(20));
        lv_obj_set_style_radius(box, BOARD_UI_PX(6), 0);
        lv_obj_set_style_bg_opa(box, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(box, accent_lv_color(), 0);
        lv_obj_set_style_border_width(box, on ? 0 : 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(GUI_COLOR_TEXT_HINT), 0);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, BOARD_UI_PX(8), 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
        if (on) {
            lv_obj_t * ck = label(box, LV_SYMBOL_OK, GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_ON_ACCENT));
            lv_obj_center(ck);
        }
        lv_obj_t * name = label(row, "", GUI_FONT_ROLE_SUBTEXT, lv_color_hex(GUI_COLOR_TEXT_MUTED));
        lv_label_set_text_fmt(name, "%s · %d %s", field_name(c->field), c->files_changed,
                              R.cue_mode ? TR("tracks") : TR("files"));
        lv_obj_set_pos(name, BOARD_UI_PX(40), BOARD_UI_PX(5));
        char change[300];
        snprintf(change, sizeof(change), "%s  " LV_SYMBOL_RIGHT "  %s", c->old_example[0] ? c->old_example : TR("empty"),
                 c->new_example);
        lv_obj_t * v = label(row, "", GUI_FONT_ROLE_BODY, on ? lv_color_hex(GUI_COLOR_TEXT) : lv_color_hex(GUI_COLOR_TEXT_DIM));
        lv_obj_set_width(v, BOARD_UI_PX(290));
        lv_obj_set_height(v, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
        label_set_text_single_line(v, change);
        lv_obj_set_pos(v, BOARD_UI_PX(40), BOARD_UI_PX(24));
    }
    pill_button(TR("Back"), false, back_to_pick_cb);
    int n = files_to_write();
    char text[48];
    if (n == 0) snprintf(text, sizeof(text), "%s", R.cover_enabled ? TR("Save cover") : TR("Write"));
    else snprintf(text, sizeof(text), n == 1 ? TR("Write 1 file") : TR("Write %d files"), n);
    bool any = n > 0 || R.cover_enabled;
    lv_obj_t * w = pill_button(text, any, write_cb);
    if (!any) lv_obj_set_style_opa(w, LV_OPA_50, 0);
}

static void render_progress(const char * title, const char * text) {
    lv_label_set_text(title_label, TR("Fix tags"));
    centered_message(title, text);
    lv_obj_t * bar = lv_bar_create(content);
    lv_obj_set_size(bar, lv_pct(80), BOARD_UI_PX(6));
    lv_obj_set_style_bg_color(bar, lv_color_hex(GUI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, accent_lv_color(), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, job_total > 0 ? job_total : 1);
    lv_bar_set_value(bar, atomic_load(&job_progress), LV_ANIM_OFF);
    lv_obj_set_style_margin_top(bar, BOARD_UI_PX(24), 0);
}

/* Appends one whole translated sentence to a paragraph. */
static void append_sentence(char * out, size_t size, const char * sentence) {
    size_t len = strlen(out);
    if (len + 1 >= size) return;
    snprintf(out + len, size - len, "%s%s", len ? " " : "", sentence);
}

static void render_result(void) {
    lv_label_set_text(title_label, TR("Fix tags"));
    char title[160], text[640];
    if (R.pending) {
        snprintf(title, sizeof(title), "%s", TR("A tag fix is waiting for your decision"));
        snprintf(text, sizeof(text), "%s", TR("Keep the new tags (the backup is deleted) or undo them (the old tags are written back)."));
    } else {
        int files = R.written + R.cue_written;
        if (files == 0) snprintf(title, sizeof(title), "%s", TR("Cover saved"));
        else snprintf(title, sizeof(title), TR_N("%d file updated", "%d files updated", files), files);
        text[0] = '\0';
        append_sentence(text, sizeof(text), TR("The library re-read them. Old tags are backed up on the SD card until you choose."));
        if (R.cover_result == 1 && files > 0) append_sentence(text, sizeof(text), TR("Cover saved as cover.jpg."));
        else if (R.cover_result == 2) append_sentence(text, sizeof(text), TR("The Cover Art Archive has no front cover for this release."));
        else if (R.cover_result == 3) append_sentence(text, sizeof(text), TR("The cover could not be downloaded."));
        char sentence[256];
        if (R.skipped) {
            snprintf(sentence, sizeof(sentence),
                     TR_N("%d file was skipped (playing now, or format not supported).",
                          "%d files were skipped (playing now, or format not supported).", R.skipped),
                     R.skipped);
            append_sentence(text, sizeof(text), sentence);
        }
        if (R.failed) {
            snprintf(sentence, sizeof(sentence),
                     TR_N("%d file could not be written and was left unchanged.",
                          "%d files could not be written and were left unchanged.", R.failed),
                     R.failed);
            append_sentence(text, sizeof(text), sentence);
        }
    }
    centered_message(title, text);
    pill_button(TR("Undo"), false, undo_cb);
    pill_button(TR("Keep"), true, keep_cb);
}

static void render_message(void) {
    lv_label_set_text(title_label, TR("Fix tags"));
    centered_message(R.message_title, R.message_text);
    pill_button(TR("Close"), false, close_cb);
    if (R.offer_search_again) pill_button(TR("Search again"), true, search_again_cb);
}

static void render_now(void) {
    if (!screen) return;
    lv_obj_clean(content);
    lv_obj_clean(footer);
    switch (R.stage) {
        case STAGE_SEARCHING: render_searching(); break;
        case STAGE_PICK: render_pick(); break;
        case STAGE_REVIEW: render_review(); break;
        case STAGE_WRITING: render_progress(TR("Writing tags…"), TR("Each file is written to a copy, checked, then swapped in.")); break;
        case STAGE_RESTORING: render_progress(TR("Restoring the old tags…"), ""); break;
        case STAGE_RESULT: render_result(); break;
        case STAGE_MESSAGE: render_message(); break;
        default: break;
    }
}

static void render(void) {
    if (render_pending) return;
    render_pending = true;
    lv_async_call(render_async_cb, NULL);
}

static void run_deferred_open(void);

static void poll_cb(lv_timer_t * t) {
    (void) t;
    if (deferred_open.pending) {
        /* The cancelled lookup's results are dropped; start the new one
         * once its worker has finished with R. */
        if (atomic_load(&job_running) != JOB_NONE) return;
        atomic_store(&job_done, JOB_NONE);
        run_deferred_open();
        return;
    }
    int done = atomic_exchange(&job_done, JOB_NONE);
    if (done == JOB_NONE) {
        if (R.stage == STAGE_WRITING || R.stage == STAGE_RESTORING) {
            lv_obj_t * bar = NULL;
            for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++)
                if (lv_obj_check_type(lv_obj_get_child(content, i), &lv_bar_class)) bar = lv_obj_get_child(content, i);
            if (bar) lv_bar_set_value(bar, atomic_load(&job_progress), LV_ANIM_OFF);
        }
        return;
    }
    if (done == JOB_SEARCH) {
        if (R.search_result == MB_CANCELLED) {
            /* Closed while looking up: nothing to show. */
        } else if (R.cand_count > 0) {
            R.stage = STAGE_PICK;
            render();
        } else if (R.search_result == MB_NET_ERROR) {
            show_message(TR("MusicBrainz could not be reached"), TR("Check the Wi-Fi connection and try again."));
        } else if (R.search_result == MB_RATE_LIMITED) {
            show_message(TR("MusicBrainz is busy"), TR("Try again in a minute."));
        } else {
            R.offer_search_again = true;
            show_message(TR("No matching release found"), TR("Try Search again with a shorter album name."));
        }
    } else if (done == JOB_WRITE) {
        if (R.written_paths && R.written > 0)
            start_library_metadata_refresh_paths((const char * const *) R.written_paths, R.written);
        gui_player_forget_cue();
        if (R.written + R.cue_written == 0 && R.cover_result != 1) {
            remove_session(R.session);
            show_message(TR("Nothing was written"), R.failed ? TR("The files could not be written; they were left unchanged.")
                                                              : TR("No file needed a change."));
        } else {
            R.stage = STAGE_RESULT;
            render();
        }
    } else if (done == JOB_RESTORE) {
        gui_player_forget_cue();
        if (restore_result.count > 0)
            start_library_metadata_refresh_paths((const char * const *) restore_result.paths, restore_result.count);
        for (int i = 0; i < restore_result.count; i++) free(restore_result.paths[i]);
        free(restore_result.paths);
        if (restore_result.failed == 0) {
            remove_session(R.session);
            R.session[0] = '\0';
            show_message(TR("Changes undone"), TR("The previous tags are back and the backup was deleted."));
        } else {
            char text[160];
            snprintf(text, sizeof(text), TR("%d files could not be restored; their backup stays on the SD card."),
                     restore_result.failed);
            show_message(TR("Undo incomplete"), text);
        }
    }
}

static void build_screen(void) {
    screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    title_label = build_screen_header(screen, TR("Fix tags"), close_cb, NULL, NULL);
    content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), BOARD_SCREEN_HEIGHT - top - BOARD_UI_PX(64));
    lv_obj_set_pos(content, 0, top);
    lv_obj_set_style_pad_hor(content, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_row(content, BOARD_UI_PX(8), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    footer = lv_obj_create(screen);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), BOARD_UI_PX(64));
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(footer, BOARD_UI_PX(12), 0);
    lv_obj_set_style_pad_column(footer, BOARD_UI_PX(8), 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    finalize_screen_navigation(screen);
}

/* One CUE image by its audio file (Files > a .cue > Fix tags): works even
 * when the image has no tags, where the library would file it under
 * "Unknown Album" together with unrelated files. */
static bool load_single_file(const char * path) {
    free_files();
    R.rows = calloc(RETAG_MAX_FILES, sizeof(song_row_t));
    R.files = calloc(RETAG_MAX_FILES, sizeof(retag_local_t));
    if (!R.rows || !R.files) { free_files(); return false; }
    song_row_t * row = &R.rows[0];
    snprintf(row->path, sizeof(row->path), "%s", path);
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read_without_artwork(path, &meta);
    cached_tags_t * t = &row->tags;
    snprintf(t->title, sizeof(t->title), "%s", meta.has_title ? meta.title : "");
    snprintf(t->artist, sizeof(t->artist), "%s", meta.has_artist ? meta.artist : "");
    snprintf(t->album, sizeof(t->album), "%s", meta.has_album ? meta.album : "");
    snprintf(t->album_artist, sizeof(t->album_artist), "%s", meta.has_album_artist ? meta.album_artist : "");
    snprintf(t->genre, sizeof(t->genre), "%s", meta.has_genre ? meta.genre : "");
    t->track_number = meta.has_track_number ? meta.track_number : -1;
    t->disc_number = meta.has_disc_number ? meta.disc_number : -1;
    t->release_year = meta.has_release_year ? meta.release_year : -1;
    free(meta.picture_data);
    free(meta.lyrics);
    retag_local_t * f = &R.files[0];
    f->path = row->path;
    f->title = t->title;
    f->artist = t->artist;
    f->album = t->album;
    f->album_artist = t->album_artist;
    f->track_number = t->track_number;
    f->disc_number = t->disc_number;
    f->year = t->release_year;
    f->genre = t->genre;
    R.file_count = R.row_count = 1;
    load_cue_tracks();
    return R.cue_mode;
}

static void retag_begin(void);

/* A lookup still running belongs to a closed fix (the screen is pushed
 * only by the two opens below): cancel it and start this open when its
 * worker is done. False when a write or undo is running. */
static bool defer_open_behind_job(bool cue_image, const char * album, const char * album_artist) {
    int running = atomic_load(&job_running);
    if (running == JOB_NONE) return false;
    if (running != JOB_SEARCH) {
        show_info_toast(TR("A tag fix is still running"));
        return true;
    }
    cancel_search();
    deferred_open.pending = true;
    deferred_open.cue_image = cue_image;
    snprintf(deferred_open.album, sizeof(deferred_open.album), "%s", album);
    snprintf(deferred_open.album_artist, sizeof(deferred_open.album_artist), "%s", album_artist ? album_artist : "");
    if (!screen) build_screen();
    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 200, NULL);
    if (!gui_navigation_is_top(screen)) nav_push(screen);
    R.stage = STAGE_SEARCHING;
    render();
    return true;
}

static void open_cue_image(const char * audio_path, bool push);
static void open_album(const char * album, const char * album_artist, bool push);

static void run_deferred_open(void) {
    char album[sizeof(deferred_open.album)], album_artist[sizeof(deferred_open.album_artist)];
    snprintf(album, sizeof(album), "%s", deferred_open.album);
    snprintf(album_artist, sizeof(album_artist), "%s", deferred_open.album_artist);
    bool cue_image = deferred_open.cue_image;
    deferred_open.pending = false;
    if (cue_image) open_cue_image(album, false);
    else open_album(album, album_artist, false);
}

void gui_retag_open_cue_image(const char * audio_path) {
    if (!audio_path || !audio_path[0]) return;
    if (defer_open_behind_job(true, audio_path, NULL)) return;
    open_cue_image(audio_path, true);
}

static void open_cue_image(const char * audio_path, bool push) {
    free_candidates();
    free_written();
    R.offer_search_again = false;
    if (!screen) build_screen();
    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 200, NULL);
    if (push) nav_push(screen);
    if (!load_single_file(audio_path)) {
        show_message(TR("No CUE sheet for this file"), TR("Fixing a CUE image needs a sheet with at least two tracks."));
        return;
    }
    const cached_tags_t * t = &R.rows[0].tags;
    snprintf(R.album, sizeof(R.album), "%s", t->album[0] ? t->album : R.cue.title);
    snprintf(R.album_artist, sizeof(R.album_artist), "%s", t->album_artist[0] ? t->album_artist : R.cue.performer);
    snprintf(R.query_album, sizeof(R.query_album), "%s", R.cue.title[0] ? R.cue.title : R.album);
    snprintf(R.query_artist, sizeof(R.query_artist), "%s", R.cue.performer[0] ? R.cue.performer : R.album_artist);
    if (!R.query_album[0]) {
        /* Nothing to search by: the folder name usually is "Year - Album". */
        char dir[PATH_MAX];
        album_dir(dir, sizeof(dir));
        const char * base = strrchr(dir, '/');
        snprintf(R.query_album, sizeof(R.query_album), "%s", base ? base + 1 : dir);
    }
    retag_begin();
}

void gui_retag_open_album(const char * album, const char * album_artist) {
    if (!album || !album[0]) return;
    if (defer_open_behind_job(false, album, album_artist)) return;
    open_album(album, album_artist, true);
}

static void open_album(const char * album, const char * album_artist, bool push) {
    if (strcmp(album, "Unknown Album") == 0) {
        /* The library's bucket for files without an album tag: not one album. */
        if (!screen) build_screen();
        if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 200, NULL);
        if (push) nav_push(screen);
        show_message(TR("These songs have no album tag"),
                     TR("They are not one album, so they can't be looked up together. For a CUE image, open its .cue "
                        "in Files and choose Fix tags there."));
        return;
    }
    free_candidates();
    free_written();
    R.offer_search_again = false;
    snprintf(R.album, sizeof(R.album), "%s", album);
    snprintf(R.album_artist, sizeof(R.album_artist), "%s", album_artist ? album_artist : "");
    snprintf(R.query_album, sizeof(R.query_album), "%s", album);
    snprintf(R.query_artist, sizeof(R.query_artist), "%s", R.album_artist);
    if (!screen) build_screen();
    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 200, NULL);
    if (push) nav_push(screen);

    if (!load_album_files()) {
        show_message(TR("No songs in this album"), "");
        return;
    }
    if (R.cue_mode && R.cue.title[0]) {
        snprintf(R.query_album, sizeof(R.query_album), "%s", R.cue.title);
        if (!R.query_artist[0]) snprintf(R.query_artist, sizeof(R.query_artist), "%s", R.cue.performer);
    }
    retag_begin();
}

static void retag_begin(void) {
    if (find_pending_session()) {
        R.pending = true;
        R.written = 0;
        R.stage = STAGE_RESULT;
        render();
        return;
    }
    char ssid[64];
    if (!wifi_status_last_ssid(ssid, sizeof(ssid))) {
        show_message(TR("Wi-Fi is not connected"), TR("Fixing tags looks the album up on MusicBrainz."));
        return;
    }
    start_search();
}

void gui_retag_teardown(void) {
    cancel_search();
    deferred_open.pending = false;
    if (poll_timer) { lv_timer_delete(poll_timer); poll_timer = NULL; }
    if (screen) { lv_obj_delete(screen); screen = NULL; }
    title_label = content = footer = NULL;
    /* A running job keeps its data; it is only read again by the next open. */
    if (atomic_load(&job_running) == JOB_NONE) {
        free_candidates();
        free_written();
        free_files();
    }
}
