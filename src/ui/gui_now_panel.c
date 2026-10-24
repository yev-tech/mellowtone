/* Now Playing second panel: a mini header that keeps
 * the track, position and play/pause in reach, over three tabs -- Up next,
 * Lyrics, Info and More (the player's actions). Opened by swiping up on the
 * cover and from the player's ⋮ button, both on Up next; tapping the mini cover (or Back) returns to the player.
 *
 * Up next reads the queue snapshot the Queue screen uses and selects rows
 * through gui_player_queue_select(); Lyrics embeds the existing lyrics view;
 * Info shows the decoder's current format. With no local lyrics the Lyrics
 * tab offers a download from LRCLIB, saved as the track's .lrc sidecar.
 * A 1 s timer refreshes the header
 * and rebuilds Up next only when the queue revision changes. */
#include "board_config.h"

#include "gui_now_panel.h"
#include "gui.h"
#include "gui_player.h"
#include "gui_lyrics.h"
#include "gui_library.h"
#include "gui_navigation.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "audio.h"
#include "i18n.h"
#include "assets.h"
#include "lrclib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

#define UP_NEXT_MAX 40

extern void get_display_names(const char * path, char * out_title, size_t title_sz, char * out_folder, size_t folder_sz);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);

typedef enum { TAB_UP_NEXT, TAB_LYRICS, TAB_INFO, TAB_MORE, TAB_COUNT } panel_tab_t;

static lv_obj_t * panel_screen;
static lv_obj_t * mini_cover;
static lv_obj_t * mini_title;
static lv_obj_t * mini_meta;
static lv_obj_t * mini_play_icon;
static lv_obj_t * tab_buttons[TAB_COUNT];
static lv_obj_t * content;
static lv_timer_t * panel_timer;
static panel_tab_t active_tab = TAB_UP_NEXT;
static uint64_t shown_revision = UINT64_MAX;
static int shown_current = -1;
static bool lyrics_embedded;

static void format_time(double s, char * out, size_t size) {
    if (s <= 0) { snprintf(out, size, "--:--"); return; }
    int t = (int) s;
    snprintf(out, size, "%d:%02d", t / 60, t % 60);
}

static void refresh_header(void) {
    char title[160] = "", artist[160] = "";
    const char * path = gui_player_get_current_track_path();
    song_row_t song;
    if (path && metadata_db_get_song_by_path(path, &song)) {
        snprintf(title, sizeof(title), "%s", song.tags.title);
        snprintf(artist, sizeof(artist), "%s", song.tags.artist);
    }
    if (!title[0] && path) {
        char folder[128];
        get_display_names(path, title, sizeof(title), folder, sizeof(folder));
        if (!artist[0]) snprintf(artist, sizeof(artist), "%s", folder);
    }
    if (!path) snprintf(title, sizeof(title), "%s", TR("Nothing playing"));
    label_set_text_single_line(mini_title, title); /* one line, cut with "…" */

    char pos[16], dur[16], meta[200];
    format_time(audio_get_position_seconds(), pos, sizeof(pos));
    format_time(gui_player_display_duration(), dur, sizeof(dur));
    if (artist[0]) snprintf(meta, sizeof(meta), "%s · %s / %s", artist, pos, dur);
    else snprintf(meta, sizeof(meta), "%s / %s", pos, dur);
    label_set_text_single_line(mini_meta, meta);
    lv_label_set_text(mini_play_icon, audio_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc();
    lv_image_set_src(mini_cover, cover && cover->data ? (const void *) cover
                                                       : (const void *) asset_path("playing_plane/default_cover_565.png"));
}

static void up_next_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_player_queue_select(shown_revision, (int) (intptr_t) lv_event_get_user_data(e)))
        show_error_toast(TR("Queue changed. Try again."));
}

static void build_up_next(void) {
    lv_obj_clean(content);
    int * order = NULL, count = 0, current = -1;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision) || count <= 0) {
        free(order);
        build_list_message(content, TR("Queue is empty"), TR("Play an album or playlist to see what comes next."));
        shown_revision = UINT64_MAX;
        return;
    }
    shown_revision = revision;
    shown_current = current;
    int shown = 0;
    for (int i = current + 1; i < count && shown < UP_NEXT_MAX; i++, shown++) {
        const char * path = gui_player_get_track_path_at(order[i]);
        char title[128] = "", subtitle[256] = "";
        song_row_t song;
        if (path && metadata_db_get_song_by_path(path, &song)) {
            gui_library_format_song_identity(&song, title, sizeof(title), subtitle, sizeof(subtitle));
        } else if (path) {
            char folder[128];
            get_display_names(path, title, sizeof(title), folder, sizeof(folder));
        }
        lv_obj_t * row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(52));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CARD), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, up_next_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * num = lv_label_create(row);
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_add_style(num, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(num, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_width(num, BOARD_UI_PX(30));
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t * t = lv_label_create(row);
        lv_label_set_text(t, title);
        lv_obj_add_style(t, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(t, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_width(t, BOARD_UI_PX(280));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, BOARD_UI_PX(42), BOARD_UI_PX(7));
        lv_obj_t * s = lv_label_create(row);
        lv_label_set_text(s, subtitle);
        lv_obj_add_style(s, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(s, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_width(s, BOARD_UI_PX(280));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, BOARD_UI_PX(42), BOARD_UI_PX(29));
    }
    if (shown == 0)
        build_list_message(content, TR("Last track in the queue"), TR("Nothing plays after this one."));
    free(order);
}

static const char * codec_name(audio_codec_t c) {
    switch (c) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        default: return "-";
    }
}

static void info_line(const char * key, const char * value) {
    lv_obj_t * row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, BOARD_UI_PX(8), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(GUI_COLOR_DIVIDER_SOFT), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_add_style(k, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(k, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_pos(k, BOARD_UI_PX(12), 0);
    lv_obj_t * v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_add_style(v, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(v, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(v, BOARD_UI_PX(200));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(v, BOARD_UI_PX(128), 0);
}

static void build_info(void) {
    lv_obj_clean(content);
    audio_current_format_info_t f;
    char buf[160];
    if (!audio_get_current_format_info(&f) || !f.valid) {
        /* Not decoded yet: what the library knows about the file. */
        const char * path = gui_player_get_current_track_path();
        if (!path) {
            build_list_message(content, TR("Nothing playing"), "");
            return;
        }
        const char * dot = strrchr(path, '.');
        char ext[16] = "-";
        if (dot) {
            size_t n = 0;
            for (const char * p = dot + 1; *p && n + 1 < sizeof(ext); p++) ext[n++] = (char) toupper((unsigned char) *p);
            ext[n] = '\0';
        }
        info_line(TR("Format"), ext);
        song_row_t song;
        if (metadata_db_get_song_by_path(path, &song)) {
            if (song.tags.title[0]) info_line(TR("Title"), song.tags.title);
            if (song.tags.artist[0]) info_line(TR("Artist"), song.tags.artist);
            if (song.tags.album[0]) info_line(TR("Album"), song.tags.album);
            if (song.tags.track_number > 0) { snprintf(buf, sizeof(buf), "%d", song.tags.track_number); info_line(TR("Track"), buf); }
            if (song.tags.release_year > 0) { snprintf(buf, sizeof(buf), "%d", song.tags.release_year); info_line(TR("Year"), buf); }
        }
        const char * slash = strrchr(path, '/');
        info_line(TR("File"), slash ? slash + 1 : path);
        return;
    }
    info_line(TR("Format"), codec_name(f.codec));
    snprintf(buf, sizeof(buf), "%u.%u kHz", f.source_sample_rate / 1000, (f.source_sample_rate % 1000) / 100);
    info_line(TR("Sample rate"), buf);
    if (f.source_bit_depth) { snprintf(buf, sizeof(buf), "%u bit", f.source_bit_depth); info_line(TR("Bit depth"), buf); }
    snprintf(buf, sizeof(buf), "%u", f.channels);
    info_line(TR("Channels"), buf);
    if (f.bitrate_kbps) { snprintf(buf, sizeof(buf), "%u kbps", f.bitrate_kbps); info_line(TR("Bitrate"), buf); }
    format_time(f.duration_seconds, buf, sizeof(buf));
    info_line(TR("Duration"), buf);
    if (f.output_sample_rate && f.output_sample_rate != f.source_sample_rate) {
        snprintf(buf, sizeof(buf), "%u.%u kHz", f.output_sample_rate / 1000, (f.output_sample_rate % 1000) / 100);
        info_line(TR("Output"), buf);
    }
    const char * slash = strrchr(f.path, '/');
    info_line(TR("File"), slash ? slash + 1 : f.path);
}

/* One LRCLIB download at a time, on its own thread; timer_cb() picks up
 * the result. */
typedef struct {
    char path[PATH_MAX];
    char title[256], artist[256], album[256];
    int duration_s;
} lyrics_fetch_req_t;

static atomic_int fetch_state; /* 0 idle, 1 running, 2 finished */
static lrclib_result_t fetch_result;
static char fetch_path[PATH_MAX];
static char fetch_status[160];

static void * lyrics_fetch_thread(void * arg) {
    lyrics_fetch_req_t * req = arg;
    if (!req->artist[0]) {
        /* Not in the library yet (copied since the last scan): the file's
         * own tags, read here off the UI thread, rather than its name. */
        track_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        metadata_read(req->path, &meta);
        if (meta.has_title && meta.title[0]) snprintf(req->title, sizeof(req->title), "%s", meta.title);
        if (meta.has_artist && meta.artist[0]) snprintf(req->artist, sizeof(req->artist), "%s", meta.artist);
        if (meta.has_album && meta.album[0]) snprintf(req->album, sizeof(req->album), "%s", meta.album);
        free(meta.picture_data);
        free(meta.lyrics);
    }
    fetch_result = lrclib_fetch_sidecar(req->path, req->title, req->artist, req->album, req->duration_s);
    free(req);
    atomic_store(&fetch_state, 2);
    return NULL;
}

static bool track_is_local(const char * path) {
    return path && path[0] == '/';
}

static void show_lyrics(void);

/* The player embeds the same lyrics list; only hide it while this panel's
 * content still holds it. */
static void release_lyrics(void) {
    if (!lyrics_embedded) return;
    lyrics_embedded = false;
    if (gui_lyrics_embedded_in(content)) gui_lyrics_hide_embedded();
}

static void lyrics_download_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || atomic_load(&fetch_state) != 0) return;
    const char * path = gui_player_get_current_track_path();
    if (!track_is_local(path)) return;
    lyrics_fetch_req_t * req = calloc(1, sizeof(*req));
    if (!req) return;
    snprintf(req->path, sizeof(req->path), "%s", path);
    song_row_t song;
    if (metadata_db_get_song_by_path(path, &song)) {
        snprintf(req->title, sizeof(req->title), "%s", song.tags.title);
        snprintf(req->artist, sizeof(req->artist), "%s", song.tags.artist);
        snprintf(req->album, sizeof(req->album), "%s", song.tags.album);
    }
    if (!req->title[0]) {
        char folder[128];
        get_display_names(path, req->title, sizeof(req->title), folder, sizeof(folder));
    }
    req->duration_s = (int) (gui_player_display_duration() + 0.5);
    snprintf(fetch_path, sizeof(fetch_path), "%s", path);
    fetch_status[0] = '\0';
    atomic_store(&fetch_state, 1);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    if (pthread_create(&th, &attr, lyrics_fetch_thread, req) != 0) {
        free(req);
        atomic_store(&fetch_state, 0);
    }
    pthread_attr_destroy(&attr);
    show_lyrics();
}

static void poll_lyrics_fetch(void) {
    if (atomic_load(&fetch_state) != 2) return;
    atomic_store(&fetch_state, 0);
    const char * path = gui_player_get_current_track_path();
    bool same = path && strcmp(path, fetch_path) == 0;
    switch (fetch_result) {
        case LRCLIB_OK:
        case LRCLIB_OK_PLAIN:
        case LRCLIB_EXISTS:
            snprintf(fetch_status, sizeof(fetch_status), "%s", TR("Lyrics saved. Loading..."));
            if (same) gui_lyrics_load_track(gui_player_get_playlist_index(), path);
            break;
        case LRCLIB_NOT_FOUND:
            snprintf(fetch_status, sizeof(fetch_status), "%s", TR("LRCLIB has no synced lyrics for this track."));
            break;
        case LRCLIB_PLAIN_ONLY:
            snprintf(fetch_status, sizeof(fetch_status), "%s",
                     TR("LRCLIB only has unsynced lyrics for this track."));
            break;
        case LRCLIB_NET_ERROR:
            snprintf(fetch_status, sizeof(fetch_status), "%s", TR("Could not reach lrclib.net. Check Wi-Fi."));
            break;
        case LRCLIB_BUSY:
            snprintf(fetch_status, sizeof(fetch_status), "%s", TR("lrclib.net is busy. Try again in a minute."));
            break;
        case LRCLIB_WRITE_ERROR:
            snprintf(fetch_status, sizeof(fetch_status), "%s", TR("Could not save the .lrc file next to the song."));
            break;
    }
    if (panel_screen && active_tab == TAB_LYRICS && !lyrics_embedded) show_lyrics();
}

static void show_lyrics(void) {
    lv_obj_clean(content);
    if (!gui_lyrics_has_lines()) {
        const char * path = gui_player_get_current_track_path();
        bool running = atomic_load(&fetch_state) != 0;
        build_list_message(content, TR("No lyrics for this track"),
                           running ? TR("Searching lrclib.net...")
                           : fetch_status[0] && path && strcmp(path, fetch_path) == 0 ? fetch_status
                           : TR("Add a .lrc file with the same name next to the song, or download synced lyrics "
                                "from lrclib.net."));
        if (!track_is_local(path) || running) return;
        lv_obj_t * btn = lv_obj_create(content);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, BOARD_UI_PX(44));
        lv_obj_set_style_pad_hor(btn, BOARD_UI_PX(20), 0);
        lv_obj_set_style_radius(btn, BOARD_UI_PX(22), 0);
        lv_obj_add_style(btn, gui_theme_accent_style(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_margin_left(btn, GUI_TEXT_INSET, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, lyrics_download_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * l = lv_label_create(btn);
        lv_label_set_text_fmt(l, LV_SYMBOL_DOWNLOAD "  %s", TR("Download lyrics"));
        lv_obj_set_style_text_color(l, lv_color_hex(GUI_COLOR_ON_ACCENT), 0);
        lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_center(l);
        return;
    }
    lv_obj_update_layout(content);
    gui_lyrics_show_embedded(content, 0, lv_obj_get_content_height(content));
    lyrics_embedded = true;
}

/* More: the actions the ⋮ sheet used to hold (play mode is the player's
 * left transport button). Delete is last and separated. */
static void more_row_cb(lv_event_t * e);

static lv_obj_t * more_row(const char * icon, const char * text, const char * value, gui_player_action_t action,
                           bool destructive) {
    lv_obj_t * row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BOARD_UI_PX(46));
    lv_obj_set_style_radius(row, BOARD_UI_PX(8), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(GUI_COLOR_CARD), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, more_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) action);
    lv_color_t color = destructive ? lv_color_hex(GUI_COLOR_ERROR) : lv_color_hex(0xE6E8E3);
    lv_obj_t * i = lv_label_create(row);
    lv_label_set_text(i, icon);
    lv_obj_set_style_text_color(i, destructive ? color : lv_color_hex(0xA9ADA6), 0);
    lv_obj_set_width(i, BOARD_UI_PX(24));
    lv_obj_set_style_text_align(i, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, BOARD_UI_PX(12), 0);
    lv_obj_t * t = lv_label_create(row);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, color, 0);
    lv_obj_set_style_text_font(t, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, BOARD_UI_PX(50), 0);
    if (value) {
        lv_obj_t * v = lv_label_create(row);
        lv_label_set_text(v, value);
        lv_obj_add_style(v, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(v, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, BOARD_UI_PX(-12), 0);
    }
    return row;
}

static void build_more(void) {
    lv_obj_clean(content);
    more_row(LV_SYMBOL_LIST, TR("Edit queue"), NULL, GUI_PLAYER_ACTION_EDIT_QUEUE, false);
    more_row(LV_SYMBOL_PLUS, TR("Add to playlist"), NULL, GUI_PLAYER_ACTION_ADD_TO_PLAYLIST, false);
    more_row(LV_SYMBOL_DIRECTORY, TR("Go to album"), NULL, GUI_PLAYER_ACTION_GO_ALBUM, false);
    more_row(LV_SYMBOL_AUDIO, TR("Go to artist"), NULL, GUI_PLAYER_ACTION_GO_ARTIST, false);
    more_row(LV_SYMBOL_SETTINGS, TR("Equalizer"), NULL, GUI_PLAYER_ACTION_EQUALIZER, false);
    lv_obj_t * divider = lv_obj_create(content);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, lv_pct(100), BOARD_UI_PX(9));
    lv_obj_set_style_border_side(divider, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(divider, 1, 0);
    lv_obj_set_style_border_color(divider, lv_color_hex(0x2A2E32), 0);
    lv_obj_set_style_margin_top(divider, BOARD_UI_PX(8), 0);
    more_row(LV_SYMBOL_TRASH, TR("Delete file"), NULL, GUI_PLAYER_ACTION_DELETE, true);
}

static void more_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_player_action_t action = (gui_player_action_t) (intptr_t) lv_event_get_user_data(e);
    if (!gui_player_get_current_track_path()) return;
    gui_player_run_action(action);
}

static void show_tab(panel_tab_t tab) {
    release_lyrics();
    active_tab = tab;
    for (int i = 0; i < TAB_COUNT; i++) {
        bool on = i == (int) tab;
        lv_obj_set_style_bg_color(tab_buttons[i], on ? lv_color_hex(0xEFDC84) : lv_color_hex(GUI_COLOR_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tab_buttons[i], 0),
                                    on ? lv_color_hex(GUI_COLOR_ON_ACCENT) : lv_color_hex(GUI_COLOR_TEXT_SOFT), 0);
    }
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    if (tab == TAB_UP_NEXT) build_up_next();
    else if (tab == TAB_INFO) build_info();
    else if (tab == TAB_MORE) build_more();
    else show_lyrics();
}

static void tab_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_tab((panel_tab_t) (intptr_t) lv_event_get_user_data(e));
}

static void play_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
    refresh_header();
}

static void back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static void timer_cb(lv_timer_t * t) {
    (void) t;
    if (!panel_screen || lv_screen_active() != panel_screen) {
        release_lyrics();
        return;
    }
    refresh_header();
    poll_lyrics_fetch();
    /* Lyrics load in the background; switch from the message once ready. */
    if (active_tab == TAB_LYRICS && !lyrics_embedded && gui_lyrics_has_lines()) show_lyrics();
    if (active_tab == TAB_UP_NEXT) {
        int * order = NULL, count = 0, current = -1;
        uint64_t revision = 0;
        if (gui_player_queue_snapshot(&order, &count, &current, &revision) &&
            (revision != shown_revision || current != shown_current))
            build_up_next();
        free(order);
    }
}

static void panel_unloaded_cb(lv_event_t * e) {
    (void) e;
    release_lyrics();
}

static lv_obj_t * build_panel(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, panel_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    mini_cover = lv_image_create(scr);
    lv_obj_set_size(mini_cover, BOARD_UI_PX(64), BOARD_UI_PX(64));
    lv_image_set_inner_align(mini_cover, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_radius(mini_cover, BOARD_UI_PX(6), 0);
    lv_obj_set_style_clip_corner(mini_cover, true, 0);
    lv_obj_set_pos(mini_cover, BOARD_UI_PX(16), BOARD_UI_PX(32));
    lv_obj_add_flag(mini_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mini_cover, back_cb, LV_EVENT_CLICKED, NULL);

    mini_title = lv_label_create(scr);
    lv_obj_add_style(mini_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(mini_title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_width(mini_title, BOARD_UI_PX(192));
    lv_label_set_long_mode(mini_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(mini_title, BOARD_UI_PX(92), BOARD_UI_PX(38));
    mini_meta = lv_label_create(scr);
    lv_obj_add_style(mini_meta, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(mini_meta, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(mini_meta, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(mini_meta, BOARD_UI_PX(192));
    lv_label_set_long_mode(mini_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(mini_meta, BOARD_UI_PX(92), BOARD_UI_PX(66));

    lv_obj_t * play = lv_obj_create(scr);
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, BOARD_UI_PX(44), BOARD_UI_PX(44));
    lv_obj_set_pos(play, BOARD_UI_PX(300), BOARD_UI_PX(42));
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_style(play, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(play, BOARD_UI_PX(8));
    lv_obj_add_event_cb(play, play_cb, LV_EVENT_CLICKED, NULL);
    mini_play_icon = lv_label_create(play);
    lv_obj_set_style_text_color(mini_play_icon, lv_color_hex(GUI_COLOR_ON_ACCENT), 0);
    lv_obj_center(mini_play_icon);

    static const char * const names[TAB_COUNT] = { N_("Up next"), N_("Lyrics"), N_("Info"), N_("More") };
    lv_obj_t * tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, BOARD_UI_PX(328), BOARD_UI_PX(34));
    lv_obj_set_pos(tabs, BOARD_UI_PX(16), BOARD_UI_PX(108));
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, BOARD_UI_PX(6), 0);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t * b = lv_obj_create(tabs);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, BOARD_UI_PX(34));
        lv_obj_set_style_pad_hor(b, BOARD_UI_PX(13), 0);
        lv_obj_set_style_radius(b, BOARD_UI_PX(17), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, TR(names[i]));
        lv_obj_set_style_text_font(l, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_center(l);
        tab_buttons[i] = b;
    }

    content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, BOARD_UI_PX(344), BOARD_SCREEN_HEIGHT - BOARD_UI_PX(152 + 14));
    lv_obj_set_pos(content, BOARD_UI_PX(8), BOARD_UI_PX(152));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_now_panel_open_tab(gui_now_panel_tab_t tab) {
    if (!panel_screen) panel_screen = build_panel();
    if (!panel_timer) panel_timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh_header();
    if (lv_screen_active() != panel_screen) nav_push(panel_screen);
    show_tab((panel_tab_t) tab);
}

void gui_now_panel_open(void) {
    gui_now_panel_open_tab(GUI_NOW_PANEL_TAB_UP_NEXT);
}

void gui_now_panel_teardown(void) {
    release_lyrics();
    if (panel_screen) { lv_obj_delete(panel_screen); panel_screen = NULL; }
}
