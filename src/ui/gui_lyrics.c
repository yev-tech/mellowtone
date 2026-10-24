#include "gui_lyrics.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "lvgl/lvgl.h"
#include "assets.h"
#include "gui.h"
#include "screen_builders.h"

#include "settings.h"
#include "metadata.h"
#include <limits.h>

#include "lyrics.h"
#include "lyrics_layout.h"
#include "fallback_font.h"
#include "db_log.h"

typedef struct {
    int tier;
    const char * label;
} lyrics_font_size_option_t;

#define LYRICS_POOL_SIZE 20
/* Both side margins together. Scaled as one value, not twice 20: on the
 * 320px board the rounding differs. */
#define LYRICS_ROW_MARGINS BOARD_SCALE_PX(40)
#define LYRICS_ROW_WIDTH (BOARD_SCREEN_WIDTH - LYRICS_ROW_MARGINS)
#define LYRICS_ROW_GAP BOARD_SCALE_PX(24)
#define LYRICS_ACTIVE_LINE_ANCHOR_Y BOARD_SCALE_PX(16)
#define LYRICS_TOP_PAD LYRICS_ACTIVE_LINE_ANCHOR_Y
#define LYRICS_TIMER_PERIOD_MS 150
#define LYRICS_AUTO_FOLLOW_RESUME_MS 3000L
#define LYRICS_FONT_SIZE_OPTION_COUNT (sizeof(lyrics_font_size_options) / sizeof(lyrics_font_size_options[0]))

extern lv_font_t app_font_lyrics;
extern lv_color_t accent_lv_color(void);
extern void show_error_toast(const char * msg);
extern void gui_navigation_invalidate_font_snapshots(void);


extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern void nav_push(lv_obj_t * screen);
extern void finalize_screen_navigation(lv_obj_t * screen);

/* Never-loaded screen that parks the shared lyrics list (and its empty
 * label) while neither the player nor the Now panel embeds it. */
static lv_obj_t * lyrics_parking;
/* ---- Lyrics view state -- see build_lyrics_parking() and the async .lrc
 * load below. ---- */
static lyrics_doc_t current_lyrics_doc;
static bool current_lyrics_doc_valid = false;
static int current_lyrics_doc_for_index = -1; /* gui_player_get_playlist_index() current_lyrics_doc was loaded for */
static int current_lyrics_doc_generation = -1;
static int lyrics_load_generation = 0;
/* Embedded lyrics tags (ID3 USLT, FLAC/Opus LYRICS/
 * UNSYNCEDLYRICS, M4A "\xA9lyr") are usually plain unsynced text with no
 * [mm:ss.xx] timestamps. current_lyrics_doc_valid/current_lyrics_doc
 * stay reserved for the synced case (an .lrc sidecar, or tags containing
 * LRC timestamps); unsynced text is displayed as a static block without
 * per-line highlight or auto-follow, mutually exclusive with
 * current_lyrics_doc_valid. current_lyrics_plain_text is malloc'd, owned
 * by these globals once poll_lyrics_load() transfers it; freed on the next
 * load and whenever current_lyrics_plain_mode is cleared. */

extern void audio_seek(double seconds);
extern double audio_get_position_seconds(void);

typedef struct {
    int generation;
    int for_index;
    char track_path[PATH_MAX];
} lyrics_load_request_t;
extern player_settings_t current_settings;
extern lv_font_t app_font_22;

bool current_lyrics_plain_mode = false;
static char * current_lyrics_plain_text = NULL;
static lv_obj_t * lyrics_list;
static lv_obj_t * lyrics_spacer;
static lv_obj_t * lyrics_empty_label;
static lv_obj_t * lyrics_plain_label; /* current_lyrics_plain_mode's own display -- see its own comment */
static lv_timer_t * lyrics_timer;
static bool lyrics_embedded = false;
static pthread_t lyrics_load_thread;
static bool lyrics_load_active = false;
/* atomic_bool ensures well-defined memory ordering between the worker
 * thread's write and the UI thread's poll read. */
static atomic_bool lyrics_load_done_flag = false;
static int lyrics_load_result_generation;
static int lyrics_load_result_for_index;
static bool lyrics_load_result_ok;
static lyrics_doc_t lyrics_load_result_doc;
/* Set alongside lyrics_load_result_ok when a source (sidecar or embedded
 * tag) had text but no [mm:ss.xx] timestamps to parse into lyrics_load_
 * result_doc -- see lyrics_load_thread_func()'s own comment. Mutually
 * exclusive with lyrics_load_result_ok: exactly one of the two, or neither
 * (no lyrics found anywhere), is ever true for a given load. Owned by the
 * worker thread until the done flag flips, same as lyrics_load_result_doc. */
static bool lyrics_load_result_plain_mode;
static char * lyrics_load_result_plain_text; /* malloc'd, only meaningful when lyrics_load_result_plain_mode */
/* Bumped on every launch_lyrics_load() call (both a real track change and
 * the internal pending-relaunch below) -- the identity a finished result is
 * checked against in poll_lyrics_load(), not gui_player_get_playlist_index(). gui_player_get_playlist_index()
 * is a numeric SLOT, not a track identity: if the whole playlist is
 * replaced (a different album/Subsonic queue/etc. loaded) while a load is
 * still in flight, the new playlist's own track at that same slot number
 * can easily be a completely different song, and an index-only check would
 * wrongly accept the stale result for it. A strictly increasing counter
 * has no such collision. */
static bool lyrics_load_pending_valid = false;
static int lyrics_load_pending_for_index;
static char lyrics_load_pending_path[PATH_MAX];
static void * lyrics_load_thread_func(void * arg) {
    lyrics_load_request_t * req = (lyrics_load_request_t *) arg;
    lyrics_doc_t doc;
    bool ok = lyrics_load_sidecar(req->track_path, &doc);
    bool plain_mode = false;
    char * plain_text = NULL;

    /* Fall back to the track's embedded lyrics tag (ID3 USLT, FLAC/Opus
     * LYRICS/UNSYNCEDLYRICS, M4A "\xA9lyr") when there is no .lrc sidecar.
     * Parses LRC timestamps if present; otherwise falls back to plain_mode
     * (static unsynced text). */
    /* A sidecar without timestamps (unsynced lyrics, e.g. from LRCLIB) shows
     * as static text, like an unsynced embedded tag. */
    if (!ok) {
        char * text = lyrics_read_sidecar_text(req->track_path);
        if (text && text[0]) {
            utf8_sanitize(text);
            plain_mode = true;
            plain_text = text;
        } else {
            free(text);
        }
    }
    if (!ok && !plain_mode) {
        track_metadata_t meta;
        metadata_read_lyrics_without_artwork(req->track_path, &meta);
        if (meta.lyrics) {
            /* Keep embedded lyrics under the same byte limit as sidecars.
             * Apart from avoiding a large parser/UI allocation, bounded
             * length prevents strlen() from walking arbitrarily far through
             * a malformed decoder result. Oversized or empty tags behave as
             * no lyrics; metadata.lyrics remains owned here and is freed
             * below. */
            size_t lyrics_len = strnlen(meta.lyrics, LYRICS_MAX_FILE_BYTES + 1U);
            if (lyrics_len > 0 && lyrics_len <= LYRICS_MAX_FILE_BYTES) {
                if (lyrics_parse_buffer(meta.lyrics, lyrics_len, &doc)) {
                    ok = true;
                } else {
                    /* Only the unsynchronized/plain fallback is sanitized.
                     * Valid synced lyrics retain their parser behavior. */
                    utf8_sanitize(meta.lyrics);
                    plain_mode = true;
                    plain_text = meta.lyrics;
                    meta.lyrics = NULL; /* ownership transferred to plain_text -- don't free it below */
                }
            }
        }
        free(meta.picture_data);
        free(meta.lyrics);
    }

    lyrics_load_result_generation = req->generation;
    lyrics_load_result_for_index = req->for_index;
    lyrics_load_result_ok = ok;
    if (ok) lyrics_load_result_doc = doc;
    lyrics_load_result_plain_mode = plain_mode;
    lyrics_load_result_plain_text = plain_text;
    free(req);
    atomic_store_explicit(&lyrics_load_done_flag, true, memory_order_release); /* written last -- poll_lyrics_load() only checks this flag */
    return NULL;
}
static void launch_lyrics_load(int for_index, const char * track_path) {
    lyrics_load_generation++;
    if (lyrics_load_active) {
        lyrics_load_pending_for_index = for_index;
        snprintf(lyrics_load_pending_path, sizeof(lyrics_load_pending_path), "%s", track_path);
        lyrics_load_pending_valid = true;
        return;
    }

    lyrics_load_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->generation = lyrics_load_generation;
    req->for_index = for_index;
    snprintf(req->track_path, sizeof(req->track_path), "%s", track_path);
    atomic_store_explicit(&lyrics_load_done_flag, false, memory_order_relaxed);
    lyrics_load_active = true;
    if (pthread_create(&lyrics_load_thread, NULL, lyrics_load_thread_func, req) != 0) {
        free(req);
        lyrics_load_active = false;
    }
}
/* Called every tick from update_timer_cb, same as poll_cover_decode() --
 * regardless of whether the lyrics view is actually shown, so a load
 * finishes well before the user ever opens it. Deliberately does not touch
 * any lyrics LVGL object directly -- lyrics_show_embedded() and
 * lyrics_timer_cb() below independently notice when
 * current_lyrics_doc_for_index no longer matches what the pool was last
 * built from and resync from scratch at that point, so there is exactly
 * one place that reconciles the parsed doc with the on-screen pool. */
static void poll_lyrics_load(void) {
    if (!lyrics_load_active || !atomic_load_explicit(&lyrics_load_done_flag, memory_order_acquire)) return;
    lyrics_load_active = false;
    pthread_join(lyrics_load_thread, NULL);

    if (lyrics_load_result_generation == lyrics_load_generation) {
        if (current_lyrics_doc_valid) lyrics_doc_free(&current_lyrics_doc);
        current_lyrics_doc_valid = lyrics_load_result_ok;
        if (lyrics_load_result_ok) current_lyrics_doc = lyrics_load_result_doc;
        current_lyrics_doc_for_index = lyrics_load_result_for_index;
        current_lyrics_doc_generation = lyrics_load_result_generation;

        free(current_lyrics_plain_text);
        current_lyrics_plain_mode = lyrics_load_result_plain_mode;
        current_lyrics_plain_text = lyrics_load_result_plain_text;
    } else {
        if (lyrics_load_result_ok) lyrics_doc_free(&lyrics_load_result_doc); /* stale -- superseded by a later launch before this landed */
        free(lyrics_load_result_plain_text);
    }

    if (lyrics_load_pending_valid) {
        lyrics_load_pending_valid = false;
        launch_lyrics_load(lyrics_load_pending_for_index, lyrics_load_pending_path);
    }
}
static lv_obj_t * lyrics_rows[LYRICS_POOL_SIZE];
/* Prefix positions for the current document: offsets[i] is row i's Y and
 * offsets[count] is the bottom after its final gap. Long lines can wrap to
 * an arbitrary number of visual lines (up to LYRICS_MAX_LINE_BYTES), so a
 * fixed "three lines ought to fit" stride still allowed real text to paint
 * into its neighbor. These two parallel tables (offsets and per-line
 * line_space) cost at most ~32KB for 4000 lines and make rendering, hit
 * mapping and auto-follow share the exact measured geometry. */
static lyrics_layout_t lyrics_layout;
static int lyrics_window_start = -1; /* index currently shown by lyrics_rows[0]; -1 forces the first update to actually run */
static int lyrics_pool_synced_for_index = -1; /* current_lyrics_doc_for_index the pool/spacer were last built from */
static int lyrics_pool_synced_generation = -1; /* current_lyrics_doc_generation the row pool was last synchronized from */
/* Layout validity is independent of the row pool: the pool is reset when
 * the list is reparented, while the measured offsets remain valid for the
 * same document and font. */
static int lyrics_layout_for_index = -1;
static int lyrics_layout_generation = -1;
static bool lyrics_layout_valid;
static bool lyrics_auto_follow = true;
static int lyrics_last_centered_index = -2; /* -2 = "never centered yet", distinct from -1 (a real "before the first line" position) */
static struct timespec lyrics_last_manual_scroll_at;
/* Only current_lyrics_doc_for_index == gui_player_get_playlist_index() counts as "this
 * doc is for the track actually playing" -- a load still in flight, one
 * that finished for a since-superseded track, or simply having no track
 * playing at all, are all "no active line" here, matching poll_lyrics_
 * load()'s own staleness check above. */
static int lyrics_current_active_index(void) {
    if (!current_lyrics_doc_valid || current_lyrics_doc_for_index != gui_player_get_playlist_index() || gui_player_get_playlist_index() < 0) return -1;
    return lyrics_find_line_at(&current_lyrics_doc, audio_get_position_seconds());
}
static int32_t lyrics_line_y(int index) {
    return lyrics_layout_y(&lyrics_layout, index);
}
static int32_t lyrics_line_height(int index) {
    return lyrics_layout_height(&lyrics_layout, index);
}
static int32_t lyrics_line_line_space(int index) {
    return lyrics_layout_line_space(&lyrics_layout, index);
}
static void lyrics_build_line_offsets(int32_t row_width) {
    lyrics_layout_build(&lyrics_layout, current_lyrics_doc_valid ? &current_lyrics_doc : NULL,
                        &app_font_lyrics, row_width, LYRICS_ROW_GAP, LYRICS_TOP_PAD);
}
static int lyrics_first_line_at_y(int32_t y, int count) {
    return lyrics_layout_first_at_y(&lyrics_layout, y, count);
}
/* Ensure measured geometry matches the current document, font invalidation
 * epoch and list anchor. This deliberately has no row-pool or scroll side
 * effects so the player can pay the first measurement before its opening
 * animation starts. */
static void lyrics_ensure_layout_width(bool force_layout_rebuild, int32_t row_width) {
    int count = current_lyrics_doc_valid ? current_lyrics_doc.count : 0;
    /* lyrics_layout_build() calls lv_text_get_size() once per line -- real,
     * synchronous UI-thread cost that scales with document length. Its
     * validity is tracked separately from pool synchronization because
     * embedding reparents/resets the row pool without changing measured
     * geometry. Font changes explicitly force a rebuild even though the
     * document identity stays the same. The row width is part of the key:
     * wrapping depends on it, and the default embedded pane and a layout's
     * own area can differ. */
    if (force_layout_rebuild || !lyrics_layout_valid || lyrics_layout.row_width != row_width ||
        current_lyrics_doc_for_index != lyrics_layout_for_index ||
        current_lyrics_doc_generation != lyrics_layout_generation) {
        uint64_t t0 = db_log_now_ms();
        lyrics_build_line_offsets(row_width);
        lyrics_layout_for_index = current_lyrics_doc_for_index;
        lyrics_layout_generation = current_lyrics_doc_generation;
        lyrics_layout_valid = true;
        DB_LOG("LYRICS", "layout_build_ms=%llu lines=%d", (unsigned long long) (db_log_now_ms() - t0), count);
    }
}
static void lyrics_ensure_layout(bool force_layout_rebuild) {
    lyrics_ensure_layout_width(force_layout_rebuild, LYRICS_ROW_WIDTH);
}
/* Re-syncs the row pool/spacer/empty-state to whatever current_lyrics_doc
 * currently holds -- called on every embed (lyrics_show_embedded() below) and
 * whenever lyrics_timer_cb() notices the doc changed while already open
 * (a track change while the user is looking at this screen). Always resets
 * scroll to the top and re-enables auto-follow, so both a fresh open and a
 * new track start from the same, predictable state rather than wherever a
 * previous track's manual scroll happened to leave it.
 *
 * force_layout_rebuild: gui_lyrics_refresh_layout() (font size / custom font
 * changed) must pass true -- see lyrics_ensure_layout() for the cache key. */
static void lyrics_reset_pool(bool force_layout_rebuild) {
    int count = current_lyrics_doc_valid ? current_lyrics_doc.count : 0;
    lyrics_ensure_layout(force_layout_rebuild);

    /* current_lyrics_plain_mode -- see its own doc comment: a single static
     * text block, no per-line pool/highlight/auto-follow at all. Handled as
     * its own branch, entirely separate from the synced count==0/count>0
     * cases below, since "0 synced lines" (count==0) still needs the
     * "No synchronized lyrics found" empty state when plain mode ISN'T
     * active either (genuinely no lyrics anywhere). */
    if (current_lyrics_plain_mode && current_lyrics_plain_text) {
        lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
        for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) lv_obj_add_flag(lyrics_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lyrics_plain_label, current_lyrics_plain_text);
        lv_obj_set_y(lyrics_plain_label, LYRICS_TOP_PAD);
        lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN);

        /* LV_LABEL_LONG_WRAP's real rendered height depends on the text
         * just set and isn't known until a layout pass runs -- unlike the
         * synced case's fixed per-line stride, this can't be computed from
         * a line count. lv_obj_update_layout() forces that pass immediately
         * so lv_obj_get_height() below reflects the real wrapped height,
         * not a stale one from whatever text was showing before. */
        lv_obj_update_layout(lyrics_plain_label);
        int32_t total_height = LYRICS_TOP_PAD + lv_obj_get_height(lyrics_plain_label) + LYRICS_TOP_PAD;
        lv_obj_set_pos(lyrics_spacer, 0, total_height > 0 ? total_height - 1 : 0);
    } else {
        lv_obj_add_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN);

        if (count == 0) {
            lv_obj_remove_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
            for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) lv_obj_add_flag(lyrics_rows[slot], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
        }

        int32_t total_height = count > 0 ? lyrics_line_y(count) + LYRICS_TOP_PAD : LYRICS_TOP_PAD;
        lv_obj_set_pos(lyrics_spacer, 0, total_height > 0 ? total_height - 1 : 0);
    }

    lv_obj_scroll_to_y(lyrics_list, 0, LV_ANIM_OFF);
    lyrics_window_start = -1;
    lyrics_pool_synced_for_index = current_lyrics_doc_for_index;
    lyrics_pool_synced_generation = current_lyrics_doc_generation;
    lyrics_auto_follow = true;
    lyrics_last_centered_index = -2;
}
/* Repositions/relabels the row pool to cover the range of lines actually
 * scrolled into (or near) view, and recolors every visible row for the
 * current active_index -- called on every scroll event and every lyrics_
 * timer_cb() tick. Unlike compact_list_update_window(), this can't early-
 * return purely on "the window hasn't moved": the active line's highlight
 * can change (playback advancing) with the view sitting perfectly still. */
static void lyrics_update_window(int active_index) {
    int count = current_lyrics_doc_valid ? current_lyrics_doc.count : 0;
    if (count == 0) return; /* nothing to position -- empty state already shown by lyrics_reset_pool() */

    int32_t scroll_y = lv_obj_get_scroll_y(lyrics_list);
    if (scroll_y < 0) scroll_y = 0;
    int first = lyrics_first_line_at_y(scroll_y, count) - LYRICS_POOL_SIZE / 4;
    if (first < 0) first = 0;
    int max_first = count - LYRICS_POOL_SIZE;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;

    bool window_moved = first != lyrics_window_start;
    if (window_moved) lyrics_window_start = first;

    for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) {
        int index = first + slot;
        lv_obj_t * row = lyrics_rows[slot];
        if (index >= count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        if (window_moved) {
            lv_obj_set_y(row, lyrics_line_y(index));
            lv_obj_set_height(row, lyrics_line_height(index));
            lv_obj_set_style_text_line_space(row, lyrics_line_line_space(index), 0);
            lv_label_set_text(row, current_lyrics_doc.lines[index].text);
        }
        lv_obj_set_style_text_color(row, index == active_index ? accent_lv_color() : lv_color_make(150, 150, 150), 0);
    }
}
/* Tap a line to seek playback to it -- pool_slot (this row's fixed slot in
 * lyrics_rows[], passed as the event's user_data, same (void *)(intptr_t)
 * idiom plugin_interval_timer_cb uses elsewhere in this file) plus lyrics_
 * window_start resolves to the actual doc line index, same as compact_
 * list_row_click_cb()'s own index resolution. Re-enables auto-follow (in
 * case it was suspended) and forces an immediate recenter on the next
 * lyrics_timer_cb() tick, so the view snaps straight to the tapped line
 * and keeps following from there rather than leaving the old scroll
 * position sitting stale until audio_get_position_seconds() catches up. */
static void lyrics_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    int index = lyrics_window_start + slot;
    if (!current_lyrics_doc_valid || index < 0 || index >= current_lyrics_doc.count) return;

    audio_seek(current_lyrics_doc.lines[index].time_ms / 1000.0);
    lyrics_auto_follow = true;
    lyrics_last_centered_index = -2;
}
/* Records a manual scroll only when driven by an actual finger-press (same
 * lv_indev_get_state()==LV_INDEV_STATE_PRESSED precedent used elsewhere in
 * this file), not the programmatic lv_obj_scroll_to_y() call lyrics_timer_
 * cb() itself makes to re-center on the active line -- otherwise every
 * auto-follow re-center would immediately look like a manual scroll and
 * suspend itself. */
static void lyrics_scroll_event_cb(lv_event_t * e) {
    (void) e;
    lv_indev_t * indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lyrics_auto_follow = false;
        clock_gettime(CLOCK_MONOTONIC, &lyrics_last_manual_scroll_at);
    }
    lyrics_update_window(lyrics_current_active_index());
}
static void lyrics_timer_cb(lv_timer_t * timer) {
    (void) timer;

    if (current_lyrics_doc_for_index != lyrics_pool_synced_for_index ||
        current_lyrics_doc_generation != lyrics_pool_synced_generation) {
        /* current_lyrics_doc_for_index changes synchronously the moment the
         * track changes (gui_lyrics_load_track()), well before the
         * background .lrc/tag load (poll_lyrics_load(), on gui.c's own
         * slower 500ms poll) has had a chance to resolve -- gui_lyrics_load_
         * track() clears current_lyrics_doc_valid/current_lyrics_plain_mode
         * to false right away too, so judging "no lyrics" on that momentary
         * state would misjudge a track that does have lyrics, and -- since
         * lyrics_pool_synced_for_index would already match by the time the
         * real result lands -- never get a second chance to display them.
         * Wait until the load actually resolves (or confirm none was
         * needed at all, e.g. a remote track: launch_lyrics_load() then
         * never runs and lyrics_load_generation is left unbumped) before
         * deciding anything. */
        if (current_lyrics_doc_generation != lyrics_load_generation) return;

        lyrics_reset_pool(false); /* index/generation already known to differ -- rebuild happens via that check */
    }

    /* current_lyrics_plain_mode -- see its own doc comment: a static text
     * block has no active line to track, no auto-follow-with-playback to
     * suspend/resume, and no per-line pool to reposition. lyrics_reset_
     * pool() above already did everything this screen needs for it. */
    if (current_lyrics_plain_mode) return;

    bool was_auto_follow = lyrics_auto_follow;
    lv_indev_t * indev = lv_indev_active();
    bool pressed = indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    if (pressed) {
        lyrics_auto_follow = false;
    } else if (!lyrics_auto_follow) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long idle_ms = (now.tv_sec - lyrics_last_manual_scroll_at.tv_sec) * 1000L +
                       (now.tv_nsec - lyrics_last_manual_scroll_at.tv_nsec) / 1000000L;
        if (idle_ms >= LYRICS_AUTO_FOLLOW_RESUME_MS) lyrics_auto_follow = true;
    }
    /* Auto-follow just resumed after being suspended -- force a recenter
     * below even if active_index hasn't changed since the view may have
     * drifted away from it while suspended. */
    if (lyrics_auto_follow && !was_auto_follow) lyrics_last_centered_index = -2;

    int active_index = lyrics_current_active_index();

    if (lyrics_auto_follow && active_index >= 0 && active_index != lyrics_last_centered_index) {
        int32_t target = lyrics_line_y(active_index) - LYRICS_ACTIVE_LINE_ANCHOR_Y;
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(lyrics_list, target, LV_ANIM_ON);
        lyrics_last_centered_index = active_index;
    }

    lyrics_update_window(active_index);
}

/* Row width, position and text alignment follow the pane they sit in. */
static void lyrics_apply_row_geometry(int32_t pane_width, int32_t row_width, lv_text_align_t align) {
    int32_t x = (pane_width - row_width) / 2;
    for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) {
        lv_obj_set_width(lyrics_rows[slot], row_width);
        lv_obj_set_x(lyrics_rows[slot], x);
        lv_obj_set_style_text_align(lyrics_rows[slot], align, 0);
    }
    lv_obj_set_width(lyrics_plain_label, row_width);
    lv_obj_set_x(lyrics_plain_label, x);
    lv_obj_set_style_text_align(lyrics_plain_label, align, 0);
}

void gui_lyrics_show_embedded(lv_obj_t * parent, int32_t top, int32_t height) {
    if (!parent || !lyrics_parking || height <= 0) return;
    lyrics_embedded = true;
    lyrics_apply_row_geometry(BOARD_SCREEN_WIDTH, LYRICS_ROW_WIDTH, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_parent(lyrics_list, parent);
    lv_obj_set_parent(lyrics_empty_label, parent);
    lv_obj_align(lyrics_list, LV_ALIGN_TOP_LEFT, 0, top);
    lv_obj_set_size(lyrics_list, BOARD_SCREEN_WIDTH, height);
    lv_obj_remove_flag(lyrics_list, LV_OBJ_FLAG_HIDDEN);
    /* Align to the list's center so parent height/padding do not shift the
     * empty state outside the requested region. */
    lv_obj_update_layout(lyrics_list);
    lv_obj_align_to(lyrics_empty_label, lyrics_list, LV_ALIGN_CENTER, 0, 0);
    /* Reset row position/scroll for the embedded view. The measured line
     * geometry is unchanged by reparenting and is reused unless its
     * document identity changed while hidden. */
    lyrics_reset_pool(false);
    lv_timer_resume(lyrics_timer);
    lyrics_timer_cb(NULL);
}

void gui_lyrics_prepare_embedded(lv_obj_t * parent, int32_t top, int32_t height) {
    /* Populate and lay out the rows before the player starts moving. Keep
     * them hidden and paused until show_embedded reveals the finished pane. */
    gui_lyrics_show_embedded(parent, top, height);
    if (!lyrics_embedded) return;
    lv_timer_pause(lyrics_timer);
    lv_obj_add_flag(lyrics_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
}

bool gui_lyrics_embedded_in(const lv_obj_t * parent) {
    return lyrics_embedded && lyrics_list && parent && lv_obj_get_parent(lyrics_list) == parent;
}

void gui_lyrics_hide_embedded(void) {
    if (!lyrics_embedded) return;
    lv_timer_pause(lyrics_timer);
    lv_obj_add_flag(lyrics_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_parent(lyrics_list, lyrics_parking);
    lv_obj_set_parent(lyrics_empty_label, lyrics_parking);
    lv_obj_set_size(lyrics_list, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_align(lyrics_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(lyrics_empty_label, lv_pct(90));
    lv_obj_set_height(lyrics_empty_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(lyrics_empty_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(lyrics_empty_label);
    lyrics_embedded = false;
    lyrics_apply_row_geometry(BOARD_SCREEN_WIDTH, LYRICS_ROW_WIDTH, LV_TEXT_ALIGN_CENTER);
    /* The next embed restores visibility and reconciles the row pool. The
     * measured line geometry is independent of this list's parent/anchor. */
}
static lv_obj_t * build_lyrics_parking(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lyrics_empty_label = lv_label_create(scr);
    lv_label_set_text(lyrics_empty_label, TR("No synchronized lyrics found"));
    lv_obj_set_style_text_font(lyrics_empty_label, &app_font_22, 0);
    lv_obj_set_style_text_color(lyrics_empty_label, lv_color_make(200, 200, 200), 0);
    /* app_font_22 is one of the size-tier-swappable app_font_* handles (see
     * fallback_font_apply_size_tier()'s own comment) -- at the largest tier
     * this string's natural single-line width exceeds the screen, and with
     * no width/wrap set here it just ran off both edges instead of
     * wrapping (GitHub issue #91). Bounded width + centered wrap keeps it
     * fully visible at every tier. */
    lv_obj_set_width(lyrics_empty_label, lv_pct(90));
    lv_label_set_long_mode(lyrics_empty_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lyrics_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lyrics_empty_label);
    lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lyrics_empty_label, LV_OBJ_FLAG_CLICKABLE);

    /* The scrollable content object itself -- transparent so the embedding
     * screen shows through. The pooled row labels below each get
     * their own LV_EVENT_CLICKED handler (lyrics_row_click_cb -- tap a
     * line to seek to it); this object only listens for LV_EVENT_SCROLL,
     * to track manual-scroll-vs-auto-follow. */
    lyrics_list = lv_obj_create(scr);
    lv_obj_set_size(lyrics_list, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_align(lyrics_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(lyrics_list, 0, 0);
    lv_obj_set_style_border_width(lyrics_list, 0, 0);
    lv_obj_set_style_radius(lyrics_list, 0, 0);
    lv_obj_set_style_pad_all(lyrics_list, 0, 0);
    lv_obj_set_scroll_dir(lyrics_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lyrics_list, LV_SCROLLBAR_MODE_OFF);

    for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) {
        lv_obj_t * row = lv_label_create(lyrics_list);
        lv_obj_set_width(row, LYRICS_ROW_WIDTH);
        lv_obj_set_height(row, lv_font_get_line_height(&app_font_lyrics));
        lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(row, &app_font_lyrics, 0); /* own separate size, not app_font_28 -- see fallback_font.h */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, BOARD_SCALE_PX(20), LYRICS_TOP_PAD);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); /* shown by lyrics_update_window() once it has real content */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE); /* tap to seek -- lyrics_row_click_cb() */
        lv_obj_add_event_cb(row, lyrics_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) slot);
        lyrics_rows[slot] = row;
    }

    /* current_lyrics_plain_mode's own display -- a single wrapped label
     * showing the whole embedded-tag text as a static block (no per-line
     * highlight, no auto-follow-with-playback), for the common case where
     * an embedded lyrics tag has no [mm:ss.xx] timestamps to drive the
     * synced view above. Plain LVGL scrolling on lyrics_list (already
     * scrollable) is enough here -- unlike the synced rows, there's no
     * windowing/virtualization needed for a single object. */
    lyrics_plain_label = lv_label_create(lyrics_list);
    lv_obj_set_width(lyrics_plain_label, LYRICS_ROW_WIDTH);
    lv_label_set_long_mode(lyrics_plain_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lyrics_plain_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lyrics_plain_label, &app_font_lyrics, 0);
    lv_obj_set_style_text_color(lyrics_plain_label, lv_color_make(230, 230, 230), 0);
    lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_CLICKABLE); /* static text -- no tap-to-seek, there's no timing to seek to */
    lv_obj_set_pos(lyrics_plain_label, BOARD_SCALE_PX(20), LYRICS_TOP_PAD);
    lv_obj_add_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN); /* shown by lyrics_reset_pool() when current_lyrics_plain_mode */

    /* 1x1 invisible spacer at the bottom of the FULL virtual list -- not
     * hidden (a hidden object doesn't count toward scrollable content
     * size), just visually imperceptible. Same technique as compact_list's
     * own spacer (screen_builders.c). */
    lyrics_spacer = lv_obj_create(lyrics_list);
    lv_obj_set_size(lyrics_spacer, 1, 1);
    lv_obj_set_style_bg_opa(lyrics_spacer, 0, 0);
    lv_obj_set_style_border_width(lyrics_spacer, 0, 0);
    lv_obj_remove_flag(lyrics_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lyrics_spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(lyrics_spacer, 0, 0);

    lv_obj_add_event_cb(lyrics_list, lyrics_scroll_event_cb, LV_EVENT_SCROLL, NULL);

    /* Created paused -- only ticks while the list is embedded (resumed by
     * lyrics_show_embedded(), paused by gui_lyrics_hide_embedded()), so it
     * costs nothing the rest of the time. Not the existing 500 ms
     * update_timer_cb: 150 ms is needed here for a scroll/highlight cadence
     * that actually looks smooth. */
    lyrics_timer = lv_timer_create(lyrics_timer_cb, LYRICS_TIMER_PERIOD_MS, NULL);
    lv_timer_pause(lyrics_timer);

    return scr;
}
static const lyrics_font_size_option_t lyrics_font_size_options[] = {
    { 0, N_("Small") }, { 1, N_("Medium") }, { 2, N_("Large") },
};
static lv_obj_t * lyrics_font_size_screen;
static lv_obj_t * lyrics_font_size_list;
static void lyrics_font_size_option_row_cb(lv_event_t * e);
static void populate_lyrics_font_size_screen(void) {
    lv_obj_clean(lyrics_font_size_list);
    for (size_t i = 0; i < LYRICS_FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.lyrics_font_size_tier == lyrics_font_size_options[i].tier;
        add_pill_option_row(lyrics_font_size_list, TR(lyrics_font_size_options[i].label),
                            selected, lyrics_font_size_option_row_cb, (void *) (intptr_t) i);
    }
}
/* Live-apply, same black-mask-behind-a-one-shot-timer shape as the general
 * Font Size selector (font_size_option_row_cb()/font_size_apply_timer_cb(),
 * gui_settings_playback.c) -- no more "Restart Now?" popup/real reboot. Deliberately
 * lighter than that one's own post-apply refresh: app_font_lyrics is
 * consumed exclusively by this screen's own row pool (gui_lyrics_refresh_
 * layout() already resyncs it, including live descender-free row heights,
 * pixel-perfect), so there's no Settings-pill-row geometry or nav-stack
 * sweep to do here, unlike a general Font Size change. */
static void lyrics_font_size_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *) lv_timer_get_user_data(timer);
    int target = (int) (intptr_t) lv_obj_get_user_data(mask) - 1;
    lv_timer_delete(timer);
    if (!fallback_font_apply_lyrics_size_tier(target)) {
        lv_obj_delete(mask);
        show_error_toast(TR("Could not apply lyrics text size"));
        return;
    }

    gui_navigation_invalidate_font_snapshots();
    current_settings.lyrics_font_size_tier = target;
    settings_save(&current_settings);
    gui_lyrics_refresh_layout();
    populate_lyrics_font_size_screen();

    lv_obj_delete(mask);
}

static void lyrics_font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    int target = lyrics_font_size_options[index].tier;
    if (target == current_settings.lyrics_font_size_tier) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *) (intptr_t) (target + 1));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);

    /* Leave enough time for the normal refresh timer to paint the mask;
     * synchronous lv_refr_now() here would re-enter rendering from an input
     * callback -- see font_size_option_row_cb()'s own matching comment
     * (gui_settings_playback.c) for why. */
    lv_timer_create(lyrics_font_size_apply_timer_cb, 35, mask);
}
static lv_obj_t * build_lyrics_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Lyrics Text Size"), &title_label, &lyrics_font_size_list);
}
static void open_lyrics_font_size_screen(void) {
    populate_lyrics_font_size_screen();
    nav_push(lyrics_font_size_screen);
}
void lyrics_font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_lyrics_font_size_screen();
}


void gui_lyrics_init(void) {
    lyrics_parking = build_lyrics_parking();
    lyrics_font_size_screen = build_lyrics_font_size_screen();
}

/* For gui_reload.c's in-process UI reload -- deletes the parking screen and
 * the font-size screen this module owns so gui_lyrics_init() can rebuild them from a clean slate
 * without leaking the old objects. */
void gui_lyrics_teardown(void) {
    gui_lyrics_hide_embedded();
    /* Unguarded lv_timer_create() at this screen's own build site -- same
     * leaked-old-timer hazard as gui_player_teardown()'s volume_popup_hide_
     * timer, see its own comment. */
    if (lyrics_timer) { lv_timer_del(lyrics_timer); lyrics_timer = NULL; }
    if (lyrics_parking) { lv_obj_delete(lyrics_parking); lyrics_parking = NULL; }
    if (lyrics_font_size_screen) { lv_obj_delete(lyrics_font_size_screen); lyrics_font_size_screen = NULL; }
}

void gui_lyrics_poll_load(void) {
    if (gui_player_lyrics_animation_in_progress()) return;
    poll_lyrics_load();
}

void gui_lyrics_load_track(int index, const char * path) {
    if (current_lyrics_doc_valid) {
        lyrics_doc_free(&current_lyrics_doc);
        current_lyrics_doc_valid = false;
    }
    if (current_lyrics_plain_mode) {
        free(current_lyrics_plain_text);
        current_lyrics_plain_text = NULL;
        current_lyrics_plain_mode = false;
    }
    current_lyrics_doc_for_index = index;
    if (path) launch_lyrics_load(index, path);
    else {
        /* Invalidate an in-flight local load when switching to a track
         * without a local lyrics source, including the same queue slot. */
        lyrics_load_pending_valid = false;
        current_lyrics_doc_generation = ++lyrics_load_generation;
    }
    if (lyrics_embedded) lyrics_reset_pool(true);
}

bool gui_lyrics_has_background_work(void) {
    return lyrics_load_active;
}

void gui_lyrics_cancel_background_work(void) {
    lyrics_load_generation++;
    if (lyrics_load_active) {
        pthread_join(lyrics_load_thread, NULL);
        lyrics_load_active = false;
    }
}

void gui_lyrics_refresh_layout(void) {
    if (!lyrics_parking) return;
    /* Called after something that changes lv_text_get_size()'s own results
     * for the SAME document (lyrics text size, custom font) -- the document
     * identity (index/generation) is unchanged, so lyrics_reset_pool()'s own
     * skip-if-unsynced check would otherwise wrongly keep the layout table
     * measured against the old font. Force it. */
    lyrics_reset_pool(true);
    if (lyrics_embedded) lv_obj_invalidate(lyrics_list);
}

void gui_lyrics_prepare_layout(void) {
    lyrics_ensure_layout(false);
}

/* Whether the current track has lyrics lines loaded (Now Playing panel). */
bool gui_lyrics_has_lines(void) {
    /* Unsynced (plain) lyrics count too: the embedded view shows them. */
    return (current_lyrics_doc_valid && current_lyrics_doc.count > 0) || current_lyrics_plain_mode;
}
