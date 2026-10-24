#ifndef ASSETS_H
#define ASSETS_H

#include "lvgl.h"
/* lv_image_decoder_dsc_t / lv_image_decoder_args_t moved behind the private
 * decoder header in LVGL 9.2+. asset_decoded_image_t embeds a decoder
 * session so large PNGs can stay decoded across animation frames. */
#include "src/draw/lv_image_decoder_private.h"

/* Real UI assets: this project's own (assets/themes/ in the repository) on
 * top of the stock HiBy firmware's own theme2 (dark) resource pack, which is
 * not ours to redistribute, so none of it is vendored into this repo. On the
 * X1600 boards BOARD.paths.theme_root is the firmware's own theme2, with our
 * assets copied over it by boards/common/repack_upt.sh; on the R3 Pro it is
 * <install_root>/themes/ (boards/r3pro/tools/make_theme.sh). On host, the
 * simulator reads ./assets/themes in the checkout: this project's own assets
 * are tracked there, and stock ones can be copied in locally (gitignored,
 * see .gitignore) so the simulator has something to show. */

/* Registers the PNG decoder and POSIX filesystem driver LVGL needs to load
 * images by path. Call once, before any screen references an asset. */
void assets_init(void);

/* Resolves a path like "launcher/music.png" to a stable interned LVGL image
 * source string (drive letter + theme root + relative path). The caller
 * does not free it. Repeated calls/reloads reuse the same resolved string;
 * stock and override paths may each have one entry for an asset. */
const char * asset_path(const char * relative_path);

/* Same override-root-then-stock-root resolution as asset_path(), but
 * returns a plain absolute filesystem path with no "S:" LVGL-driver
 * prefix -- for callers that add their own prefix/handling downstream
 * (e.g. pill_row_apply_icon()'s icon_asset contract, screen_builders.c)
 * rather than passing straight into an LVGL image-source setter. Returned
 * strings have the same stable interned lifetime as asset_path(). */
const char * asset_path_plain(const char * relative_path);

/* Keeps one PNG decoded as an LVGL draw buffer until close. Use for large,
 * immutable artwork that is redrawn on every animation/drag frame; unlike
 * the global image cache, this cannot be evicted by unrelated screen art. */
typedef struct {
    lv_image_decoder_dsc_t decoder;
    char * path;
    bool open;
} asset_decoded_image_t;

bool asset_decoded_image_open(asset_decoded_image_t * image, const char * relative_path);
/* Pre-quantize smooth backgrounds with fixed ordered dithering for RGB565.
 * Alpha is preserved; run once at load, never during a redraw. */
bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * relative_path);
void asset_decoded_image_close(asset_decoded_image_t * image);
const void * asset_decoded_image_source(const asset_decoded_image_t * image);

#endif /* ASSETS_H */
