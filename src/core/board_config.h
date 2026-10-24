#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Board-specific constants selected by the board's define (boards/<name>/
 * board.cmake: MT_BOARD_DEFINE -> -DBOARD_R1 / -DBOARD_R3PROII / ...). Screen dimensions here are
 * real, device-confirmed values, not placeholders:
 *   - R1: 480x800, this codebase's own long-standing confirmed value.
 *   - R3PRO II: 480x720, confirmed via the extracted stock firmware dump
 *     (usr/resource/config.json's own "type":"screen","hor":480,"ver":720
 *     entry, independently corroborated by etc/logo.jpeg/logo1.jpeg/
 *     logo2.jpeg all measuring exactly 480x720 -- the same evidentiary
 *     standard the old bootloader splash relied on:
 *     the boot splash is authored to exactly match the real panel).
 *   - R3II 2025: extracted v1.3 firmware's gt9xx_touch.sh sets the touch
 *     limits to 320x480, corroborated by its 320x480 boot logos. */
#if defined(BOARD_R3PRO)
  /* HiBy R3 Pro (X1000): /sys/class/graphics/fb0 virtual_size 360,480, 32 bpp. */
  #define BOARD_SCREEN_WIDTH  360
  #define BOARD_SCREEN_HEIGHT 480
#elif defined(BOARD_R3PROII)
  #define BOARD_SCREEN_WIDTH  480
  #define BOARD_SCREEN_HEIGHT 720
#elif defined(BOARD_R3II_2025)
  #define BOARD_SCREEN_WIDTH  320
  #define BOARD_SCREEN_HEIGHT 480
#else // default to R1 config (though BOARD_R1 could be checked)
  #define BOARD_SCREEN_WIDTH  480
  #define BOARD_SCREEN_HEIGHT 800
#endif

/* Player (Now Playing) screen composition -- a full-bleed cover-art image,
 * top-aligned, plus a bottom-aligned gradient/controls overlay panel that
 * exactly fills the remaining screen height below it, with no gap or
 * overlap. Both pieces are real, fixed-size PNG assets shipped in each
 * device's OWN stock firmware (usr/resource/litegui/theme2/playing_plane/
 * default_cover_565.png and buttom.png -- see assets.c's THEME_ROOT), not
 * something this app generates, so these constants must track each
 * device's own real asset dimensions exactly, not an arbitrary split:
 *   - R1: default_cover_565.png is 480x480, buttom.png is 480x320
 *     (480+320=800, this codebase's own long-standing values).
 *   - R3Pro II: confirmed directly from the extracted stock firmware's own
 *     theme2 assets -- default_cover_565.png measures 480x460, buttom.png
 *     measures 480x260 (460+260=720). HiBy's own stock UI already solved
 *     this exact layout problem for this panel; these numbers are read
 *     from their asset files, not derived.
 *   - R3II 2025: Confirmed by referencing buttom.png from v1.3 of
 *     r3ii_2025.upt (320x170) and subtracting that from the screen height */
#if defined(BOARD_R3PRO)
  /* stock theme2/play_plane: default_cover.png 360x360, buttom_bg.png 360x120 */
  #define BOARD_PLAYER_COVER_HEIGHT 360
  #define BOARD_PLAYER_OVERLAY_HEIGHT 120
#elif defined(BOARD_R3PROII)
  #define BOARD_PLAYER_COVER_HEIGHT 460
  #define BOARD_PLAYER_OVERLAY_HEIGHT 260
#elif defined(BOARD_R3II_2025)
  #define BOARD_PLAYER_COVER_HEIGHT 310
  #define BOARD_PLAYER_OVERLAY_HEIGHT 170
#else
  #define BOARD_PLAYER_COVER_HEIGHT 480
  #define BOARD_PLAYER_OVERLAY_HEIGHT 320
#endif

/* Widget pixel sizes throughout the src/ui GUI sources (button/slider/roller/
 * swatch widths & heights, spacing, etc.) were authored against a 480px-wide
 * reference panel -- both the R1 and R3PRO II screens are 480 wide, so
 * those literals have always rendered correctly on them. The R3II 2025
 * panel is only 320 wide (2/3 of that reference), so the same literal
 * pixel sizes read as oversized there.
 *
 * Rather than hand-tune every call site per board, route any such
 * reference-width-relative literal through BOARD_SCALE_PX() at its
 * definition/call site. It scales proportionally to how much narrower the
 * real panel is than the 480px reference, so it is a no-op on R1/R3PRO II
 * (BOARD_SCREEN_WIDTH == BOARD_REFERENCE_WIDTH) and shrinks automatically
 * for any future narrower board without adding another per-board branch.
 * Rounds to the nearest pixel rather than always truncating down. */
#define BOARD_REFERENCE_WIDTH 480
/* Panels narrower than the 480px reference (R3II 2025 at 320, R3 Pro at 360)
 * use the compact font and transport-icon sizing. */
#define BOARD_NARROW_PANEL (BOARD_SCREEN_WIDTH < BOARD_REFERENCE_WIDTH)
#define BOARD_SCALE_PX(px) \
  ((px) >= 0 \
    ? (((px) * BOARD_SCREEN_WIDTH + (BOARD_REFERENCE_WIDTH / 2)) / BOARD_REFERENCE_WIDTH) \
    : (((px) * BOARD_SCREEN_WIDTH - (BOARD_REFERENCE_WIDTH / 2)) / BOARD_REFERENCE_WIDTH))

/* Vertical layout coordinates and heights were authored against an 800px-tall
 * reference panel (the R1). Because R1 and R3 Pro II are both 480 wide,
 * BOARD_SCALE_PX() evaluates to a 1:1 ratio between them and cannot express
 * a height difference. The R3 Pro II panel is only 720 tall, so vertical
 * coordinates need their own scale.
 *
 * BOARD_SCALE_PY() scales proportionally to the panel height relative to the
 * 800px reference height, rounding to the nearest pixel. It is a no-op on R1
 * (BOARD_SCREEN_HEIGHT == BOARD_REFERENCE_HEIGHT, 800/800) and scales
 * proportionally on shorter panels such as R3 Pro II (720) and R3II 2025 (480). */
#define BOARD_REFERENCE_HEIGHT 800
#define BOARD_SCALE_PY(px) \
  ((px) >= 0 \
    ? (((px) * BOARD_SCREEN_HEIGHT + (BOARD_REFERENCE_HEIGHT / 2)) / BOARD_REFERENCE_HEIGHT) \
    : (((px) * BOARD_SCREEN_HEIGHT - (BOARD_REFERENCE_HEIGHT / 2)) / BOARD_REFERENCE_HEIGHT))

/* The modern UI (clock home, grouped settings lists,
 * bottom sheets, Now panel, stream home, retag, radio, the cover_first
 * player) was designed on the R3 Pro's 360x480 panel, and its literals are
 * 360x480 pixels. BOARD_UI_PX() scales a horizontal size, or any size that
 * should keep its proportions (rows, icons, radii, paddings, font sizes), by
 * panel width / 360; BOARD_UI_PY() scales a vertical position on a
 * full-screen layout by panel height / 480. Same rounding as
 * BOARD_SCALE_PX(): on the R3 Pro both are the identity, so its layout is
 * unchanged pixel for pixel. */
#define BOARD_UI_DESIGN_WIDTH 360
#define BOARD_UI_DESIGN_HEIGHT 480
#define BOARD_UI_PX(px) \
  ((px) >= 0 \
    ? (((px) * BOARD_SCREEN_WIDTH + (BOARD_UI_DESIGN_WIDTH / 2)) / BOARD_UI_DESIGN_WIDTH) \
    : (((px) * BOARD_SCREEN_WIDTH - (BOARD_UI_DESIGN_WIDTH / 2)) / BOARD_UI_DESIGN_WIDTH))
#define BOARD_UI_PY(px) \
  ((px) >= 0 \
    ? (((px) * BOARD_SCREEN_HEIGHT + (BOARD_UI_DESIGN_HEIGHT / 2)) / BOARD_UI_DESIGN_HEIGHT) \
    : (((px) * BOARD_SCREEN_HEIGHT - (BOARD_UI_DESIGN_HEIGHT / 2)) / BOARD_UI_DESIGN_HEIGHT))

/* Filesystem layout (compile-time strings: they are pasted into path
 * literals across the player; the descriptor's paths.install_root/sd_root
 * carry the same values and board.c / tests/board check they agree). The
 * X1600 boards mount the SD card themselves under /data/mnt/sd_0 and keep app
 * data in /usr/data. The R3 Pro's stock init mounts the card at /mnt/sd_0
 * (exFAT) and its writable partition is /data (ubifs); its rootfs is nearly
 * full, so nothing may be written under /usr.
 *
 * Everything else that differs per board is run-time data in the board
 * descriptor (board.h, boards/<name>/board.c). */
#if defined(BOARD_R3PRO)
  #define BOARD_SD_ROOT   "/mnt/sd_0"
  #define BOARD_SD_PARENT "/mnt"
  #define BOARD_DATA_ROOT "/data/mellowtone"
#else
  #define BOARD_SD_ROOT   "/data/mnt/sd_0"
  #define BOARD_SD_PARENT "/data/mnt"
  #define BOARD_DATA_ROOT "/usr/data"
#endif
#endif /* BOARD_CONFIG_H */
