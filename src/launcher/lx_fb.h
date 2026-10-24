#ifndef LX_FB_H
#define LX_FB_H

/* The launcher's screen: a back buffer in the viewer's orientation (XRGB
 * 8888), flushed to /dev/fb0 in the panel's format (RGB565 on X1600, XRGB
 * 8888 on the R3 Pro), rotated when the panel is mounted upside down. On a
 * smart LCD (R3 Pro jzfb) both driver pages get the frame and a pan starts
 * the transfer, as patches/lvgl_r3pro_slcd_pan.patch does for LVGL. A
 * memory-only screen serves the tests. Text: a 5x8 bitmap font (printable
 * ASCII) drawn at an integer scale. */

#include <stdbool.h>
#include <stdint.h>

#include "lx.h"

lx_fb_t * lx_fb_open_mem(int w, int h);
/* rotation: 0 or 180. slcd: see above. Fails (NULL) when the device's
 * geometry is not w x h at 16 or 32 bpp. */
lx_fb_t * lx_fb_open_dev(const char * dev, int w, int h, int rotation, bool slcd);
void lx_fb_close(lx_fb_t * fb);

int lx_fb_w(const lx_fb_t * fb);
int lx_fb_h(const lx_fb_t * fb);
uint32_t lx_fb_pixel(const lx_fb_t * fb, int x, int y); /* back buffer, 0xRRGGBB */
unsigned lx_fb_flushes(const lx_fb_t * fb);

void lx_fb_fill(lx_fb_t * fb, uint32_t rgb);
void lx_fb_rect(lx_fb_t * fb, int x, int y, int w, int h, uint32_t rgb);
/* Returns the drawn width. */
int lx_fb_text(lx_fb_t * fb, int x, int y, int scale, const char * text, uint32_t rgb);
int lx_fb_text_w(int scale, const char * text);
int lx_fb_char_w(int scale);
int lx_fb_line_h(int scale);
void lx_fb_flush(lx_fb_t * fb);
/* Baseline JPEG of exactly the screen size into the back buffer. */
bool lx_fb_jpeg(lx_fb_t * fb, const char * path);

#endif
