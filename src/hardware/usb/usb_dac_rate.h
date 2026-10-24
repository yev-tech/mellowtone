#ifndef USB_DAC_RATE_H
#define USB_DAC_RATE_H

/* Clock drift between a USB host and the DAC, for the USB DAC bridge
 * (usb_dac_bridge.c).
 *
 * The host sends at its own clock (measured: ~0.08-0.09% fast against the
 * device), so the bridge's ring slowly fills. Cutting the surplus out is
 * heard as a tick on a steady tone, however small the cut and however it is
 * faded. Instead PCM is played through a resampler whose ratio follows the
 * ring: a slow PI controller holds the ring at a level learned after the
 * output starts, and the ratio stays within USB_DAC_RATE_MAX_PPM of 1 and
 * moves at most USB_DAC_RATE_SLEW_PPM_PER_S, far too slowly to be heard as
 * a pitch change.
 *
 * Both halves are pure (no clock, no I/O), so tests drive them with
 * simulated time. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- resampler ------------------------------------------------------------- */

/* Most input frames one call takes. */
#define USB_DAC_RESAMPLER_MAX_IN 1024
/* Frames held back for the interpolation (one before, two after). */
#define USB_DAC_RESAMPLER_HISTORY 3
/* Output frames one call can produce beyond its input frames, at the
 * largest ratio away from 1 (size output buffers with it). */
#define USB_DAC_RESAMPLER_EXTRA_OUT 8

/* Interleaved stereo, samples 24-bit right-justified in int32. 4-point
 * Hermite interpolation with a Q32 fractional read position; at a step of
 * exactly 1 and a whole position the input passes unchanged (two frames
 * late). */
typedef struct {
    int32_t buf[(USB_DAC_RESAMPLER_MAX_IN + USB_DAC_RESAMPLER_HISTORY + USB_DAC_RESAMPLER_EXTRA_OUT) * 2];
    size_t frames; /* frames in buf */
    uint64_t pos;  /* read position in buf, Q32 frames; always >= 1 frame */
} usb_dac_resampler_t;

/* Starts afresh: one frame of silence as history, nothing held. */
void usb_dac_resampler_reset(usb_dac_resampler_t * rs);

/* Where the caller writes the next input frames, and how many fit (*room). */
int32_t * usb_dac_resampler_input(usb_dac_resampler_t * rs, size_t * room);

/* Takes `added` frames the caller wrote at usb_dac_resampler_input() and
 * writes up to out_cap frames to out, advancing the input position by
 * `step` (Q32, input frames per output frame, ~2^32) per output frame.
 * Output is clamped to 24 bits. Returns the frames written. */
size_t usb_dac_resampler_run(usb_dac_resampler_t * rs, size_t added, int64_t step, int32_t * out, size_t out_cap);

/* Q32 step for a ratio (input frames per output frame). */
int64_t usb_dac_resampler_step(double ratio);

/* ---- controller ------------------------------------------------------------ */

#define USB_DAC_RATE_MAX_PPM 1500.0       /* clamp on |ratio - 1| (hosts measured ~900 ppm fast) */
#define USB_DAC_RATE_SLEW_PPM_PER_S 50.0  /* fastest the ratio moves */
#define USB_DAC_RATE_WINDOW_S 2.0         /* occupancy is judged by each window's lowest level */
#define USB_DAC_RATE_SETTLE_S 2.0         /* ignored after a (re)start: the output fills its own queue */
#define USB_DAC_RATE_LEARN_S 6.0          /* baseline: median of the windows' lowest levels */
#define USB_DAC_RATE_LEVEL_TAU_S 5.0      /* smoothing of the windows' lowest levels */
#define USB_DAC_RATE_KP 0.04              /* per second: 1 ms above baseline asks for 40 ppm */
#define USB_DAC_RATE_KI 0.001             /* per second squared */

#define USB_DAC_RATE_LEARN_WINDOWS 16

typedef struct {
    double rate;           /* frames/s of the stream */
    double since_reset_s;
    bool learned;
    double baseline;       /* frames: the ring level held once learned */
    double learn_min[USB_DAC_RATE_LEARN_WINDOWS];
    unsigned int learn_count;
    double window_s;       /* time into the current window */
    double window_min;     /* frames: lowest level in the current window */
    double last_window_min; /* frames: the last complete window's lowest level, -1 before one */
    bool window_done;      /* the last update closed a window */
    double level;          /* frames: smoothed window minima */
    double integral;       /* the integral term, as a ratio offset */
    double target;         /* the controller's correction, before the slew limit */
    double correction;     /* applied: ratio = 1 + correction */
} usb_dac_rate_ctl_t;

/* Starts with no correction and no baseline. */
void usb_dac_rate_ctl_init(usb_dac_rate_ctl_t * ctl, unsigned int rate);

/* Learns the baseline again (new rate, output reopened, route changed),
 * keeping the correction: the clocks drift apart the same as before. */
void usb_dac_rate_ctl_relearn(usb_dac_rate_ctl_t * ctl, unsigned int rate);

/* One observation: the ring holds occupancy_frames, dt_s after the last
 * call. adapt false (DoP, native DSD) learns and tracks the level but
 * keeps the correction where it is. Returns the ratio to play at (input
 * frames per output frame). */
double usb_dac_rate_ctl_update(usb_dac_rate_ctl_t * ctl, double occupancy_frames, double dt_s, bool adapt);

/* Correction in ppm, for logs. */
double usb_dac_rate_ctl_ppm(const usb_dac_rate_ctl_t * ctl);

#endif /* USB_DAC_RATE_H */
