# Player layouts

The Now Playing screen is an LVGL XML file that says what the screen looks
like; the app takes care of what it does: playback, seeking, cover art,
lyrics, theme colors (`src/ui/player_layouts.c`, `src/ui/gui_player.c`).

One layout ships: `player_layouts/cover_first.xml` under the theme root
(`assets/themes/player_layouts/` in this repository). A theme override with
the same path replaces it; a plugin installs one with `plugin.set_icon`
([PLUGINS.md](PLUGINS.md)). When the XML cannot be loaded, the player falls
back to its own layout written in C.

### Screen sizes

The players have different screens: R3 Pro is 360x480, R1 is 480x800,
R3 Pro II is 480x720 and R3 II 2025 is 320x480. A layout drawn in pixels for
the 360x480 panel starts with the comment
`<!-- mellowtone:design 360x480 -->`. On every other screen the loader then
scales its plain integer values: `x`, `width`, `height`, radius, padding,
margins, `<px>` constants and the `style_width`, `style_height` and
`style_translate_x` forms by screen width / 360; `y` and `style_translate_y`
by screen height / 480. The built-in `cover_first` layout works this way. A layout without the
comment is used as written, so build it with percentages and flex rows.

## Writing the XML

A layout is one LVGL XML `<component>`: optional `<consts>` and `<styles>`,
plus a `<view>` holding the widgets. The format is described
in LVGL's XML documentation for version 9.4 (https://docs.lvgl.io/9.4/). Things to know:

- The app uses the **LVGL 9.4** XML engine, so follow the 9.4 docs.
- Use LVGL's own widgets (`lv_obj`, `lv_label`, `lv_image`, `lv_slider`,
  `lv_button`, `lv_bar`, `lv_arc` and so on). A layout cannot use other
  components or other XML files.
- XML cannot call app code. Interactive widgets work through the names below.
- Sizes are in pixels. Use `#screen_w`, `#screen_h`, percentages and flex
  layouts to adapt to the screen.

### Ready-made fonts, images and sizes

These names can be used directly:

- **Fonts:** `player_meta` (the built-in Player's artist font), `font_16` and
  `font_22` (these follow the Font Size setting and a custom font), and
  `lv_font_default`. Example: `style_text_font="font_22"`.
- **Images:** the Player's own icons, which also follow theme overrides:
  `default_cover`, `btn_play`, `btn_prev`, `btn_next`, `ic_more`,
  `collect_out` and `order`. Example: `<lv_image src="btn_next"/>`. These
  names don't work for `bg_image_src` inside `<styles>`; use an `<lv_image>`
  widget there instead.
- **Sizes:** `screen_w` and `screen_h`, the screen size in pixels. Example:
  `width="#screen_w"`.

Anything you declare in your own `<consts>`, `<fonts>` or `<images>` takes
priority over these names.

## Widgets the app looks for

The app finds widgets by their `name` attribute and brings them to life. The
rest of your layout is decoration.

**Required.** Without these the layout is rejected and the default one is
used:

| Name | Type | What it does |
| --- | --- | --- |
| `cover_card` | any | Frame for the cover art. Give it a size, rounded corners, clipping. |
| `cover_img` | `lv_image` | The cover art. Put it inside `cover_card`; it is scaled to fill the card. Tapping it opens and closes lyrics. |
| `title` | `lv_label` | Song title. Scrolls when too long (on the R3 Pro: one line ending in "…"). |
| `play_btn` | `lv_image` | Play/pause. Its icon switches automatically and takes the accent color. |
| `progress_slider` | `lv_slider` | Seek bar. |

**Optional.** Leave out what you don't want:

| Name | Type | What it does |
| --- | --- | --- |
| `artist`, `album` | `lv_label` | Artist (or folder name) and album. Scroll when too long (on the R3 Pro: one line ending in "…"). Tapping opens the matching local artist or album when the playing path is in the library. |
| `pos_label`, `dur_label` | `lv_label` | Elapsed and total time. |
| `song_count` | `lv_label` | Position in the queue, for example "3/12". |
| `format_badge` | `lv_label` | Codec, bit depth and sample rate. |
| `quality_pill` | any | Container around the format badge. |
| `favorite_circle` | any | Background of the heart button. Tapping it toggles the favorite. |
| `favorite_icon` | `lv_image` | The heart. Switches between empty and full. |
| `order_btn` | `lv_image` | Play mode (in order, repeat, repeat one, shuffle). Tap to change. |
| `prev_btn`, `next_btn` | `lv_image` | Previous and next. Hold to seek. |
| `more_btn` | `lv_image` | Opens the song menu. |
| `dismiss_btn` | any | Leaves the Player, like Back. |
| `overlay_panel` | any | Full-screen surface the blurred cover background is drawn on. |
| `background_img` | `lv_image` | The blurred cover, inside `overlay_panel`. |
| `volume_slider` | `lv_slider` | Shows the volume. Display only, it can't be dragged. |
| `play_circle` | any | Container around `play_btn`. Its whole rectangle is included in the play button's touch target. |

A widget with the right name but the wrong type is ignored (for a required
one, the layout is rejected).

### What you get for free

- **Theme colors.** Labels get the theme's text colors, and the slider, heart
  outline and play button get the accent color. These are applied after the
  layout's `<styles>`, so to use your own color set it directly on the widget,
  for example `style_text_color="0xFFFFFF"`.
- **Bigger touch areas.** Small buttons get an invisible touch area of at
  least 44x44 pixels around them. It is placed once, when the screen is built.
- **Scrolling text.** `title`, `artist` and `album` scroll when the text is
  too long (the R3 Pro cuts them with "…" instead). Give them a fixed width (for example `width="100%"`).
  Use `height="content"` to keep the full font height available, including
  at larger font settings, so the marquee scrolls horizontally.
- **Artist and album taps.** Keep the `artist` and `album` labels outside the
  cover image's hit area: tapping the cover toggles lyrics, while tapping a
  metadata label opens its matching local library group. Streams and tracks
  that are not indexed locally leave these labels inert.
- **The blurred background.** If you leave out `overlay_panel` and
  `background_img`, the app adds hidden ones behind everything. To see the
  blurred cover, keep whatever sits on top of them transparent.
- **Song info for other features.** If you leave out `artist` or `album`, the
  app keeps hidden copies, so Remote Control and the quick drawer still show
  them.

## The lyrics view

Tapping the cover opens the lyrics: the cover shrinks to the top-left corner,
the song info moves next to it, the controls hide, and the lyrics fill the
space below. Lines are centered, normal lines gray and the current line in the
accent color; the text size is the Lyrics Text Size setting.

`assets/themes/player_layouts/cover_first.xml` is a complete layout.

## If something goes wrong

- **The C layout shows instead of the XML one.** The file failed to load, or a
  required widget is missing or has the wrong type. The reason is printed to
  the app's error output (stderr). On the simulator, run it from a terminal
  to see it.
- **Part of the screen is cut off on one model.** Use percentages and flex
  rows, or draw for 360x480 with the design comment so the loader scales it.
- **An attribute seems to do nothing.** LVGL ignores attributes it doesn't
  know, and in release builds it doesn't warn about them. Check the spelling
  against the 9.4 XML docs.

## Notes

- The XML engine adds about 220 KB to the app.
- The app sizes `background_img` to the full screen and places it at the
  top-left of its parent, so put it in a full-screen container at the
  top-left of the screen.
