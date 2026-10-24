#include "usb_dac_rate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- resampler ------------------------------------------------------------- */

#define FRAME_SAMPLES 2
#define BUF_FRAMES (sizeof(((usb_dac_resampler_t *) 0)->buf) / sizeof(int32_t) / FRAME_SAMPLES)
#define S24_MAX ((1 << 23) - 1)
#define S24_MIN (-(1 << 23))

void usb_dac_resampler_reset(usb_dac_resampler_t * rs) {
    memset(rs->buf, 0, FRAME_SAMPLES * sizeof(int32_t));
    rs->frames = 1;
    rs->pos = (uint64_t) 1 << 32;
}

int32_t * usb_dac_resampler_input(usb_dac_resampler_t * rs, size_t * room) {
    size_t free_frames = BUF_FRAMES - rs->frames;
    *room = free_frames < USB_DAC_RESAMPLER_MAX_IN ? free_frames : USB_DAC_RESAMPLER_MAX_IN;
    return rs->buf + rs->frames * FRAME_SAMPLES;
}

int64_t usb_dac_resampler_step(double ratio) { return (int64_t) llround(ratio * 4294967296.0); }

/* 4-point Hermite between x0 and x1 at t (Q24), coefficients doubled so
 * they stay whole numbers. With 24-bit samples every coefficient fits in
 * 28 bits, so each product is one 32x32->64 multiply. */
static inline int32_t hermite(int32_t xm1, int32_t x0, int32_t x1, int32_t x2, int32_t t) {
    int32_t c1 = x1 - xm1;
    int32_t c2 = 2 * xm1 - 5 * x0 + 4 * x1 - x2;
    int32_t c3 = (x2 - xm1) + 3 * (x0 - x1);
    int32_t acc = (int32_t) (((int64_t) c3 * t) >> 24);
    acc = (int32_t) (((int64_t) (acc + c2) * t) >> 24);
    acc = (int32_t) (((int64_t) (acc + c1) * t) >> 24);
    int32_t y = (acc + 2 * x0 + 1) >> 1;
    if (y > S24_MAX) return S24_MAX;
    if (y < S24_MIN) return S24_MIN;
    return y;
}

size_t usb_dac_resampler_run(usb_dac_resampler_t * rs, size_t added, int64_t step, int32_t * out, size_t out_cap) {
    rs->frames += added;
    size_t n = 0;
    uint64_t pos = rs->pos;
    while (n < out_cap) {
        size_t i = (size_t) (pos >> 32);
        if (i + 2 >= rs->frames) break;
        const int32_t * x = rs->buf + (i - 1) * FRAME_SAMPLES;
        int32_t t = (int32_t) ((uint32_t) pos >> 8);
        if (t == 0) {
            out[n * 2] = x[2];
            out[n * 2 + 1] = x[3];
        } else {
            out[n * 2] = hermite(x[0], x[2], x[4], x[6], t);
            out[n * 2 + 1] = hermite(x[1], x[3], x[5], x[7], t);
        }
        n++;
        pos += (uint64_t) step;
    }
    /* Keep the frame before the read position and everything after it. */
    size_t drop = (size_t) (pos >> 32) - 1;
    if (drop > rs->frames) drop = rs->frames;
    if (drop > 0) {
        memmove(rs->buf, rs->buf + drop * FRAME_SAMPLES, (rs->frames - drop) * FRAME_SAMPLES * sizeof(int32_t));
        rs->frames -= drop;
        pos -= (uint64_t) drop << 32;
    }
    rs->pos = pos;
    return n;
}

/* ---- controller ------------------------------------------------------------ */

#define MAX_CORRECTION (USB_DAC_RATE_MAX_PPM / 1e6)

static double clamp(double v, double lim) { return v > lim ? lim : v < -lim ? -lim : v; }

static void start_learning(usb_dac_rate_ctl_t * ctl, unsigned int rate) {
    ctl->rate = rate > 0 ? (double) rate : 1.0;
    ctl->since_reset_s = 0.0;
    ctl->learned = false;
    ctl->baseline = 0.0;
    ctl->learn_count = 0;
    ctl->window_s = 0.0;
    ctl->window_min = HUGE_VAL;
    ctl->last_window_min = -1.0;
    ctl->window_done = false;
    ctl->level = 0.0;
}

void usb_dac_rate_ctl_init(usb_dac_rate_ctl_t * ctl, unsigned int rate) {
    memset(ctl, 0, sizeof(*ctl));
    start_learning(ctl, rate);
}

void usb_dac_rate_ctl_relearn(usb_dac_rate_ctl_t * ctl, unsigned int rate) { start_learning(ctl, rate); }

static int cmp_double(const void * a, const void * b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

double usb_dac_rate_ctl_update(usb_dac_rate_ctl_t * ctl, double occupancy_frames, double dt_s, bool adapt) {
    /* A long stall (output reopening, the host pausing) says nothing about
     * drift: count it as one window at most. */
    if (!(dt_s > 0.0)) dt_s = 0.0;
    if (dt_s > USB_DAC_RATE_WINDOW_S) dt_s = USB_DAC_RATE_WINDOW_S;
    ctl->since_reset_s += dt_s;
    ctl->window_s += dt_s;
    ctl->window_done = false;
    if (occupancy_frames < ctl->window_min) ctl->window_min = occupancy_frames;

    if (ctl->window_s >= USB_DAC_RATE_WINDOW_S) {
        double window_s = ctl->window_s, window_min = ctl->window_min;
        ctl->window_s = 0.0;
        ctl->window_min = HUGE_VAL;
        ctl->last_window_min = window_min;
        ctl->window_done = true;
        if (!ctl->learned) {
            /* Only windows that began after the settling time count. */
            if (ctl->since_reset_s - window_s >= USB_DAC_RATE_SETTLE_S - 1e-9 &&
                ctl->learn_count < USB_DAC_RATE_LEARN_WINDOWS)
                ctl->learn_min[ctl->learn_count++] = window_min;
            unsigned int need = (unsigned int) (USB_DAC_RATE_LEARN_S / USB_DAC_RATE_WINDOW_S + 0.5);
            if (need > USB_DAC_RATE_LEARN_WINDOWS) need = USB_DAC_RATE_LEARN_WINDOWS;
            if (ctl->learn_count >= need) {
                double sorted[USB_DAC_RATE_LEARN_WINDOWS];
                memcpy(sorted, ctl->learn_min, ctl->learn_count * sizeof(double));
                qsort(sorted, ctl->learn_count, sizeof(double), cmp_double);
                ctl->baseline = sorted[ctl->learn_count / 2];
                ctl->level = ctl->baseline;
                ctl->learned = true;
            }
        } else {
            ctl->level += (1.0 - exp(-window_s / USB_DAC_RATE_LEVEL_TAU_S)) * (window_min - ctl->level);
            if (adapt) {
                double error_s = (ctl->level - ctl->baseline) / ctl->rate;
                ctl->integral = clamp(ctl->integral + USB_DAC_RATE_KI * error_s * window_s, MAX_CORRECTION);
                ctl->target = clamp(USB_DAC_RATE_KP * error_s + ctl->integral, MAX_CORRECTION);
            }
        }
    }

    if (adapt) {
        double max_move = USB_DAC_RATE_SLEW_PPM_PER_S / 1e6 * dt_s;
        double move = clamp(ctl->target - ctl->correction, max_move);
        ctl->correction = clamp(ctl->correction + move, MAX_CORRECTION);
    }
    return 1.0 + ctl->correction;
}

double usb_dac_rate_ctl_ppm(const usb_dac_rate_ctl_t * ctl) { return ctl->correction * 1e6; }
