# Writing plugins

Plugins add features to the player with plain Lua: new screens, settings,
themes, streaming sources, automations. No compiler, rebuild or firmware
flash is needed.

Examples are in `examples/plugins/` in this repository. To install a
plugin, copy its `.lua` file to `<SD card>/.mellowtone/plugins/` and restart
the player.

This guide describes the API in `src/plugins/plugin_manager.c`. If the two
ever disagree, the code is right.

**Contents**

- [Quick start](#quick-start)
- [How plugins run](#how-plugins-run)
- [Where your plugin appears](#where-your-plugin-appears)
- [Identity and compatibility](#identity-and-compatibility)
- [Screens](#screens)
- [Theming](#theming)
- [Playback](#playback)
- [Music library](#music-library)
- [Files and playlists](#files-and-playlists)
- [Sound](#sound)
- [Device](#device)
- [Network](#network)
- [Data and storage](#data-and-storage)
- [Events and timers](#events-and-timers)
- [Examples](#examples)
- [Testing and debugging](#testing-and-debugging)
- [Extending the API (C developers)](#extending-the-api-c-developers)

## Quick start

1. Create a `.lua` file.
2. Give the plugin an identity with `plugin.define()`.
3. Add a way to open it, for example a row in Settings.
4. Copy the file to `<SD card>/.mellowtone/plugins/`.
5. Open **Settings > System > Plugin Manager** and tap **Refresh Plugins**,
   or restart the player.

```lua
plugin.define({
    id = "org.example.hello",
    name = "Hello Player",
    version = "1.0.0",
    api_min = 1,
})

plugin.register_list_item("settings", "Hello Player", function()
    plugin.show_list("Hello Player", { "The plugin is working!" }, function(index)
        plugin.show_toast("Selected row " .. index)
    end)
end)
```

Save it as `<SD card>/.mellowtone/plugins/HelloPlayer.lua`. The row appears under
Settings > System > Additional Tools.

Tip: the [examples](#examples) are short, one topic each. Start with
`HelloPlayer.lua`, then `SettingsDemo.lua` for settings screens and saved
values, and `PlaybackEvents.lua` or `HttpDemo.lua` for what you need next.

## How plugins run

**Loading**

- Every `*.lua` file directly inside `<SD card>/.mellowtone/plugins/` is a
  plugin (`/mnt/sd_0/.mellowtone/plugins/` on the R3 Pro,
  `/data/mnt/sd_0/.mellowtone/plugins/` on the other boards,
  `./music/.mellowtone/plugins/` on the simulator). Subfolders are not scanned, but plugins can keep their own files
  there.
- Plugins load at startup, and again on **Refresh Plugins** or
  `plugin.reload_ui()`. Each one runs its file from top to bottom once; this is
  where it registers rows, tiles and events.
- Files load in alphabetical order (byte-wise, case-sensitive). Up to 32
  enabled plugins load; Plugin Manager marks the rest "Not loaded: limit
  reached".
- Some settings are global and the last plugin to set them wins, for example
  colors, icons and the menu layouts. Because the order is alphabetical, the
  winner is always the same.
- Each plugin has its own Lua state, kept alive for as long as it is loaded,
  so callbacks can run at any time later.
- A plugin that fails to load is skipped and logged; the others are not
  affected. A callback that raises an error is logged and the player keeps
  running.

**Sandbox**

Plugins come from the SD card, so they don't get the full Lua standard
library:

- Removed: `load`, `loadstring`, `loadfile`, `dofile`, `require`, `package`,
  `debug`, `os.execute`, `os.getenv`, `os.exit`, `os.tmpname` and `io.popen`.
  A plugin must be a single file; it cannot load other Lua files or run shell
  commands.
- Kept: the rest of `io` (including `io.open`), and `os.time`, `os.date`,
  `os.clock`, `os.difftime`, `os.remove` and `os.rename`, so plugins can keep
  their own state files.
- File access is otherwise unrestricted, with two exceptions:
  - nothing can touch `<SD card>/.mellowtone/plugin-data/`, where
    `plugin.storage` and `plugin.secrets` keep their data, or
    `<SD card>/.mellowtone/config/`, where the player keeps its settings
    (with the Subsonic password and the remote control PIN) and saved
    servers. Every `plugin.*` function that takes a path refuses paths inside
    them with the error `path is reserved (player settings, plugin.storage,
    plugin.secrets)`; so do Lua's `io` and `os` functions (`io.open`,
    `os.remove` and `os.rename` return `nil, error`, the others raise). The
    check ignores case and follows symbolic links. `os.rename` also refuses to
    move a folder that contains one of them, such as `.mellowtone` itself;
  - the player's install directory in flash (`/data/mellowtone` on the R3 Pro,
    `/usr/data` outside the SD card on the other boards, and `/usr/mellowtone`
    on a firmware-image install) is read-only for
    plugins: writing, removing or renaming there fails the same way. Keep
    your files on the SD card.

**Time limit**

Each run of plugin code (the top-level code, a callback, an event, a timer
tick) may use at most 2 seconds of Lua time. Time spent inside `plugin.*`
functions (copying icons, a slow HTTP request) doesn't count, but a runaway
loop is stopped. File reads through Lua's own file handles do count.

## Where your plugin appears

| Entry point | Function | Limit |
| --- | --- | --- |
| A row in a native list | `register_list_item` | 8 rows per list, all plugins together |
| A tile in Stream Media | `register_stream_media_tile` | 5 tiles |
| An on/off tile in the quick drawer | `register_quick_toggle` | 4 tiles |

The lists that accept rows:

| `list_id` | Where the row appears | Good for |
| --- | --- | --- |
| `"settings"` | Settings > System > Additional Tools | General plugin settings |
| `"display"` | Settings > Display | Themes and visual tools |
| `"playback"` | Settings > Playback | Resume and playback tools |
| `"music_audio"` | Settings > Sound | EQ, DSP, gain and volume tools |
| `"music_controls"` | Settings > Playback > Buttons & Remote | Button and remote behavior |
| `"music_timers"` | Settings > Power | Sleep and idle timers |
| `"music_library"` | Settings > Library | Scanning, tagging, scrobbling |
| `"power"` | Settings > Power | Battery and power tools |
| `"system"` | Settings > System | Device and maintenance tools |
| `"radio"` | Radio, above the stations | Station finders and directories |
| `"developer"` | Settings > System > About > Developer Options | Status pages and debug tools |

Rows from different plugins share a list, up to 8 per list, and the list
scrolls. Any other `list_id` is an error.

## Identity and compatibility

### `plugin.define(info)`

Declare who the plugin is, once, at the top of the file:

```lua
plugin.define({
    id = "org.example.my_plugin",  -- letters, digits, ".", "_", "-"; must be unique
    name = "My Plugin",
    version = "1.0.0",
    api_min = 2,                   -- refuse to load on an older player
})
```

- The `id` names the plugin's private storage, so keep it stable across
  versions. Two plugins with the same id are an error, and so are ids that
  differ only in case (`My.Plugin` and `my.plugin`): the memory card ignores
  case, so the data lives in `.mellowtone/plugin-data/<id in lower case>/`.
- `api_min` stops the plugin from loading on a player whose API is older.
- Plugins without `define()` still work. They get an id from their file
  name, made unique with a short hash of the path if needed.

### Checking what the player supports

- `plugin.api_version()` returns the API version, currently `15`.
- `plugin.has_capability(name)` returns whether one feature exists. Prefer it
  over `api_min` when you only need one feature. Tokens:

  | Area | Tokens |
  | --- | --- |
  | UI | `ui.list`, `ui.settings`, `ui.row_width`, `ui.text_input`, `ui.toast`, `ui.theme`, `ui.theme_refresh`, `ui.reload`, `ui.launcher_layout`, `ui.quick_toggle`, `ui.text_view`, `ui.text_view_images`, `ui.list_grid`, `ui.list_showing`, `ui.list_wrap`, `ui.settings_list_wrap`, `ui.details` |
  | Playback and audio | `playback.control`, `playback.state`, `playback.events`, `playback.remote`, `playback.transport_skip`, `audio.peq`, `audio.hw_volume_curve` |
  | Files | `filesystem.sd`, `filesystem.mkdir`, `filesystem.playlists`, `filesystem.usage`, `filesystem.paths` |
  | Storage | `storage.namespaced`, `storage.secrets`, `storage.secrets_get` |
  | Network | `network.http.sync`, `network.http.async`, `network.http.download`, `network.wifi_ssid` |
  | Data | `data.json`, `crypto.md5`, `data.image_thumbnail` |
  | Library and radio | `library.artist_albums`, `library.paged`, `library.refresh`, `radio.list` |

- Hardware tokens (`hw.*`) say whether this board's hardware has a function:
  `hw.audio_dac`, `hw.gain_modes`, `hw.bt_audio_out`, `hw.bt_receiver`,
  `hw.wifi`, `hw.wifi_5ghz`, `hw.usb_storage`, `hw.usb_dac`, `hw.usb_dac_dsd`,
  `hw.usb_host_audio`, `hw.charge_control`, `hw.input_current`,
  `hw.battery_current`, `hw.leds`, `hw.inline_remote`, `hw.touch`, `hw.keys`.
- `plugin.has_feature(name)` returns whether an optional software feature is
  built in: `feature.airplay`, `feature.dlna_renderer`, `feature.wifi_import`,
  `feature.remote`, `feature.codec_opus`, `feature.codec_wma`, `feature.i18n`.
  The answer matches exactly what this build compiled in: a feature switched
  off at build time has no code in the player at all.
  Check `plugin.has_feature` exists first on older players.
- `plugin.board()` returns `{ name, model, screen = { w, h }, caps }`, where
  `name` is `r1`, `r3pro`, `r3proii` or `r3ii_2025` and `caps` lists the
  board's `hw.*` tokens.
- LEDs depend on the model: check `plugin.led_available()` instead.
- `plugin.get_app_info()` returns `{ version, build, platform, plugin_api }`.
  `platform` is `hiby-<board name>` (e.g. `hiby-r3pro`), or `host` in the
  simulator.

### API versions

| Version | Added |
| --- | --- |
| 2 | `storage`, `secrets`, `json_decode`/`json_encode`, `media_capabilities`, `download_file_async`, `mkdir`, and the full `http_request` (headers, all methods, timeouts, redirects, response headers, real cancel). The sandbox also removed `require`, `dofile` and friends in this release. |
| 3 | `play_remote`, `queue_remote_list`, and `provider`/`track_id` on `track_started` |
| 4 | `set_home_layout` (removed in 15) |
| 5 | `reload_ui` |
| 6 | `refresh_theme` |
| 7 | `register_home_tile` and `order` in `set_home_layout` (removed in 15) |
| 8 | `set_launcher_layout` |
| 10 | `background_image` in `set_home_layout` (removed in 15) |
| 11 | `set_hw_volume_curve` |
| 12 | `register_quick_toggle`, `set_quick_toggle` |
| 13 | LED control, `get_volume`, `get_battery`, and the `volume_changed`, `battery_changed`, `suspending` and `system_resumed` events |
| 14 | `show_text_view` with pictures, grid lists, `is_list_showing`, and one long (4095-byte) HTTP header per request |
| 15 | Removed `set_home_layout` and `register_home_tile` (Home has a fixed layout now) with their tokens `ui.home_layout`, `ui.home_tiles` and `ui.home_background`. `set_launcher_layout` no longer takes `mode`: the Music, Stream Media and Wireless menus are always lists, and it only styles them. Plugins also lose access to `<SD card>/.mellowtone/config/`, and ids that differ only in case now collide. |

Functions added since then, such as `show_details`, `fs_usage`, `wifi_ssid`,
`paths` and the radio list, have a capability token instead of a version.
Check a feature with `has_capability` before using it, so the plugin also
runs on a player without it.

## Screens

### `plugin.register_list_item(list_id, label, on_open [, options])`

Adds a row to a native list (see [Where your plugin
appears](#where-your-plugin-appears)). `on_open()` runs when the row is
tapped; open your first screen from there.

`options` may contain the [row options](#row-options) `icon`, `height`,
`width` and `text_size`, plus `group` (up to 32 bytes) to place the row in a
submenu:

- In `music_audio`: `"effects"` (under Sound Effects) or `"profiles"` (under
  Equalizer > Profiles > Download profiles).
- In `display`: `"appearance"` or `"player_layout"` (under those submenus).
- Other values leave the row directly in its list. Older players ignore
  `group`.

A plugin may register several rows. More than 8 rows in one list (all plugins
together) is an error.

### `plugin.register_stream_media_tile(label, on_open [, icon])`

Adds a tile to Stream Media, after Subsonic. Good for streaming and radio
plugins. `icon` is a theme image path, default `"stream_media/radio.png"`. Up
to 5 plugin tiles; the grid doesn't scroll.

### `plugin.register_quick_toggle(id, label, on_change, options)`

Adds an on/off tile to the quick drawer's expanded area (pull the drawer
down, then drag the handle below the first row). Use it for a simple switch;
use a list row for anything that needs a screen.

- `id`: 1 to 39 letters, digits, `.`, `_` or `-`, unique.
- `label`: short caption, about 8 characters fit.
- `on_change(new_value)`: called with the new boolean when tapped.
- `options.icon` (required, under 80 characters): the off-state image.
- `options.icon_selected`: the on-state image. Default: `icon` with
  `_s.png` in place of its extension. It is recolored to the accent color.
- `options.value`: initial state, default `false`.
- `options.on_text`, `options.off_text`: state captions, default `"On"` and
  `"Off"`.
- `options.on_hold()`: called when the tile is held, for example to open the
  plugin's settings.

Up to 4 tiles from all plugins; they fill the drawer's third row in
registration order.

```lua
if plugin.has_capability("ui.quick_toggle") then
    plugin.register_quick_toggle("boost", "Boost", function(on) set_enabled(on) end,
        { icon = "pull_down/fade.png", value = enabled })
end
```

### `plugin.set_quick_toggle(id, value)`

Updates the tile's state without calling `on_change`. Use it when the plugin
changes the setting elsewhere, such as its own settings screen.

### `plugin.show_list(title, items, on_select [, options])`

Opens a list screen and returns a handle (`nil` on players older than API
14).

- `items`: one entry per row. Either a string, or a table
  `{ label = "...", icon = "...", text_size = "...", wrap = true }`.
  - Labels are up to 159 bytes, or 511 with `wrap = true`, which wraps the
    label onto several lines and grows the row instead of scrolling it
    (check `ui.list_wrap`).
  - Up to 500 rows; the rest are dropped.
- `on_select(index)`: called with the 1-based index of the tapped row. Not
  called if the user goes back.
- `options`:
  - `height`, `width`: size of every row (see [row options](#row-options)).
  - `selected`: 1-based row drawn with an accent outline. The outline follows
    later taps.
  - `layout = "grid"`: shows cards instead of rows. Each card shows its `icon`
    in a 2:3 portrait area (fitted, not cropped, like a book cover) with the
    label below in up to two lines; without an icon, the label fills the
    card. `height` and `width` don't apply. Check `ui.list_grid`.
  - `columns`: cards per row in a grid, 2 to 4, default 3.

Calling `show_list` again (for example from `on_select`) opens a new screen
on top, and Back returns to the previous one with its own callback. Up to 4
lists can be stacked; going deeper breaks Back navigation.

`plugin.is_list_showing(handle)` returns `true` while that list is the screen
in front. Check it before opening a screen from a late callback (an HTTP
response, a finished image job), so you don't open it over whatever the user
moved on to. Check `ui.list_showing`.

Rows with icons use the native submenu look (44 px icons, 96 px rows, a blue
gradient that a custom `list_row` color replaces). Text-only lists stay
compact.

### `plugin.show_settings_list(title, items)`

Opens a screen that looks like a native Settings submenu, with real switches
and sliders. Each item is a table with `type` and `label`:

```lua
plugin.show_settings_list("My Plugin", {
    { type = "row",    label = "About",  on_select = function() ... end },
    { type = "toggle", label = "Enable", value = true, on_change = function(on) ... end },
    { type = "slider", label = "Boost",  min = 0, max = 12, value = 3,
      on_change = function(v) ... end },
})
```

- `row`: `on_select()` runs on tap. Open another `show_settings_list` from it
  for a nested submenu.
- `toggle`: `on_change(new_value)` runs on every tap.
- `slider`: integer range `min` to `max`. `on_change(new_value)` runs once,
  when the finger is lifted.
- Every type accepts the [row options](#row-options) `icon`, `width` and
  `text_size`; `height` works on rows and toggles but not sliders. Rows and
  toggles also accept `wrap = true` for labels up to 511 bytes (95 without
  it). Check `ui.settings_list_wrap`.
- Up to 24 items and 4 sliders per screen; extras are dropped. A missing
  `type`, an unknown `text_size`, or a missing callback is an error.
- Up to 2 settings screens can be stacked.

### Row options

These optional fields work in `register_list_item` options, `show_list`
items and `show_settings_list` items. In `show_list`, `height` and `width` go
in the call's `options` so every row matches.

| Field | Meaning |
| --- | --- |
| `icon` | An image shown left of the label, scaled to a fixed size. An absolute path, or a path relative to `<SD card>/.mellowtone/plugins/`. A missing image is skipped, not an error. |
| `height` | Row height in px, clamped to 48 to 220 (below 64 an icon is clipped). Default: 124 for list rows added with `register_list_item`, 84 for plain `show_list` rows. |
| `width` | Row width in px, clamped to 240 to the screen width, kept centered. |
| `text_size` | `"small"`, `"medium"`, `"large"` (with fallback fonts for Cyrillic, CJK, Korean and Thai) or `"mono"` (an ASCII-only pixel font). Any other value is an error. |

### `plugin.show_text_input(title, initial_text, is_password, on_submit)`

Opens the app's keypad text entry (the one used for Wi-Fi passwords).
`on_submit(text)` runs when the user presses Enter; it doesn't run if they go
back. Returns `true`, or `false, "text input busy"` if another request is
open. You can open the next input from inside `on_submit` (for example a
username, then a password).

### `plugin.show_text_view(title, text [, options])`

Opens a paged reader for long text. Returns `true`, or `false, "busy"` if one
is already open, or `false, "unavailable"` during a screen transition.

- `text`: up to 256 KB; longer text is cut.
- `options.page` (1-based) or `options.offset` (byte offset): where to start.
  `page` wins.
- `options.images`: up to 64 picture files (usually made by
  `image_thumbnail_async`). The text shows picture `n` (0-based) where it contains
  `"\27" .. n .. "\27"`. Each picture gets its own page. Check
  `ui.text_view_images`.
- `options.on_turn(page, pages, byte_offset)`: runs shortly after the view
  opens (never inside the `show_text_view` call), after each page turn, and
  when the final page count is known. Use it to save the reading position.
  While the view is still finding the requested `page` or `offset`, it isn't
  called, so a saved position is never overwritten by a temporary one.
- `options.on_close(page, byte_offset)`: runs after the view closes, however
  it was closed. If it closes before reaching the requested `offset`, it
  reports that offset. Not called on a plugin reload.
- `options.font_px` is accepted but ignored; the text uses the Font Size
  setting.

Tap the right third or swipe left for the next page; tap the left third or
swipe right for the previous page. Swiping right on the first page, or the
Back button, closes the view. The footer shows `page / pages`. Pagination runs
in the background, so very long texts open immediately; past 8192 pages the
rest isn't shown.

### `plugin.show_toast(message [, duration_ms])`

Shows a short message at the bottom of the screen. Duration 100 to 30000 ms,
default 5000.

### `plugin.show_details(title, sections)`

Opens a read-only page of key/value cards in the Settings style, for status
and information pages. Check `ui.details`.

```lua
plugin.show_details("Status", {
    { title = "Storage", rows = { { "Free", "12 GB" }, { "Total", "64 GB" } } },
    { title = "Network", rows = { { "Wi-Fi", plugin.wifi_ssid() } } },
})
```

- Each section has an optional `title` and a list of `rows`; each row is
  `{ key, value }`. Numbers are shown as text, and a `nil` value as `-`.
- Up to 64 sections and 320 lines (section titles and rows together); the
  rest is dropped.

## Theming

Colors and layouts aren't saved; only icons copied with `set_icon` stay on
the device. To keep a look across restarts, save the user's choice in your own
file or `plugin.storage` and apply it again from top-level code every time
the plugin loads. `examples/plugins/Theme.lua` does exactly that.

| To apply... | Call |
| --- | --- |
| colors | nothing, they apply immediately |
| icons and launcher layouts | `plugin.refresh_theme()` |
| anything, the heavy way | `plugin.reload_ui()` |

### `plugin.set_icon(theme_path, source_file)`

Replaces a built-in icon. `theme_path` is the icon's path in the theme (for
example `"launcher/music.png"`); `source_file` is the replacement image, which
is copied into the theme override folder. Raises an error if it can't be
copied.

- Call it from top-level code. The app caches icons, and screens built before
  the override existed keep using the old file until `refresh_theme()` or
  `reload_ui()`.
- Keep the replacement close to the original's size and shape: the layout was
  measured for the original.
- The Player (Now Playing) screen's layout is a file in the theme too,
  `player_layouts/cover_first.xml`, so `set_icon` can replace it the same
  way; it applies once the Player screen is rebuilt (`reload_ui()` or a
  restart). How to write one is explained in
  [PLAYER_LAYOUTS.md](PLAYER_LAYOUTS.md).

### `plugin.set_background_color(slot, rgb)` / `plugin.set_text_color(slot, rgb)`

Change colors app-wide, immediately, from anywhere. `rgb` is `0xRRGGBB`.

- Background slots: `"screen"` (every screen), `"card"` (popups and cards),
  `"list_row"` (every list row).
- Text slots: `"primary"` (titles and labels) and `"muted"` (secondary text).
  Red warning text and accent-colored text are not affected.

Another slot name is an error.

### `plugin.set_launcher_layout(options)`

Styles the rows of the Music, Stream Media and Wireless menus, each
independently. Each of `music`, `stream_media` and `wireless` takes a table
with these fields, all optional:

```lua
{
    row_gap = 10,                     -- px between rows (0: the default)
    height = 108, width = 440,        -- row size in px, like the row options
    bg_color = 0x1e3524, text_color = 0xd8c9a3,
    radius = 24,                      -- px, not negative
    align = "center",                 -- "left" (default), "center" or "right"
    accessory = false,                -- hide the chevron
    text_size = "medium",             -- like row options
    icon = false,                     -- hide the row's icon
}
```

`height` and `width` follow the [row options](#row-options); left out, the
rows size themselves. Menus you leave out keep the default style; an empty
call resets all three. Each call replaces the previous one. Apply with `refresh_theme()`.
Errors: a menu that isn't a table, a negative `radius`, an unknown `align` or
`text_size`, or a number out of range. Before API 15 each menu also took
`mode = "tile"` or `"list"`; the field is now ignored.

```lua
plugin.set_launcher_layout({
    music = { height = 108, width = 480, row_gap = 10,
              bg_color = 0x000000, text_color = 0x33FF33, text_size = "mono" },
})
```

### `plugin.refresh_theme()`

Applies icon changes and the launcher layouts, without disturbing
anything else: other screens, navigation, playback and connections stay as
they are. It runs after your callback returns. Use it after a batch of theme
calls.

### `plugin.reload_ui()`

Rebuilds every screen and restarts all plugins, without restarting the app
or interrupting playback and connections. It runs after your callback
returns, and the user lands on Home. Call it only in response to a user
action, never unconditionally from top-level code (that would reload on
every load; a second request during a reload is ignored).

```lua
plugin.register_list_item("display", "Theme", function()
    plugin.show_list("Theme", { "Dark", "White" }, function(index)
        apply_theme(index == 2 and "white" or "dark")  -- your own theme calls
        plugin.refresh_theme()
    end)
end)
```

## Playback

### Playing files

- `plugin.play_file(path)`: plays one file (absolute path) or stream URL as a
  new queue, replacing Up Next like tapping a song does.
- `plugin.play_list(paths [, start_index])`: plays a list of paths or URLs as
  a new queue, starting at the 1-based `start_index` (default 1, out-of-range
  values are clamped). Up to 500 entries.

**Stream URLs** (`http://` or `https://`) play as live streams:

- MP3 by default. Add `#.flac` to the URL for FLAC, or `#.aac` (or `#.aacp`)
  for AAC in ADTS frames; the fragment is never sent to the server. A server
  that answers with an AAC content type also plays as AAC. Other formats fail
  to open.
- No seeking. FLAC streams show their duration; MP3 streams don't.
- No reconnect: if the connection drops, playback stops.
- An endless stream (radio) never auto-advances. A finite FLAC file does.
- Stream titles (ID3 or ICY metadata) are not shown.

### `plugin.play_remote(track)` / `plugin.queue_remote_list(tracks [, start_index])`

For streaming-service plugins: plays one track, or a new queue of up to 500,
with full metadata.

```lua
plugin.play_remote({
    provider = "myservice",       -- required
    track_id = "12345",           -- required, stable for this track
    stream_url = "https://...",   -- required
    verify_tls = true,            -- default true
    title = "Song", artist = "Artist", album = "Album",
    duration_ms = 214000,
    artwork_url = "https://...",
    codec = "flac",               -- "mp3" (default), "flac" or "aac"
    sample_rate = 44100, bit_depth = 16, channels = 2, bitrate_kbps = 0,  -- display only
    replaygain_db = -3.5,
})
```

- Favorites and play counts use `remote://<provider>/<track_id>`, not the URL,
  so they add up even when the URL changes on every play. They don't appear
  in the Favorites and Most Played screens yet.
- Remote tracks are not resumed after a restart.
- Playback works like a stream URL above: no seeking, no reconnect.

### Controls

| Function | Does |
| --- | --- |
| `plugin.toggle_pause()` | Play/pause, like the button (blocked in Bluetooth DAC and AirPlay modes, like the button). |
| `plugin.stop()` | Stops playback. |
| `plugin.next_track()`, `plugin.prev_track()` | Next and previous, respecting shuffle. |
| `plugin.seek(seconds)` | Jumps to a position in the current track. |
| `plugin.set_volume(percent)` | Sets the volume, 0 to 100, and shows the volume popup. |
| `plugin.set_transport_skip(dir, seconds)` | Makes Next and Previous skip `seconds` (1 to 300) within files under `dir` instead of changing track; 0 turns it off. Applies to every Next/Previous control, not to automatic track changes. Not saved; the last call wins. Check `playback.transport_skip`. |

### Current state

| Function | Returns |
| --- | --- |
| `plugin.is_playing()`, `plugin.is_paused()` | booleans |
| `plugin.get_position()`, `plugin.get_duration()` | seconds |
| `plugin.get_volume()` | 0 to 100 |
| `plugin.get_now_playing()` | `title, artist, album, duration_seconds`, or `nil` if nothing has played yet |
| `plugin.get_current_track_path()` | absolute path, or `nil` when nothing is loaded |
| `plugin.get_play_mode()` | `"sequential"`, `"repeat_all"`, `"repeat_one"` or `"shuffle"` |

### `plugin.media_capabilities()`

Describes what the player can play, for streaming plugins choosing a quality:

```lua
-- codecs = {"mp3", "aac", "flac"}, containers = {"mp3", "adts", "flac"}
-- max_bit_depth = 16, max_channels = 2
-- max_sample_rate = 0 (unknown), direct_http_streaming = true
-- range_seeking = false, hls = false, dash = false
-- encryption_modes = {}, drm_systems = {}
```

## Music library

The library functions read the on-device database. Matching is
case-insensitive.

- `plugin.get_artist_albums(artist)`: the artist's album names, sorted, or
  `nil`.
- `plugin.get_album_tracks(artist, album)`: the album's track paths in album
  order, or `nil`.
- `plugin.get_next_album_tracks(artist, album)`: the tracks of the artist's
  next album, or `nil` if there is none (it doesn't wrap). Useful to keep
  playing through a discography.

### Paged access

For browsing the whole library without loading it all. `limit` defaults to,
and is capped at, 200; use `offset` to page. Check `library.paged`.

- `plugin.library_song_count()`: number of songs.
- `plugin.library_get_songs([offset], [limit], [filters])`: returns
  `songs, total`. `filters` may contain `query` (part of the title or artist),
  `artist`, `album_artist` and `album` (exact matches). `total` counts all
  matches, for "page N of M".
- `plugin.library_search(query, [limit])`: songs whose title or artist
  contains `query`.
- `plugin.library_get_song(id)`: one song, or `nil` if it no longer exists.
- `plugin.library_get_artists([offset], [limit])`: artist groups.
- `plugin.library_get_albums([offset], [limit], [artist])`: album groups,
  optionally only those whose artist or album artist is `artist`.

A **song** is `{ id, path, title, artist, album, album_artist }`. Its `id`
survives rescans: keep it to look the song up later with
`library_get_song(id)`. A **group** is
`{ name, count, first_song_id, album_artist }`; `album_artist` tells apart
same-named albums by different artists.

```lua
local songs, total = plugin.library_get_songs(0, 50, { artist = "Boards of Canada" })
plugin.show_toast(("Found %d songs"):format(total))
```

### `plugin.refresh_library()`

Rescans the music library, like Settings > Library > Update Music Database,
with the same progress screen. Needed after a plugin adds music files, since
the library doesn't watch for changes. Returns `true, "started"`, or
`false, "already_running"` or `false, "rate_limited"` (one scan per minute per
plugin). Check `library.refresh`.

## Files and playlists

- `plugin.sd_root()`: the SD card's path (`/mnt/sd_0` on the R3 Pro,
  `/data/mnt/sd_0` on the other boards, `./music` on the simulator). Build
  every path from it so the plugin works everywhere.
- `plugin.list_dir(path)`: the entries of a folder (absolute path), as
  `{ name, dir, size, modified }` tables (`size` and `modified`, in Unix
  seconds, when available). Hidden entries are skipped, the order isn't
  sorted, and a missing folder gives an empty table.
- `plugin.mkdir(path)`: creates a folder and any missing parents. Returns
  `true`, or `nil, error`. An existing folder is fine.
- `plugin.fs_usage(path)`: `{ total, free }` in bytes for the file system
  holding `path` (the SD card, for example), or `nil, error`. Check
  `filesystem.usage`.

### Playlists

These work on the `.m3u` playlists the Playlists screen shows, and changes
appear there immediately.

| Function | Returns |
| --- | --- |
| `plugin.playlist_list()` | paths of all playlists, or `nil` if there are none |
| `plugin.playlist_read(m3u_path)` | song paths in order, or `nil` |
| `plugin.playlist_create(name, song_path)` | the new playlist's path (with `song_path` as its first song), or `nil` |
| `plugin.playlist_add(m3u_path, song_path)` | `true`, or `nil, error` |
| `plugin.playlist_remove(m3u_path, song_path)` | `true`, or `nil, error`; removes every copy of the song |
| `plugin.playlist_delete(m3u_path)` | whether it was deleted, or `nil, error` for an invalid path |

## Sound

### Equalizer

These control the player's 10-band parametric EQ. Every change applies and is
saved immediately, and the EQ screen shows it.

| Function | Does |
| --- | --- |
| `plugin.eq_load_profile(path)` | Loads a `.peq` profile. Returns `false` if it can't be read. |
| `plugin.eq_save_profile(path)` | Saves the current EQ as a `.peq` profile, the same format as the EQ screen. Returns `false` if it can't be written or the path is longer than 500 bytes. |
| `plugin.eq_reset()` | Restores the defaults. |
| `plugin.eq_set_bypass(on)` | `true` turns the whole EQ off. |
| `plugin.eq_set_preamp(db)` | Sets the preamp gain. |
| `plugin.eq_set_band(index, freq_hz, gain_db, q)` | Sets one band. `index` is 1 to 10. |
| `plugin.eq_set_band_type(index, type)` | `"peaking"`, `"low_shelf"` or `"high_shelf"`. |
| `plugin.eq_set_band_enabled(index, on)` | Turns one band on or off. |

There are no getters. A bad index or type is an error. Since every call writes
to storage, change only the bands that changed.

### `plugin.set_hw_volume_curve(curve)`

Replaces how the volume slider maps to the internal DAC, for example to
recreate the stock firmware's Low and High Gain modes. `curve` is a table of
exactly 101 integers (volume 0% to 100%), each a raw register value from 0
(loudest) to 255 (quietest). `nil` restores the built-in curve. It applies
immediately but isn't saved, so set it again on every load. It affects only
the headphone and line output, not Bluetooth or USB.

## Device

### LEDs

Plugins can drive the red and blue charge LEDs while the player is awake.
First check `plugin.led_available()`.

| Function | Does |
| --- | --- |
| `plugin.led_set(color, on_or_level)` | `color` is `"red"` or `"blue"`. `true` (full), `false` (off) or a level 0 to 100. Works even when the LED Indicator setting is off. |
| `plugin.led_blink(color, on_ms, off_ms [, level])` | Blinks. Each phase is clamped to 50 to 60000 ms. |
| `plugin.led_breathe(color, rate [, level])` | Pulses. `rate` up to 600 means breaths per minute; above 600, a period in ms (up to 120000). `level` is the peak, default 100. |
| `plugin.led_get(color)` | `{ mode, level }`, `mode` being `off`, `on`, `blink`, `breathe` or `status`. |
| `plugin.led_status([color])` | Returns one LED (or both) to showing the charge status. |
| `plugin.led_release()` | Gives both LEDs back to the player. |

Levels are clamped to 0 to 100 and default to 100 for effects. The last
plugin to use the LEDs owns them until it releases them, fails, or
the plugins reload. Effects pause during suspend and resume afterwards.

```lua
if plugin.led_available() then
    plugin.led_blink("red", 300, 700, 80)
    -- later:
    plugin.led_release()
end
```

### Battery

`plugin.get_battery()` returns `{ level, charging, full, external_power }`,
`level` being 0 to 100 (0 without a battery). See also the `battery_changed`
event.

## Network

### `plugin.http_request(options, callback)`

The way to make HTTP requests. It returns a handle immediately, the request
runs in the background, and `callback(status, body, error, headers)` runs when
it finishes. On success `error` is `nil` and `headers` is a table of response
headers; on failure `status` and `body` are `nil` and `error` says why.

If the request can't start, it returns `nil, error` instead (for example
`"too many active HTTP requests"`), and the callback never runs.

```lua
local handle = plugin.http_request({
    url = "https://example.com/api",       -- the only required field
    method = "POST",                       -- GET (default), POST, PUT, PATCH, DELETE, HEAD
    headers = { Authorization = "Bearer " .. token },
    body = '{"hello":"world"}',            -- sent only with POST, PUT and PATCH
    content_type = "application/json",
    verify_tls = true,                     -- default true
    max_response_bytes = 262144,           -- default 512 KB, at most 2 MB
    connect_timeout_ms = 10000,            -- timeouts: none by default, at most 5 minutes
    read_timeout_ms = 15000,
    total_timeout_ms = 30000,
    redirect_limit = 5,                    -- default 0, at most 10
}, function(status, body, err, headers)
    if err then plugin.show_toast(err) return end
    plugin.show_toast("HTTP " .. status .. ", " .. #body .. " bytes")
end)
```

Limits and rules:

- Up to 4 requests at a time, from all plugins together.
- Request body up to 1 MB. Up to 32 headers, names up to 63 bytes, values up
  to 767 bytes, plus one value per request up to 4095 bytes (for long tokens;
  not for Host, User-Agent, Content-Type, Content-Length or Connection). Going
  over a limit, or a value out of range, raises an error instead of being cut.
- Without `redirect_limit`, a 3xx response is returned as a normal result.
  Redirects follow browser rules (303, and 301/302 after a POST, become a GET
  without body), and credentials (`Authorization`, `Cookie`, `Cookie2`,
  `Proxy-Authorization`) are never sent to a different origin (scheme, host
  or port). A redirect from
  HTTPS to HTTP is refused while `verify_tls` is on.
- If a response repeats a header, the last one wins.

Error values: `invalid_url`, `invalid_request` (a line break in the URL or a
header), `dns_failure`, `connect_failed`, `connect_timeout`, `tls_failure`,
`timeout`, `cancelled`, `too_many_redirects`, `response_too_large`,
`malformed_response`, `io_error`, `insecure_redirect`. They are stable, so you
can compare against them.

`plugin.cancel(handle)` stops a request (or download) and returns whether it
was still running. Its callback is then not called. The connection closes at
once, except while the host name is being looked up, which finishes first.

### `plugin.download_file_async(url, dest_path [, verify_tls], callback)`

Downloads a file straight to disk in the background, without loading it into
memory. `verify_tls` defaults to `true`. It returns a handle, or `nil, error`
if it can't start. `callback(saved_path, error)` runs when it ends; on failure
it gets `nil, "download failed"`. The file only appears at `dest_path` after
a complete HTTP 2xx response, so a failed download never leaves a broken file
or replaces an existing one. It shares the 4 request slots and can be
cancelled with `plugin.cancel()`.

```lua
local dir = plugin.sd_root() .. "/Talks"
if plugin.mkdir(dir) then
    plugin.download_file_async("https://example.com/talk.mp3", dir .. "/talk.mp3",
        function(path, err) plugin.show_toast(err or ("Saved " .. path)) end)
end
```

### `plugin.http_get(url [, verify_tls])` / `plugin.http_post(url, body [, content_type] [, verify_tls])`

Older, blocking requests: the whole player freezes until they finish. Use
`http_request` instead. They return `status, body` (HTTP error codes such as
404 included), or `nil, "network error"`. `http_post` sends `body` as is, with
`content_type` defaulting to `application/x-www-form-urlencoded`.

### `plugin.wifi_ssid()`

Returns the name of the Wi-Fi network the player is connected to, or `nil`.
Check `network.wifi_ssid`.

## Data and storage

### Radio list (`radio.list`)

The Radio screen shows the built-in station list (read-only, in the install
directory) minus the stations you removed, then your own stations from
`<SD card>/Radio.txt`. Change it through these functions rather than by
editing the files:

```lua
for _, st in ipairs(plugin.radio_list()) do     -- { name =, url =, base = bool }
    print(st.name, st.url, st.base and "built-in" or "yours")
end
local ok, err = plugin.radio_add("My Station", "http://example.com/stream.mp3")
local ok, err = plugin.radio_remove("http://example.com/stream.mp3")
```

`radio_add` appends to `Radio.txt` (a removed built-in station is shown
again instead); `radio_remove` deletes your station or hides a built-in one
("Show removed stations" on the Radio screen brings it back). Both return
`true`, or `nil` and a reason (`"no writable SD card"`, `"already in the
Radio list"`, ...). Check `plugin.has_capability("radio.list")` first.

### Player paths (`filesystem.paths`)

`plugin.paths()` returns `{ sd_root =, state_root =, install_root = }`: the SD
card, the player's state on it (`<SD card>/.mellowtone`, with `config/`,
`logs/`, `plugins/`, ...) and the player's install directory in flash, which
plugins may read but not write.

### `plugin.storage` and `plugin.secrets`

Private key-value storage for your plugin (keyed by its `id`, so call
`plugin.define()` first), kept on the SD card in
`.mellowtone/plugin-data/<id>/`. Without a writable SD card nothing is saved
(`set` fails).

```lua
plugin.storage.set("last_sync", tostring(os.time()))
local last = plugin.storage.get("last_sync", "never")   -- with a default
local keys = plugin.storage.list("cache_")              -- keys starting with "cache_"
plugin.storage.delete("last_sync")

plugin.secrets.set("access_token", token)
local token = plugin.secrets.get("access_token")         -- nil if missing
if plugin.secrets.exists("access_token") then ... end
plugin.secrets.delete("access_token")
```

- `get(key [, default])` returns `default` (or `nil`) for a missing key.
  `list([prefix])` lists all keys when the prefix is omitted or empty.
- Values are strings (binary is fine), up to 256 KB each. Setting the same
  value again doesn't write to storage again.
- `set` and `delete` return `true` on success. `false` can mean a limit was
  reached, or that the change was made but couldn't be confirmed as safe from
  a sudden power loss, so don't retry in a loop.
- Limits: 2 MB and 500 keys per plugin (storage and secrets together), 8 MB
  for all plugins.
- `secrets` is for tokens and passwords. It has `get`, `set`, `exists` and
  `delete` but no `list`, so secret names can't be enumerated. `get` returns
  `nil` for a missing secret (it was added later: check
  `storage.secrets_get`). Only your plugin can read its secrets, but they are
  **not encrypted**: anyone with root access to the device can. Don't describe
  them as encrypted to your users.

### `plugin.json_decode(text [, limits])` / `plugin.json_encode(value [, limits])`

Convert between JSON and Lua. They return `value` or `nil, error`.

| Limit | Default and maximum |
| --- | --- |
| `max_input_bytes` (decode) / `max_output_bytes` (encode) | 512 KB |
| `max_nesting` | 32, at most 64 |
| `max_entries` | 10000 |

Larger values are lowered to the maximum. Data nested deeper than
`max_nesting` fails with `nil, "json too deeply nested"`. Keep in mind:

- JSON `null` becomes `nil`, so it looks like a missing key.
- Numbers are exact only up to 2^53: treat large ids as strings.
- A table with items `1..n` becomes a JSON array; any other table, even an
  empty one, an object. Object keys must be strings.
- Invalid UTF-8 is an error.

### `plugin.md5(text)`

Returns the MD5 hash of `text` in lowercase hex, for APIs that sign requests
this way (like Last.fm).

### `plugin.image_thumbnail_async(source, dest, max_w, max_h, callback)`

Makes a thumbnail from a JPEG, PNG or BMP file of up to 3 MB. Check
`data.image_thumbnail`.

It scales the image to fit `max_w` x `max_h` (16 to 800), never enlarging or
cropping it, and writes it to `dest`, which must end in `.bin`. The result can
be used as a `show_list` icon or a `show_text_view` picture. It runs in the
background and calls `callback(dest)` or `callback(nil, reason)`.

- It returns `true`, or `nil, "busy"` while another thumbnail job (from any
  plugin) runs: try again a bit later.
- Failure reasons include `"unsupported image"`, `"could not decode image"`,
  `"could not write image"`, `"could not open image"`, `"not a regular
  file"`, `"image too large"` and `"could not read image"`.
  `"busy"` and `"nomem"` are temporary (the player was decoding artwork), so
  don't treat the image as broken.
- `dest` is replaced only on success. Jobs are cancelled, without callback,
  when the plugins reload.

## Events and timers

### `plugin.on(event, callback)`

Runs `callback` whenever something happens. Every subscribed plugin is
called. Up to 8 subscribers per event; an unknown event name is an error.

| Event | Arguments | When |
| --- | --- | --- |
| `track_started` | `title, artist, album, duration_seconds, provider, track_id` | A new track starts, for any reason. `provider` and `track_id` are `""` except for `play_remote` tracks. |
| `paused`, `resumed` | none | Playback pauses or resumes. |
| `stopped` | none | Playback stops (not when a queue simply ends; see `queue_exhausted`). |
| `queue_exhausted` | `direction` (`1` next, `-1` previous) | Next or Previous is pressed with no track left in that direction. |
| `screen_woke` | none | The screen turns back on. |
| `volume_changed` | `percent` | The volume changes, at most twice a second. |
| `battery_changed` | same table as `get_battery()` | A battery value changes. |
| `suspending` | none | The player is about to sleep (it can't be stopped). |
| `system_resumed` | none | The player woke from sleep. |

### `plugin.set_interval(seconds, callback)` / `plugin.clear_interval(handle)`

Runs `callback` every `seconds` (at least 1) until cleared. Returns a handle;
clearing an unknown or already cleared handle does nothing. Up to 8 timers
from all plugins together. Callbacks run on the interface thread, so keep
them quick.

## Examples

All in `examples/plugins/`, each a short file on one topic:

| Example | Shows |
| --- | --- |
| `HelloPlayer.lua` | `define`, a settings row, `show_list`, `show_toast` |
| `SettingsDemo.lua` | `show_settings_list` with a toggle, a slider and a nested screen; `plugin.storage` |
| `PlaybackEvents.lua` | Events, a timer, the playback state and controls |
| `HttpDemo.lua` | `http_request`, `cancel`, `json_decode`/`json_encode`, `is_list_showing` |
| `Theme.lua` | A theme picker: colors, launcher layouts, icons, a saved choice, `refresh_theme` and `reload_ui`. It loads `.theme` files; `Themes/Earthy.theme` is one, and `Theme.lua` describes the format. |
| `ApiInfo.lua` | `api_version`, `has_capability`, `has_feature`, `board`, `get_app_info`, `show_details` |

## Testing and debugging

1. Write the plugin in any text editor. `luac -p MyPlugin.lua` catches syntax
   errors before copying.
2. Copy it to `<SD card>/.mellowtone/plugins/`.
3. Tap **Refresh Plugins** in Settings > System > Plugin Manager (this reloads
   every plugin and returns to Home), or restart.
4. Read the player's output in `<SD card>/.mellowtone/logs/player.out` (the
   previous run's is `player.prev.out`), or follow it over ADB.

Errors look like this:

| Message | Meaning |
| --- | --- |
| `[plugins] failed to load <path>: <error>` | A syntax error or an error in the top-level code. |
| `[plugins] <context> error: <error>` | A callback failed; `<context>` names which one. |

Plugin Manager can switch each plugin off. Switching one off stops it from
loading; it doesn't undo what it already changed. Copied icons and EQ settings,
for example, stay until changed again.

## Extending the API (C developers)

To add a function plugins can call, follow the existing `l_plugin_*`
functions in `src/plugins/plugin_manager.c`: read the arguments with
`luaL_check*`/`luaL_opt*`, do the work, push the results, return their
count, and add the function to `plugin_funcs[]`.

- If it touches screens, widgets or other UI state, go through a
  `gui_plugin_*` function in `src/ui/gui_plugins.c` (declared in `gui.h`), like `show_list`,
  `play_file` and the playback controls do.
- If it uses a self-contained module with no UI (`peq.c`, `http_client.c`),
  call it directly, like the `eq_*` and HTTP functions do.

When unsure, look at what the native UI does around the same call. If it also
updates icons, saves state or checks a condition, that belongs in the `gui_plugins.c`
bridge too, so a plugin can't skip it.

Add a capability token to `plugin_capabilities[]` so plugins can detect the
new feature, and document it here.
