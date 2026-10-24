#include "board.h"
#include "board_config.h"
#include "assets.h"
#include "image_filters.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define THEME_ROOT "assets/themes/"
#else
  /* The stock firmware's own on the X1600 boards; assembled by
   * boards/r3pro/tools/make_theme.sh and installed with the player on the R3 Pro. */
  #define THEME_ROOT (board_asset_path(BOARD.paths.theme_root))
  /* Writable override, checked first by asset_path(), for custom assets
   * not present in the read-only THEME_ROOT squashfs pack. BOARD_DATA_ROOT
   * is the persistent writable install root (/usr/data, or /data/mellowtone
   * on the R3 Pro). */
  #define THEME_OVERRIDE_ROOT BOARD_DATA_ROOT "/theme_overrides/"
#endif

void assets_init(void) {
    lv_lodepng_init();
    lv_fs_posix_init();
    lv_tjpgd_init();
    lv_fs_memfs_init();
}

typedef struct asset_path_entry {
    struct asset_path_entry * next;
    char value[];
} asset_path_entry_t;

static asset_path_entry_t * asset_paths;

/* LVGL keeps raw path pointers in some styles. Intern the fully resolved
 * value so the pointer remains stable without leaking another strdup on
 * every soft UI rebuild. The set is bounded by native asset names times
 * the two possible roots (stock/override), not by reload count. */
static const char * asset_path_intern(const char * value) {
    for (asset_path_entry_t * e = asset_paths; e; e = e->next) {
        if (strcmp(e->value, value) == 0) return e->value;
    }
    size_t len = strlen(value);
    asset_path_entry_t * e = malloc(sizeof(*e) + len + 1);
    if (!e) return NULL;
    memcpy(e->value, value, len + 1);
    e->next = asset_paths;
    asset_paths = e;
    return e->value;
}

const char * asset_path(const char * relative_path) {
    char buf[320];
#ifndef HOST_BUILD
    /* One access() check, at screen-build time only (every caller resolves
     * its own path once when the screen is constructed, never per-frame),
     * so the extra syscall is negligible -- see THEME_OVERRIDE_ROOT's own
     * comment for why this exists at all. */
    snprintf(buf, sizeof(buf), THEME_OVERRIDE_ROOT "%s", relative_path);
    if (access(buf, R_OK) == 0) {
        snprintf(buf, sizeof(buf), "S:" THEME_OVERRIDE_ROOT "%s", relative_path);
        return asset_path_intern(buf);
    }
#endif
    snprintf(buf, sizeof(buf), "S:%s%s", THEME_ROOT, relative_path);
    return asset_path_intern(buf);
}

const char * asset_path_plain(const char * relative_path) {
    char buf[320];
#ifndef HOST_BUILD
    snprintf(buf, sizeof(buf), THEME_OVERRIDE_ROOT "%s", relative_path);
    if (access(buf, R_OK) == 0) {
        return asset_path_intern(buf);
    }
    snprintf(buf, sizeof(buf), "%s%s", THEME_ROOT, relative_path);
#else
    /* THEME_ROOT is a relative path on host ("assets/themes/", resolved
     * against the process's own CWD, same as MUSIC_ROOT_DIR's own "./music"
     * elsewhere) -- fine for asset_path()'s "S:"-prefixed LVGL image source
     * (LVGL's POSIX fs driver just opens it via the process's normal CWD),
     * but this function's whole point is a path callers like pill_row_
     * apply_icon() can tell apart from a plugin-relative one purely by
     * whether it starts with '/' -- so unlike asset_path(), this needs a
     * genuinely absolute path even on host. */
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(buf, sizeof(buf), "%s/" THEME_ROOT "%s", cwd, relative_path);
    } else {
        snprintf(buf, sizeof(buf), THEME_ROOT "%s", relative_path);
    }
#endif
    return asset_path_intern(buf);
}

bool asset_decoded_image_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!image || !relative_path) return false;
    asset_decoded_image_close(image);

    const char * resolved = asset_path(relative_path);
    image->path = resolved ? strdup(resolved) : NULL;
    if (!image->path) return false;

    lv_image_decoder_args_t args = { .no_cache = true };
    if (lv_image_decoder_open(&image->decoder, image->path, &args) != LV_RESULT_OK ||
        !image->decoder.decoded) {
        free(image->path);
        memset(image, 0, sizeof(*image));
        return false;
    }
    image->open = true;
    return true;
}

void asset_decoded_image_close(asset_decoded_image_t * image) {
    if (!image) return;
    if (image->open) lv_image_decoder_close(&image->decoder);
    free(image->path);
    memset(image, 0, sizeof(*image));
}

bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!asset_decoded_image_open(image, relative_path)) return false;
    const lv_draw_buf_t * buffer = image->decoder.decoded;
    if (buffer->header.cf != LV_COLOR_FORMAT_ARGB8888) return true;
    /* Keep the decoder's owned ARGB buffer and its alpha plane. Expanding
     * the dithered 565 result back to 888 makes the eventual framebuffer
     * conversion exact without changing LVGL's image/decoder ownership. */
    for (uint32_t y = 0; y < buffer->header.h; ++y) {
        lv_color32_t * row = (lv_color32_t *) (buffer->data + y * buffer->header.stride);
        for (uint32_t x = 0; x < buffer->header.w; ++x) {
            uint16_t pixel = image_rgb888_to_565_dithered(row[x].red, row[x].green, row[x].blue, x, y);
            uint8_t r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
            row[x].red = (r << 3) | (r >> 2);
            row[x].green = (g << 2) | (g >> 4);
            row[x].blue = (b << 3) | (b >> 2);
        }
    }
    return true;
}

const void * asset_decoded_image_source(const asset_decoded_image_t * image) {
    return image && image->open ? image->decoder.decoded : NULL;
}
