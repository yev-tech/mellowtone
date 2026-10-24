#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* Plugin API version definition.
 * Plugins can declare api_min to require specific API features.
 * Sandboxed Lua states restrict filesystem and OS execution access. */
#define PLUGIN_API_VERSION 15
#define PLUGIN_LIST_SCREEN_POOL_SIZE 4

/* Third-party Lua plugin support. Every *.lua file under
 * <SD card>/.mellowtone/plugins/ is loaded into its own lua_State at startup, with a
 * small C API (the `plugin` global table, see plugin_manager.c's own
 * comment above register_plugin_api()) exposed for adding a row to an
 * existing list screen (see PLUGIN_MAX_SETTINGS_LIST_ITEMS below), showing a
 * simple list screen, browsing the SD card, and driving playback -- so a
 * plugin is a plain text script, not compiled code. Each lua_State stays
 * open for the app's entire lifetime (not closed after the initial load)
 * since a plugin's registered callbacks (on_open, on_select, ...) are Lua
 * closures that need their owning state alive to be called later. */

/* Upper bounds on the rows all plugins together may add to each list with
 * plugin.register_list_item(list_id, ...). Each sizes that list's slot in
 * plugin_manager.c's shared plugin_list_items[][] (one array for all 11
 * targets, described by plugin_list_target_desc_t) and the screen's own row
 * array. Where each list appears (docs/PLUGINS.md has the same table):
 *
 *   "settings"        Settings > System > Additional Tools
 *   "display"         Settings > Display (group "appearance" or
 *                     "player_layout": those submenus)
 *   "playback"        Settings > Playback
 *   "music_audio"     Settings > Sound (group "effects": Sound Effects;
 *                     "profiles": Equalizer > Profiles > Download profiles)
 *   "music_controls"  Settings > Playback > Buttons & Remote
 *   "music_timers"    Settings > Power
 *   "music_library"   Settings > Library
 *   "power"           Settings > Power
 *   "system"          Settings > System
 *   "radio"           Radio, above the Radio.txt stations
 *   "developer"       Settings > System > About > Developer Options */
#define PLUGIN_MAX_SETTINGS_LIST_ITEMS 8
#define PLUGIN_MAX_DISPLAY_LIST_ITEMS 8
#define PLUGIN_MAX_PLAYBACK_LIST_ITEMS 8
#define PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS 8
#define PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS 8
#define PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS 8
#define PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS 8
#define PLUGIN_MAX_RADIO_LIST_ITEMS 8
#define PLUGIN_MAX_DEVELOPER_LIST_ITEMS 8
#define PLUGIN_MAX_POWER_LIST_ITEMS 8
#define PLUGIN_MAX_SYSTEM_LIST_ITEMS 8

/* Upper bound on how many tiles all plugins combined may register via
 * plugin.register_stream_media_tile() -- sizes plugin_manager.c's own
 * internal plugin_stream_tiles[] array. Stream Media has exactly one
 * built-in tile (Subsonic) after this session's Qobuz/Tidal/Net Radio
 * cleanup, so unlike Home (already full at 6, can't scroll) it has real
 * room -- capped at 5 to keep the total at 6, the same "fills exactly 3
 * rows, no scroll" ceiling proven out for Home. gui.c's
 * build_stream_media_screen() sizes its own static items[] array off this
 * (1 built-in + this many plugin tiles) -- icon_grid_item_t has no
 * runtime-append API, so that array has to be sized up front. */
#define PLUGIN_MAX_STREAM_TILES 5

/* ---- plugin.show_settings_list() -- a second, separate screen pool from
 * plugin.show_list() (PLUGIN_LIST_SCREEN_POOL_SIZE, gui.c), for a plugin's
 * OWN nested settings submenu with real toggle/slider rows, not plain
 * tappable text. Shared between plugin_manager.c (per-slot Lua callback ref
 * storage, so a reused pool slot's stale refs get released rather than
 * leaked) and gui.c (the pool itself + its per-slot row state) -- see
 * gui.c's gui_plugin_show_settings_list() and plugin_manager.c's
 * l_plugin_show_settings_list(). ---- */

/* Row type tags, passed as plain ints across the gui.c/plugin_manager.c
 * boundary (parallel-array style, matching every other plugin.* bridge in
 * this codebase -- no shared struct type needed across the two headers). */
#define PLUGIN_SETTINGS_ROW_TAP    0 /* plain tap row -- on_select(), no args */
#define PLUGIN_SETTINGS_ROW_TOGGLE 1 /* on/off switch -- on_change(new_value: bool) */
#define PLUGIN_SETTINGS_ROW_SLIDER 2 /* on_change(new_value: number), fires on release only */

/* Two pool slots -- settings submenus nest shallower in practice than plain
 * list browsing (plugin.show_list()'s own pool is 4), so smaller with real
 * headroom under NAV_STACK_MAX. Sizes both gui.c's screen pool and
 * plugin_manager.c's per-slot callback-ref storage below. */
#define PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE 2

/* Upper bound on rows per plugin.show_settings_list() call -- silently
 * truncates past this, same convention plugin.show_list()'s own
 * PLUGIN_MAX_LIST_ITEMS cap uses. Sizes both plugin_manager.c's per-slot ref
 * storage and gui.c's per-slot row-state storage. */
#define PLUGIN_SETTINGS_LIST_MAX_ROWS 24

/* Upper bound on "slider"-type rows per call specifically -- separate from
 * PLUGIN_SETTINGS_LIST_MAX_ROWS because each slider row needs a real,
 * bounded slot in gui.c's swipe_dead_zones[] (see SWIPE_DEAD_ZONE_MAX's own
 * comment there): PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE pool slots x this
 * many sliders each is real, accounted-for headroom, not unbounded growth.
 * A slider row past this cap is silently dropped (not rendered at all),
 * same truncate-don't-error convention as the row cap above. */
#define PLUGIN_SETTINGS_LIST_MAX_SLIDERS 4

/* ---- plugin.on(event, callback) -- lets a plugin react to a playback
 * lifecycle change it didn't itself cause (a scrobbler needs to know when a
 * track starts, for instance), rather than only ever running in response to
 * a direct tap. Unlike plugin.register_list_item() (each plugin's row
 * coexists as its own list entry), an event has no UI real estate to divide
 * up -- every subscriber to a given event fires, so storage is a small
 * per-event array of Lua refs rather than one ref per plugin. See gui.c's
 * own comment on where each event actually fires (play_track_at_from()/
 * on_track_auto_advanced()/toggle_play_pause()/the six audio_stop() call
 * sites). ---- */

/* Upper bound on how many plugin.on() subscriptions any ONE event may have,
 * across every loaded plugin combined -- luaL_error()s past this, same
 * cap-and-fail-loudly convention as PLUGIN_MAX_STREAM_TILES. Each playback,
 * device, volume, and battery event gets its own array sized off this in
 * plugin_manager.c. */
#define PLUGIN_MAX_EVENT_SUBSCRIBERS 8

/* ---- plugin.set_interval(seconds, callback) / plugin.clear_interval(handle)
 * -- a generic repeating timer, needed so a plugin can periodically poll
 * plugin.get_position() (e.g. a scrobbler checking Last.fm's own "50% or 4
 * minutes played" threshold) without a native event to hang that logic off
 * of. Backed by gui.c's lv_timer_create(), the same mechanism
 * update_timer_cb() already uses for its own 500ms polling loop. ---- */

/* Fixed pool of concurrently-active intervals, across every loaded plugin
 * combined -- luaL_error()s a set_interval() call past this rather than
 * silently overwriting or refusing quietly. Sizes gui.c's own
 * plugin_interval_timers[] array too. */
#define PLUGIN_MAX_INTERVALS 8

/* Minimum enforced period for plugin.set_interval() -- a request for less
 * than this is silently clamped up to it (not an error: asking for 100ms
 * isn't misusing the API, just asking for more than makes sense), so one
 * misbehaving plugin can't flood the main UI thread with timer callbacks. */
#define PLUGIN_INTERVAL_MIN_MS 1000

/* Scans <SD card>/.mellowtone/plugins/ for .lua files and loads each one, discovering
 * the rows/tiles they register during load. Should run before anything
 * might dispatch a click (gui_init() calls it early, well before either
 * the Settings or Stream Media screen could plausibly be reached). A missing
 * or empty plugins folder is not an error; a script that fails to
 * load/run is skipped (logged to stderr) without aborting the others. */
void plugin_manager_init(void);
void plugin_manager_poll(void);
bool plugin_manager_has_background_work(void);

/* One entry per *.lua file found directly under <SD card>/.mellowtone/plugins/,
 * whether or not it's currently loaded (a disabled plugin's own
 * plugin.define() id/name is never seen, since its script never runs, so
 * display_name falls back to the filename itself for those). Used by the
 * "Manage Plugins" settings screen (gui_plugin_manage.c) to list every
 * installed plugin, not just the loaded subset plugin_instances[] knows
 * about. */
typedef struct {
    char filename[256];
    bool disabled;
    bool loaded;
    bool over_limit; /* enabled, but the last load hit PLUGIN_MAX_FILES first */
    char display_name[96];
} plugin_available_entry_t;

/* Fills `out` with up to `max` entries and returns the count actually
 * written. Does its own directory scan (safe to call whether or not
 * plugin_manager_init() has run), but each entry's `disabled` flag reflects
 * whatever plugin_disabled_list_load() last loaded into memory --
 * currently only called from inside plugin_manager_init() itself, so
 * calling this before the first plugin_manager_init() will report every
 * file as enabled regardless of what's actually in .disabled on disk.
 * Entries are sorted by filename, UNLESS the on-disk file count exceeds
 * `max`, in which case every currently-loaded plugin is placed first
 * (guaranteed never truncated out) and the remaining budget is filled
 * alphabetically -- see plugin_manager.c's own comment on this function for
 * why. */
int plugin_manager_scan_available(plugin_available_entry_t * out, int max);

/* Counterpart to plugin_manager_init(), for gui_reload.c's in-process UI
 * reload -- closes every plugin's lua_State (after draining background work
 * that references it: async HTTP, interval timers, a pending text-input
 * dialog) and zeroes every list-item/tile/callback/event-subscriber
 * registry, leaving the plugin system ready for plugin_manager_init() to
 * run again from a clean state. See its own doc comment (plugin_manager.c)
 * for exactly what it does and doesn't touch. Not safe to call while any
 * plugin_call() is on the C stack -- see gui_reload.h's own comment on
 * deferred execution. */
void plugin_manager_deinit(void);

/* Optional grouping metadata from register_list_item options.group. Returns
 * NULL for unknown list ids, invalid indices, or rows without a group. */
const char * plugin_manager_get_list_item_group(const char * list_id, int index);

/* Rows registered via plugin.register_list_item("settings", label, on_open)
 * -- gui.c's build_settings_screen() appends these after its own 5 built-in
 * category rows.
 *
 * ..._clicked() calls back into row `index`'s on_open Lua function -- the
 * screen's shared row click handler is the caller, with `index` matching a
 * row's position among the plugin rows (i.e. already offset past the
 * built-in rows by the caller).
 *
 * ..._options() reads back row `index`'s optional icon/height/width/text_size
 * (plugin.register_list_item()'s 4th `options` arg, docs/PLUGINS.md) -- *out_icon
 * and *out_text_size are set to NULL, and both numeric outputs to 0, when
 * index is out of range or that field was never set, matching "unset" all
 * the way through to screen_builders.c's pill_row_apply_icon()/
 * pill_row_resolve_text_size() (both already treat NULL/0 as "default, don't
 * touch today's look"). One combined accessor per list_id family (not three
 * separate ones) -- same data, just avoids tripling the repetition of this
 * family's own count/label/clicked trio. */
int plugin_manager_get_settings_list_item_count(void);
const char * plugin_manager_get_settings_list_item_label(int index);
void plugin_manager_settings_list_item_clicked(int index);
void plugin_manager_get_settings_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("display", ...) -- gui.c's
 * build_settings_display_screen() appends these after its own 4 built-in
 * rows. */
int plugin_manager_get_display_list_item_count(void);
const char * plugin_manager_get_display_list_item_label(int index);
void plugin_manager_display_list_item_clicked(int index);
void plugin_manager_get_display_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                   int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("playback", ...) --
 * gui_settings.c's build_music_playback_screen() appends these after its own
 * built-in rows. */
int plugin_manager_get_playback_list_item_count(void);
const char * plugin_manager_get_playback_list_item_label(int index);
void plugin_manager_playback_list_item_clicked(int index);
void plugin_manager_get_playback_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("music_audio", ...) --
 * gui_settings.c's build_music_audio_screen() appends these after its own
 * built-in rows. */
int plugin_manager_get_music_audio_list_item_count(void);
const char * plugin_manager_get_music_audio_list_item_label(int index);
void plugin_manager_music_audio_list_item_clicked(int index);
void plugin_manager_get_music_audio_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                       int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("music_controls", ...) --
 * gui_settings.c's build_music_controls_screen() appends these after its own
 * built-in rows. */
int plugin_manager_get_music_controls_list_item_count(void);
const char * plugin_manager_get_music_controls_list_item_label(int index);
void plugin_manager_music_controls_list_item_clicked(int index);
void plugin_manager_get_music_controls_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                          int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("music_timers", ...) --
 * gui_settings.c's build_music_timers_screen() appends these after its own
 * built-in row. */
int plugin_manager_get_music_timers_list_item_count(void);
const char * plugin_manager_get_music_timers_list_item_label(int index);
void plugin_manager_music_timers_list_item_clicked(int index);
void plugin_manager_get_music_timers_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                        int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("music_library", ...) --
 * gui_settings.c's build_music_library_screen() appends these after its own
 * built-in row. */
int plugin_manager_get_developer_list_item_count(void);
const char * plugin_manager_get_developer_list_item_label(int index);
void plugin_manager_developer_list_item_clicked(int index);
void plugin_manager_get_developer_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size);

int plugin_manager_get_radio_list_item_count(void);
const char * plugin_manager_get_radio_list_item_label(int index);
void plugin_manager_radio_list_item_clicked(int index);

int plugin_manager_get_music_library_list_item_count(void);
const char * plugin_manager_get_music_library_list_item_label(int index);
void plugin_manager_music_library_list_item_clicked(int index);
void plugin_manager_get_music_library_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                         int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("power", ...) -- gui.c's
 * build_settings_power_screen() appends these after its own built-in rows. */
int plugin_manager_get_power_list_item_count(void);
const char * plugin_manager_get_power_list_item_label(int index);
void plugin_manager_power_list_item_clicked(int index);
void plugin_manager_get_power_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                 int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("system", ...) -- gui.c's
 * build_settings_system_screen() appends these after its own built-in rows. */
int plugin_manager_get_system_list_item_count(void);
const char * plugin_manager_get_system_list_item_label(int index);
void plugin_manager_system_list_item_clicked(int index);
void plugin_manager_get_system_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                  int32_t * out_width, const char ** out_text_size);

/* Invoked by gui.c's plugin-list-screen row click handler when row `index`
 * (into whatever items table the most recent plugin.show_list() call
 * passed) is tapped -- calls back into that call's on_select Lua function
 * with a 1-based Lua index. No-op if no plugin.show_list() call is still
 * "current" (i.e. none has ever been made yet). */
void plugin_manager_list_item_selected(int slot, int index);

/* Same shape as the plugin_manager_get_tile_* / plugin_manager_tile_clicked
 * family above, but for the separate plugin.register_stream_media_tile()
 * registry -- gui.c's build_stream_media_screen() reads these to append
 * plugin-registered tiles after the built-in Subsonic one. */
int plugin_manager_get_stream_tile_count(void);
const char * plugin_manager_get_stream_tile_label(int index);
const char * plugin_manager_get_stream_tile_icon(int index);
const char * plugin_manager_get_stream_tile_icon_selected(int index);
void plugin_manager_stream_tile_clicked(int index);

/* Invoked by gui.c's plugin-settings-list row widgets (see
 * gui_plugin_show_settings_list()) when a "row"-type row in pool slot `slot`
 * at position `row` is tapped -- calls that row's own on_select Lua function
 * with no arguments. Unlike plugin_manager_list_item_selected() above (one
 * shared "current" ref for plugin.show_list()), this looks up a ref stored
 * per (slot, row) -- see plugin_manager.c's own plugin_settings_list_rows[]
 * comment for why. No-op if slot/row is out of range or was never
 * populated. */
void plugin_manager_settings_list_row_selected(int slot, int row);

/* Same shape, for a "toggle"-type row -- calls its on_change(new_value)
 * with the toggle's new boolean state. */
void plugin_manager_settings_list_toggled(int slot, int row, bool new_value);

/* Same shape, for a "slider"-type row -- calls its on_change(new_value) with
 * the slider's new integer value. Fired once on release, not on every drag
 * tick -- see gui.c's plugin_settings_slider_event_cb()'s own comment. */
void plugin_manager_settings_list_slid(int slot, int row, int new_value);

/* ---- Dispatchers for plugin.on() event subscribers. Playback and suspend
 * transitions are called from gui.c at their transition points; volume and
 * battery state are sampled on its existing control tick. Each loops every
 * subscriber and calls it through plugin_call() on the UI thread. ---- */
/* provider/track_id are "" for a local/Subsonic track -- non-empty only for
 * a remote-provider one (plugin.play_remote(), see remote_track.h), purely
 * additive on top of the original 4-arg event so an existing subscriber
 * Lua function (which just ignores extra args it didn't declare) keeps
 * working unchanged. */
void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                          double duration_seconds, const char * provider, const char * track_id);
void plugin_manager_notify_paused(void);
void plugin_manager_notify_resumed(void);
void plugin_manager_notify_stopped(void);
void plugin_manager_notify_screen_woke(void);
void plugin_manager_notify_queue_exhausted(int direction);
void plugin_manager_notify_volume_changed(int percent);
void plugin_manager_poll_battery(void);
void plugin_manager_notify_suspending(void);
void plugin_manager_notify_system_resumed(void);

/* ---- plugin.register_quick_toggle() -- a toggle in the quick drawer's
 * expanded area (gui_shell.c's build_quick_drawer(), third toggle row),
 * alongside the native Wifi/Bluetooth/Sleep/Crossfade ones. For a plugin
 * whose feature is a plain on/off the user wants to reach without walking
 * into Settings (an MSEB-style tone tuner, a gain-mode switch). A plugin
 * that wants a full screen instead still uses register_list_item(). ---- */

/* Fills exactly the drawer's third toggle row (the same 4-column grid the
 * native row uses), across every loaded plugin combined. */
#define PLUGIN_MAX_QUICK_TOGGLES 4

int plugin_manager_get_quick_toggle_count(void);
/* Registered toggle index, or -1 when its plugin is absent or disabled. */
int plugin_manager_find_quick_toggle_by_id(const char * id);
const char * plugin_manager_get_quick_toggle_label(int index);
const char * plugin_manager_get_quick_toggle_icon(int index);
const char * plugin_manager_get_quick_toggle_icon_selected(int index);

/* Caption shown under the icon for the given state -- "On"/"Off" unless the
 * plugin passed its own (GainMode uses "High"/"Low"). */
const char * plugin_manager_get_quick_toggle_state_text(int index, bool on);

/* Last value the plugin published, via the registration's `value` or a later
 * plugin.set_quick_toggle(). The drawer re-reads these on open rather than
 * being pushed to, so a change made inside the plugin's own settings screen
 * shows up the next time the drawer is pulled down. */
bool plugin_manager_get_quick_toggle_value(int index);
/* Whether this toggle registered an on_hold callback, and dispatch that
 * callback. open_settings returns false when no callback is available. */
bool plugin_manager_quick_toggle_has_on_hold(int index);
bool plugin_manager_quick_toggle_open_settings(int index);

/* Stores `new_value` and calls that toggle's on_change(new_value). Invoked by
 * the drawer when the user taps it. */
void plugin_manager_quick_toggle_set(int index, bool new_value);

/* Invoked by gui.c's shared plugin-interval lv_timer callback when the
 * timer for pool slot `slot` (plugin_interval_timers[], gui.c) fires --
 * looks up that slot's stored Lua ref and calls it with no arguments. */
void plugin_manager_interval_fired(int slot);

/* Invoked by gui.c's plugin_text_entry_done_cb() when the single pending
 * plugin.show_text_input() call is submitted (T9 keypad Enter) -- calls its
 * on_submit(text) once, then clears the pending ref (one-shot, matching
 * show_text_entry()'s own "never fires on cancel" semantics -- see
 * plugin_manager.c's l_plugin_show_text_input() for the caveat this
 * inherits from the native singleton screen). */
void plugin_manager_text_input_submitted(const char * text);
void plugin_manager_text_input_cancelled(void);

#endif /* PLUGIN_MANAGER_H */
