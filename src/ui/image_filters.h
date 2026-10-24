#ifndef IMAGE_FILTERS_H
#define IMAGE_FILTERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel-math helpers on 8-bit planes and RGB888 values: a separable box
 * blur and RGB888 -> RGB565 conversion with dithering. Used by gradient
 * asset decoding (assets.c) and the player's blurred cover reflection
 * (gui_player.c). */

/* One pass of a box blur along `length` elements `stride` apart (1 for a
 * row, the width for a column), edges clamped. src and dst must differ. */
void image_box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius);

/* RGB888 -> RGB565 with 8x8 ordered (Bayer) dithering. */
uint16_t image_rgb888_to_565_dithered(int r, int g, int b, int x, int y);
/* Non-periodic, coordinate-stable variant for full-screen artwork. */
uint16_t image_rgb888_to_565_spatial_dithered(int r, int g, int b, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_FILTERS_H */
