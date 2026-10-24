#define _GNU_SOURCE
#include "lx_fb.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lvgl/src/libs/tjpgd/tjpgd.h"

/* jzfb vendor ioctl, _IOR('F', 0x20, int): waits until the previous SLCD
 * frame transfer has finished (the stock UI and LVGL's patch do the same). */
#define JZFB_WAIT_FRAME_DONE 0x80044620

struct lx_fb {
    int w, h;
    uint32_t * px; /* back buffer, viewer orientation */
    unsigned flushes;
    /* device only */
    int fd;
    uint8_t * mem;
    size_t mem_len;
    int bpp;
    uint32_t line_len;
    int rotation;
    bool slcd;
    struct fb_var_screeninfo vinfo;
};

/* 5x8 column font for 0x20..0x7E: five bytes per glyph, one per column,
 * bit 0 = top row (the classic public-domain 5x7 table; descenders use row 7). */
static const uint8_t font5x8[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x5F, 0x00, 0x00 }, { 0x00, 0x07, 0x00, 0x07, 0x00 },
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, { 0x23, 0x13, 0x08, 0x64, 0x62 },
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, { 0x00, 0x05, 0x03, 0x00, 0x00 }, { 0x00, 0x1C, 0x22, 0x41, 0x00 },
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, { 0x08, 0x2A, 0x1C, 0x2A, 0x08 }, { 0x08, 0x08, 0x3E, 0x08, 0x08 },
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, { 0x08, 0x08, 0x08, 0x08, 0x08 }, { 0x00, 0x60, 0x60, 0x00, 0x00 },
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, { 0x3E, 0x51, 0x49, 0x45, 0x3E }, { 0x00, 0x42, 0x7F, 0x40, 0x00 },
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4B, 0x31 }, { 0x18, 0x14, 0x12, 0x7F, 0x10 },
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1E }, { 0x00, 0x36, 0x36, 0x00, 0x00 },
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, { 0x08, 0x14, 0x22, 0x41, 0x00 }, { 0x14, 0x14, 0x14, 0x14, 0x14 },
    { 0x00, 0x41, 0x22, 0x14, 0x08 }, { 0x02, 0x01, 0x51, 0x09, 0x06 }, { 0x32, 0x49, 0x79, 0x41, 0x3E },
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, { 0x7F, 0x49, 0x49, 0x49, 0x36 }, { 0x3E, 0x41, 0x41, 0x41, 0x22 },
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, { 0x7F, 0x49, 0x49, 0x49, 0x41 }, { 0x7F, 0x09, 0x09, 0x09, 0x01 },
    { 0x3E, 0x41, 0x49, 0x49, 0x7A }, { 0x7F, 0x08, 0x08, 0x08, 0x7F }, { 0x00, 0x41, 0x7F, 0x41, 0x00 },
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, { 0x7F, 0x08, 0x14, 0x22, 0x41 }, { 0x7F, 0x40, 0x40, 0x40, 0x40 },
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, { 0x7F, 0x04, 0x08, 0x10, 0x7F }, { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, { 0x3E, 0x41, 0x51, 0x21, 0x5E }, { 0x7F, 0x09, 0x19, 0x29, 0x46 },
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, { 0x01, 0x01, 0x7F, 0x01, 0x01 }, { 0x3F, 0x40, 0x40, 0x40, 0x3F },
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, { 0x3F, 0x40, 0x38, 0x40, 0x3F }, { 0x63, 0x14, 0x08, 0x14, 0x63 },
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, { 0x61, 0x51, 0x49, 0x45, 0x43 }, { 0x00, 0x7F, 0x41, 0x41, 0x00 },
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, { 0x00, 0x41, 0x41, 0x7F, 0x00 }, { 0x04, 0x02, 0x01, 0x02, 0x04 },
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, { 0x00, 0x01, 0x02, 0x04, 0x00 }, { 0x20, 0x54, 0x54, 0x54, 0x78 },
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, { 0x38, 0x44, 0x44, 0x44, 0x20 }, { 0x38, 0x44, 0x44, 0x48, 0x7F },
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, { 0x08, 0x7E, 0x09, 0x01, 0x02 }, { 0x18, 0xA4, 0xA4, 0xA4, 0x7C },
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, { 0x00, 0x44, 0x7D, 0x40, 0x00 }, { 0x40, 0x80, 0x84, 0x7D, 0x00 },
    { 0x7F, 0x10, 0x28, 0x44, 0x00 }, { 0x00, 0x41, 0x7F, 0x40, 0x00 }, { 0x7C, 0x04, 0x18, 0x04, 0x78 },
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, { 0x38, 0x44, 0x44, 0x44, 0x38 }, { 0xFC, 0x24, 0x24, 0x24, 0x18 },
    { 0x18, 0x24, 0x24, 0x18, 0xFC }, { 0x7C, 0x08, 0x04, 0x04, 0x08 }, { 0x48, 0x54, 0x54, 0x54, 0x20 },
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, { 0x3C, 0x40, 0x40, 0x20, 0x7C }, { 0x1C, 0x20, 0x40, 0x20, 0x1C },
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, { 0x44, 0x28, 0x10, 0x28, 0x44 }, { 0x1C, 0xA0, 0xA0, 0xA0, 0x7C },
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, { 0x00, 0x08, 0x36, 0x41, 0x00 }, { 0x00, 0x00, 0x7F, 0x00, 0x00 },
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, { 0x08, 0x04, 0x08, 0x10, 0x08 },
};

#define GLYPH_W 5
#define GLYPH_H 8
#define CELL_W (GLYPH_W + 1)
#define CELL_H (GLYPH_H + 2)

static lx_fb_t * alloc_fb(int w, int h) {
    lx_fb_t * fb = calloc(1, sizeof(*fb));
    if (!fb) return NULL;
    fb->w = w;
    fb->h = h;
    fb->fd = -1;
    fb->px = calloc((size_t) w * (size_t) h, sizeof(uint32_t));
    if (!fb->px) {
        free(fb);
        return NULL;
    }
    return fb;
}

lx_fb_t * lx_fb_open_mem(int w, int h) {
    return w > 0 && h > 0 ? alloc_fb(w, h) : NULL;
}

lx_fb_t * lx_fb_open_dev(const char * dev, int w, int h, int rotation, bool slcd) {
    int fd = -1;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    /* The node can exist before the driver has settled (main.c's own wait). */
    for (int attempt = 0; attempt < 50; attempt++) {
        fd = open(dev, O_RDWR | O_CLOEXEC);
        if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0)
            break;
        if (fd >= 0) close(fd);
        fd = -1;
        usleep(100000);
    }
    if (fd < 0) {
        lx_log("fb: %s not ready", dev);
        return NULL;
    }
    if ((int) vinfo.xres != w || (int) vinfo.yres != h || (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32)) {
        lx_log("fb: %ux%u@%u, expected %dx%d at 16 or 32 bpp", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, w, h);
        close(fd);
        return NULL;
    }
    uint8_t * mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        lx_log("fb: mmap failed");
        close(fd);
        return NULL;
    }
    size_t frame = (size_t) finfo.line_length * vinfo.yres;
    size_t page = (size_t) vinfo.yoffset * finfo.line_length + (size_t) vinfo.xoffset * (vinfo.bits_per_pixel / 8);
    if (page + frame > finfo.smem_len) {
        lx_log("fb: page offset %u,%u leaves no room for a frame", vinfo.xoffset, vinfo.yoffset);
        munmap(mem, finfo.smem_len);
        close(fd);
        return NULL;
    }
    lx_fb_t * fb = alloc_fb(w, h);
    if (!fb) {
        munmap(mem, finfo.smem_len);
        close(fd);
        return NULL;
    }
    fb->fd = fd;
    fb->mem = mem;
    fb->mem_len = finfo.smem_len;
    fb->bpp = (int) vinfo.bits_per_pixel;
    fb->line_len = finfo.line_length;
    fb->rotation = rotation;
    fb->slcd = slcd;
    fb->vinfo = vinfo;
    /* No FBIOBLANK unblank: on a jzfb smart LCD it reprograms the DMA
     * registers mid-transfer and misaligns the panel until reboot. */
    return fb;
}

void lx_fb_close(lx_fb_t * fb) {
    if (!fb) return;
    if (fb->mem) {
        if (!fb->slcd && (fb->vinfo.xoffset || fb->vinfo.yoffset)) {
            /* The player draws to page 0: show the last frame there before
             * handing over (the old bootloader's fb_close()). */
            for (int y = 0; y < fb->h; y++) {
                memcpy(fb->mem + (size_t) y * fb->line_len,
                       fb->mem + ((size_t) fb->vinfo.yoffset + (size_t) y) * fb->line_len +
                           (size_t) fb->vinfo.xoffset * (size_t) (fb->bpp / 8),
                       (size_t) fb->w * (size_t) (fb->bpp / 8));
            }
            struct fb_var_screeninfo v = fb->vinfo;
            v.xoffset = 0;
            v.yoffset = 0;
            (void) ioctl(fb->fd, FBIOPAN_DISPLAY, &v);
        }
        munmap(fb->mem, fb->mem_len);
    }
    if (fb->fd >= 0) close(fb->fd);
    free(fb->px);
    free(fb);
}

int lx_fb_w(const lx_fb_t * fb) { return fb->w; }
int lx_fb_h(const lx_fb_t * fb) { return fb->h; }
unsigned lx_fb_flushes(const lx_fb_t * fb) { return fb->flushes; }

uint32_t lx_fb_pixel(const lx_fb_t * fb, int x, int y) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return 0;
    return fb->px[(size_t) y * (size_t) fb->w + (size_t) x];
}

void lx_fb_rect(lx_fb_t * fb, int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->w ? fb->w : x + w, y1 = y + h > fb->h ? fb->h : y + h;
    for (int py = y0; py < y1; py++) {
        uint32_t * row = fb->px + (size_t) py * (size_t) fb->w;
        for (int px = x0; px < x1; px++) row[px] = rgb & 0xFFFFFF;
    }
}

void lx_fb_fill(lx_fb_t * fb, uint32_t rgb) {
    lx_fb_rect(fb, 0, 0, fb->w, fb->h, rgb);
}

int lx_fb_char_w(int scale) { return CELL_W * scale; }
int lx_fb_line_h(int scale) { return CELL_H * scale; }

int lx_fb_text_w(int scale, const char * text) {
    return (int) strlen(text) * CELL_W * scale;
}

int lx_fb_text(lx_fb_t * fb, int x, int y, int scale, const char * text, uint32_t rgb) {
    int cx = x;
    for (const unsigned char * p = (const unsigned char *) text; *p; p++) {
        unsigned c = *p >= 0x20 && *p <= 0x7E ? *p - 0x20u : (unsigned) ('?' - 0x20);
        for (int col = 0; col < GLYPH_W; col++) {
            uint8_t bits = font5x8[c][col];
            for (int row = 0; row < GLYPH_H; row++)
                if (bits & (1u << row)) lx_fb_rect(fb, cx + col * scale, y + row * scale, scale, scale, rgb);
        }
        cx += CELL_W * scale;
    }
    return cx - x;
}

static void put_pixel(uint8_t * dst, int bpp, uint32_t rgb) {
    if (bpp == 32) {
        uint32_t v = 0xFF000000u | rgb;
        memcpy(dst, &v, 4);
    } else {
        uint16_t v = (uint16_t) (((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
        memcpy(dst, &v, 2);
    }
}

void lx_fb_flush(lx_fb_t * fb) {
    fb->flushes++;
    if (!fb->mem) return;
    const size_t bytes = (size_t) (fb->bpp / 8);
    const size_t frame = (size_t) fb->line_len * fb->vinfo.yres;
    const size_t page = (size_t) fb->vinfo.yoffset * fb->line_len + (size_t) fb->vinfo.xoffset * bytes;
    /* jzfb's pages are page-aligned: the second starts at smem_len / 2, not
     * at line_length * yres (see the LVGL patch). */
    const size_t mirror = fb->mem_len / 2;
    const bool both = fb->slcd && mirror >= frame && page + mirror + frame <= fb->mem_len;
    for (int y = 0; y < fb->h; y++) {
        int sy = fb->rotation == 180 ? fb->h - 1 - y : y;
        const uint32_t * src = fb->px + (size_t) sy * (size_t) fb->w;
        uint8_t * d0 = fb->mem + page + (size_t) y * fb->line_len;
        for (int x = 0; x < fb->w; x++) {
            uint32_t v = src[fb->rotation == 180 ? fb->w - 1 - x : x];
            put_pixel(d0 + (size_t) x * bytes, fb->bpp, v);
        }
        if (both) memcpy(d0 + mirror, d0, (size_t) fb->w * bytes); /* as the patch: page + smem_len / 2 */
    }
    if (fb->slcd) {
        /* A smart LCD keeps the picture in its own RAM and transfers a new
         * frame only on a pan; never start one while one is in flight. */
        int done = 0;
        (void) ioctl(fb->fd, JZFB_WAIT_FRAME_DONE, &done);
        if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->vinfo) != 0) lx_log("fb: pan failed");
    }
}

/* ---- JPEG (boot splash) ---- */

typedef struct {
    FILE * f;
    lx_fb_t * fb;
} jpeg_ctx_t;

static size_t jpeg_read(JDEC * jd, uint8_t * buf, size_t n) {
    jpeg_ctx_t * c = jd->device;
    if (!buf) return fseek(c->f, (long) n, SEEK_CUR) == 0 ? n : 0;
    return fread(buf, 1, n, c->f);
}

static int jpeg_out(JDEC * jd, void * bitmap, JRECT * rect) {
    jpeg_ctx_t * c = jd->device;
    int w = rect->right - rect->left + 1;
    const uint8_t * src = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            const uint8_t * p = src + ((size_t) (y - rect->top) * (size_t) w + (size_t) (x - rect->left)) * 3;
            /* The vendored tjpgd emits B, G, R (the old bootloader's mapping). */
            if (x < c->fb->w && y < c->fb->h)
                c->fb->px[(size_t) y * (size_t) c->fb->w + (size_t) x] = (uint32_t) p[2] << 16 | (uint32_t) p[1] << 8 | p[0];
        }
    }
    return 1;
}

bool lx_fb_jpeg(lx_fb_t * fb, const char * path) {
    FILE * f = fopen(path, "rbe");
    if (!f) return false;
    jpeg_ctx_t ctx = { f, fb };
    static uint8_t work[4096];
    JDEC jd;
    bool ok = jd_prepare(&jd, jpeg_read, work, sizeof(work), &ctx) == JDR_OK && (int) jd.width == fb->w &&
              (int) jd.height == fb->h && jd_decomp(&jd, jpeg_out, 0) == JDR_OK;
    fclose(f);
    return ok;
}
