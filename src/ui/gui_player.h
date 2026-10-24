#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "metadata.h"
#include "settings.h"
#include "audio.h"



void gui_player_init(uint32_t screen_width, uint32_t screen_height);
/* Deletes every screen/popup this module owns so gui_reload.c's in-process
 * UI reload can call gui_player_init() again from a clean slate. */
void gui_player_teardown(void);
/* Before that teardown: persists a paused or waiting track's resume point
 * (queue checkpoint and last_position). Playback itself is left alone. */
void gui_player_prepare_for_ui_reload(void);
/* Thread-safe: forget remembered "no cover" tracks after a library publication. */
void gui_player_invalidate_cover_negatives(void);
/* Quiesces player cover decoding while the generated artwork cache is being
 * rebuilt. Completion re-reads the playing track's cover. */
void gui_player_begin_cover_reload(void);
void gui_player_finish_cover_reload(void);
void gui_player_cancel_cover_reload(void);
/* Thread-safe: no player cover decode (which may write cache files) is running. */
bool gui_player_cover_decode_idle(void);
void gui_player_refresh_static_assets(void);
void gui_player_refresh_frosted_background(void);
void sync_player_topbar_visibility(lv_obj_t * screen);
void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta);
void poll_cover_decode(void);
void gui_player_update_progress(void);
/* Drops the cached CUE sheet (after a sheet was rewritten). */
void gui_player_forget_cue(void);
void gui_format_time(double seconds, char * buf, size_t buf_size);
void show_volume_popup(int32_t percent);
void hide_volume_popup(void);
void poll_volume_popup_timeout(void);
void refresh_play_btn_icon(void);
/* Recolored btn_play/btn_pause (white disc, accent-colored glyph). Falls
 * back to the stock path if decode failed. Shared by the player transport
 * row and the quick-drawer copy. */
const void * gui_player_play_btn_image_src(bool is_playing);
void refresh_format_badge(void);
void set_play_button_state(bool is_playing);
bool gui_player_boot_prompt_blocked(void);

void configure_native_slider_rail(lv_obj_t * slider);
void cycle_play_mode(void);
void gui_player_set_play_mode(int mode);
void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db, bool * out_has_peak, double * out_peak);

void favorite_icon_event_cb(lv_event_t * e);
/* Playback state semantic accessors and operations */
int gui_player_get_playlist_count(void);
int gui_player_get_playlist_index(void);
bool gui_player_has_active_track(void);
/* Defer visual refresh work while the lyrics pane is moving. */
bool gui_player_lyrics_animation_in_progress(void);
const char * gui_player_get_current_track_path(void);
/* Duration to show: the decoder's once playing, otherwise the current file
 * probed once (cached), so a loaded-but-not-started track shows its length
 * instead of "--:--". 0 when unknown (streams, unsupported files). */
double gui_player_display_duration(void);
const char * gui_player_get_track_path_at(int index);
/* Resolve Subsonic stream identity by exact URL after validating the current
 * playlist slot; this remains valid when queue edits move stream entries. */
bool gui_player_get_subsonic_track_identity(int track_index, const char * path,
                                            char * title, size_t title_size,
                                            char * artist, size_t artist_size,
                                            char * album, size_t album_size);
int gui_player_get_queued_count(void);
const char * gui_player_get_queued_path_at(int offset);
void gui_player_queue_add(const char * path);
void gui_player_queue_add_many(const char * const * paths, int count);
void gui_player_queue_remove_at(int offset);
void gui_player_queue_clear(void);
void gui_player_queue_play_next(const char * path);
/* Indices in a snapshot identify occurrences, not paths. A revision makes
 * stale UI actions fail rather than acting on a different occurrence. */
uint64_t gui_player_queue_revision(void);
bool gui_player_queue_snapshot(int ** order, int * count, int * current, uint64_t * revision);
void gui_player_sync_remote_queue(void);
bool gui_player_remote_queue_remove(int offset, uint64_t revision);
bool gui_player_remote_queue_clear(uint64_t revision);
/* Remote Control Up Next edits by offset (current track excluded); each
 * returns false and changes nothing when the revision is stale. */
bool gui_player_remote_queue_move(int from_offset, int to_offset, uint64_t revision);
bool gui_player_remote_queue_play(int offset, uint64_t revision);
/* Plays a file with its folder as the queue, like a Files tap. */
bool gui_player_play_folder_track(const char * track_path);
/* Reloads the now-playing heart after Remote Control changed a favorite. */
void gui_player_refresh_favorite(void);
bool gui_player_queue_edit(uint64_t revision, int from, int to);
bool gui_player_queue_select(uint64_t revision, int index);
/* Now-playing actions, shared by the ⋮ sheet and the Now panel's More tab. */
typedef enum {
    GUI_PLAYER_ACTION_PLAY_MODE,
    GUI_PLAYER_ACTION_ADD_TO_PLAYLIST,
    GUI_PLAYER_ACTION_GO_ALBUM,
    GUI_PLAYER_ACTION_GO_ARTIST,
    GUI_PLAYER_ACTION_EQUALIZER,
    GUI_PLAYER_ACTION_DELETE,
    GUI_PLAYER_ACTION_EDIT_QUEUE,
} gui_player_action_t;
void gui_player_run_action(gui_player_action_t action);
/* Untranslated name of the current play mode (pass through TR()). */
const char * gui_player_play_mode_name(void);
void gui_player_queue_clear_all(void);
/* Reorder the current queue into its displayed order and select one row.
 * Lazy ranks and explicit path pointers are moved, not copied. */
bool gui_player_queue_restart_displayed(int selected_index);
bool gui_player_queue_save_as(const char * name);
bool gui_player_queue_save_as_poll(bool *done, bool *ok);
void gui_player_queue_checkpoint(void);
void gui_player_queue_checkpoint_urgent(void);
void gui_player_queue_poll_urgent(void);
bool gui_player_queue_write_busy(void);
bool gui_player_queue_checkpoint_failed(void);
void gui_player_queue_flush(void);
void gui_player_play_at(int index);
void gui_player_play_at_from(int index, double start_seconds);
void gui_player_step_manual(int direction);
/* Time skipping applies only to manual transport within this directory. */
void gui_player_set_transport_skip(const char * directory, int seconds);
/* Applies `step_count` accumulated forward-seek steps from a held physical
 * Next button (hw_buttons_consume_next_seek_steps()) -- is_first resets the
 * seek target from the live playback position, same as a touch hold's first
 * tick vs its later repeats. */
void gui_player_hw_next_seek_steps(int step_count, bool is_first);
lv_obj_t * gui_player_get_screen(void);
lv_obj_t * gui_player_get_cover_img(void);
/* The frosted (blurred, darkened) copy of the current cover, or NULL when
 * none has been produced (no cover decoded yet). */
const lv_image_dsc_t * gui_player_get_current_reflection_dsc(void);
bool gui_player_is_seeking(void);
bool gui_player_volume_control_hit_test(lv_point_t point);

const char * playlist_path_at(int index);
void free_playlist(void);
void on_file_selected(char ** new_playlist, int count, int selected_index);
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds);
void on_file_selected_lazy_all_songs(int selected_index);
void on_file_selected_lazy_recently_added(int selected_index);
void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
void on_track_auto_advanced(int index);
void play_track_at(int index);
void play_track_at_from(int index, double start_seconds);
void toggle_play_pause(void);
int compute_manual_step_index(int index, int direction);
void arm_next_track_for_audio(int current_index);

/* Applies a crossfade on/off change from either entry point (the Settings
 * switch or the quick drawer tile). Enabling it also re-enables gapless,
 * which crossfade depends on -- see the definition's own comment. */
void gui_player_set_crossfade_enabled(bool enabled);

/* Applies a gapless on/off change from either entry point (the Settings
 * switch or the quick drawer tile). Disabling it also disables crossfade,
 * which depends on it -- see the definition's own comment. */
void gui_player_set_gapless_enabled(bool enabled);
void commit_auto_advance(void);

void queue_add_song(const char * path);
void queue_remove_song_at_offset(int offset);
void queue_clear_pending(void);

void clear_player_source(void);
void set_player_source_file_browser(const char * dir, int row);
void set_player_source_all_songs(int selected_index);
void set_player_source_recently_added(int selected_index);
struct group_song_entry_s;
typedef struct group_song_entry_s group_song_entry_t;
void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index);

bool gui_player_has_background_work(void);
void gui_player_cancel_background_work(void);


int compute_auto_advance_index(int index);
void reset_decoder_failure_tracking(void);
extern bool user_seeking;
extern bool deferred_resume_pending;
extern double deferred_resume_position;
bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index);
bool build_sd_card_resume_playlist(char *** out_playlist, int * out_count, int * out_index, double * out_position);
bool gui_player_restore_sd_queue(bool is_boot);
void gui_player_notify_sd_unmounted_immediate(void);
void gui_player_notify_sd_mounted(void);
void gui_player_handle_sd_unmount(void);
/* Lending the card: stops "save queue as playlist", waits for a queue
 * checkpoint being written and closes the queue's pinned library directory.
 * False while a checkpoint is still writing: call again. */
bool gui_player_sd_lend_release(void);
bool install_saved_resume_playlist(char ** resume_playlist, int resume_count);
void prepare_deferred_resume(int index, double start_seconds);


int32_t gui_player_get_volume_percent(void);
void gui_player_set_volume_percent(int32_t percent);
/* Remember volume changes without changing audio output. */
void gui_player_remember_volume_percent(int32_t percent);
bool gui_player_volume_is_being_adjusted(void);
const char * gui_player_get_now_playing_title(void);
const char * gui_player_get_now_playing_folder(void);
const char * gui_player_get_now_playing_album(void);
const char * gui_player_get_now_playing_genre(void);
int gui_player_get_now_playing_track_number(void);


void gui_player_handle_auto_advance(void);
void gui_player_handle_track_finished(void);
void gui_player_handle_playback_error(audio_error_t err);
void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation);
void gui_player_poll_confirmed_playback(void);

void gui_player_sync_topbar_visibility(lv_obj_t * screen);
void gui_player_refresh_font_geometry(void);
lv_obj_t * gui_player_get_dismiss_btn(void);
const lv_image_dsc_t * gui_player_get_current_cover_dsc(void);
