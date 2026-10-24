/* The USB DAC bridge's drift handling (usb_dac_rate.c) on simulated time: a
 * host 0.09% fast streams a 440 Hz sine at 44.1 kHz into a ring; a writer
 * like the bridge's moves it through the resampler into an output that
 * takes 100 ms at a time (Bluetooth-like). Over 60 s nothing may be cut
 * (no step in the sine), the output may not run dry, the ring must stay
 * near the learned level and the ratio must settle on the drift. Also: a
 * ratio of exactly 1 passes samples unchanged, 16-bit-range input, clamping,
 * and DoP/DSD (adapt off) never get a correction. */
#include "usb_dac_rate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

#define RATE 44100
#define PERIOD 1024
#define OUT_CAP (PERIOD + USB_DAC_RESAMPLER_EXTRA_OUT)

static usb_dac_resampler_t rs;

/* ---- the drift simulation ---------------------------------------------------- */

typedef struct {
    double drift;        /* host clock - 1 */
    double seconds;
    double chunk_ms;     /* the output takes this much at a time */
    double queue_ms;     /* and holds at most this much */
    double amplitude;    /* of the sine, in 24-bit units */
    double tone_hz;
} sim_cfg_t;

typedef struct {
    double max_step;         /* largest sample-to-sample change in the output */
    double step_bound;       /* what the sine itself allows */
    unsigned int underruns;  /* output found short after the start */
    unsigned int unpaired;   /* frames where R is not -L (to the rounding LSB) */
    double ring_min_ms_late, ring_max_ms_late; /* window lows in the last 20 s, vs baseline */
    double ring_max_ms;      /* highest level after learning */
    double baseline_ms;
    double final_ppm, max_ppm;
    double max_slew_ppm_per_s;
    bool learned;
} sim_result_t;

static void simulate(const sim_cfg_t * cfg, sim_result_t * res) {
    memset(res, 0, sizeof(*res));
    const size_t ring_cap = (size_t) (RATE * (cfg->seconds + 2.0)) * 2;
    int32_t * ring = malloc(ring_cap * sizeof(int32_t)); /* never wraps: indexes only grow */
    size_t ring_head = 0, ring_tail = 0;                 /* frames */
    int32_t out[OUT_CAP * 2];

    usb_dac_rate_ctl_t ctl;
    usb_dac_rate_ctl_init(&ctl, RATE);
    usb_dac_resampler_reset(&rs);
    double ratio = 1.0, last_update_s = 0.0, last_ppm = 0.0, last_ppm_s = 0.0;

    double host_phase = 0.0, host_frac = 0.0;
    const double host_step = 2.0 * M_PI * cfg->tone_hz / RATE; /* per host frame */
    const double queue_cap = RATE * cfg->queue_ms / 1000.0;
    /* The output already full (it takes whatever it is given at first, as an
     * aplay pipe does), so the ring keeps the startup cushion. */
    double out_queued = queue_cap - OUT_CAP;
    const double chunk = RATE * cfg->chunk_ms / 1000.0;
    double next_chunk_s = cfg->chunk_ms / 1000.0;
    bool have_prev = false, playing = false;
    int32_t prev[2] = { 0, 0 };
    res->ring_min_ms_late = 1e9;
    res->ring_max_ms_late = -1e9;
    double ms_per_frame = 1000.0 / RATE;

    /* 40 ms queued before the output starts, as after the bridge's startup trim. */
    for (int i = 0; i < RATE * 40 / 1000; i++) {
        int32_t s = (int32_t) lround(cfg->amplitude * sin(host_phase));
        host_phase += host_step;
        ring[ring_head * 2] = s;
        ring[ring_head * 2 + 1] = -s;
        ring_head++;
    }

    long steps = (long) (cfg->seconds * 1000.0);
    for (long ms = 1; ms <= steps; ms++) {
        double now_s = ms / 1000.0;
        /* The host: its frames for this millisecond, at its own clock. */
        host_frac += RATE * (1.0 + cfg->drift) / 1000.0;
        while (host_frac >= 1.0) {
            host_frac -= 1.0;
            int32_t s = (int32_t) lround(cfg->amplitude * sin(host_phase));
            host_phase += host_step;
            ring[ring_head * 2] = s;
            ring[ring_head * 2 + 1] = -s;
            ring_head++;
        }
        /* The output: a chunk at a time, at the device clock. */
        if (now_s >= next_chunk_s - 1e-9) {
            next_chunk_s += cfg->chunk_ms / 1000.0;
            if (out_queued < chunk) {
                if (playing && now_s > 1.0) res->underruns++;
                out_queued = 0.0;
            } else {
                out_queued -= chunk;
            }
            playing = true;
        }
        /* The writer: a period at a time while the output has room. */
        while (ring_head > ring_tail && out_queued + OUT_CAP <= queue_cap) {
            size_t frames = ring_head - ring_tail;
            if (frames > PERIOD) frames = PERIOD;
            size_t room = 0;
            int32_t * dst = usb_dac_resampler_input(&rs, &room);
            if (frames > room) frames = room;
            memcpy(dst, ring + ring_tail * 2, frames * 2 * sizeof(int32_t));
            ring_tail += frames;
            size_t n = usb_dac_resampler_run(&rs, frames, usb_dac_resampler_step(ratio), out, OUT_CAP);
            CHECK(n <= OUT_CAP, "output fits (%zu)", n);
            for (size_t k = 0; k < n; k++) {
                if (have_prev) {
                    double d = fabs((double) out[k * 2] - prev[0]);
                    if (d > res->max_step) res->max_step = d;
                    if (abs(out[k * 2 + 1] + out[k * 2]) > 1) res->unpaired++;
                }
                prev[0] = out[k * 2];
                prev[1] = out[k * 2 + 1];
                have_prev = true;
            }
            out_queued += (double) n;

            double occupancy = (double) (ring_head - ring_tail);
            ratio = usb_dac_rate_ctl_update(&ctl, occupancy, now_s - last_update_s, true);
            last_update_s = now_s;
            double ppm = usb_dac_rate_ctl_ppm(&ctl);
            if (fabs(ppm) > res->max_ppm) res->max_ppm = fabs(ppm);
            if (now_s - last_ppm_s >= 1.0) {
                double slew = fabs(ppm - last_ppm) / (now_s - last_ppm_s);
                if (slew > res->max_slew_ppm_per_s) res->max_slew_ppm_per_s = slew;
                last_ppm = ppm;
                last_ppm_s = now_s;
            }
            if (ctl.learned) {
                double level_ms = occupancy * ms_per_frame;
                if (level_ms > res->ring_max_ms) res->ring_max_ms = level_ms;
                if (ctl.window_done && now_s >= cfg->seconds - 20.0) {
                    double low_ms = (ctl.last_window_min - ctl.baseline) * ms_per_frame;
                    if (low_ms < res->ring_min_ms_late) res->ring_min_ms_late = low_ms;
                    if (low_ms > res->ring_max_ms_late) res->ring_max_ms_late = low_ms;
                }
            }
        }
    }
    res->learned = ctl.learned;
    res->baseline_ms = ctl.baseline * ms_per_frame;
    res->final_ppm = usb_dac_rate_ctl_ppm(&ctl);
    /* The sine's own largest step at the output rate, ratio at its clamp. */
    res->step_bound = cfg->amplitude * 2.0 * sin(host_step * (1.0 + USB_DAC_RATE_MAX_PPM / 1e6) / 2.0);
    free(ring);
}

static void test_drift(double drift, double chunk_ms, double queue_ms, const char * what) {
    sim_cfg_t cfg = {
        .drift = drift, .seconds = 240.0, .chunk_ms = chunk_ms, .queue_ms = queue_ms,
        .amplitude = 0.9 * (1 << 23), .tone_hz = 440.0,
    };
    sim_result_t r;
    simulate(&cfg, &r);
    double want_ppm = drift * 1e6;
    printf("%s: baseline %.1f ms, late lows %+.1f..%+.1f ms, max %.1f ms, rate %+.0f ppm (max %.0f, slew %.0f ppm/s), "
           "max step %.0f (sine %.0f), underruns %u\n",
           what, r.baseline_ms, r.ring_min_ms_late, r.ring_max_ms_late, r.ring_max_ms, r.final_ppm, r.max_ppm,
           r.max_slew_ppm_per_s, r.max_step, r.step_bound, r.underruns);
    CHECK(r.learned, "%s: baseline learned", what);
    /* Hermite on a 440 Hz sine is exact to a few LSB; any cut would be a
     * step of up to twice the amplitude. */
    CHECK(r.max_step <= r.step_bound + 16.0, "%s: no discontinuity (%.0f > %.0f)", what, r.max_step, r.step_bound);
    CHECK(r.unpaired == 0, "%s: channels interpolated alike (%u)", what, r.unpaired);
    CHECK(r.underruns == 0,"%s: output never ran dry (%u)", what, r.underruns);
    CHECK(r.ring_min_ms_late > -10.0 && r.ring_max_ms_late < 10.0, "%s: ring held near baseline (%+.1f..%+.1f ms)",
          what, r.ring_min_ms_late, r.ring_max_ms_late);
    CHECK(r.ring_max_ms < r.baseline_ms + chunk_ms + 30.0, "%s: ring bounded (%.1f ms)", what, r.ring_max_ms);
    CHECK(r.max_ppm <= USB_DAC_RATE_MAX_PPM + 1e-6, "%s: within the clamp (%.0f)", what, r.max_ppm);
    CHECK(fabs(r.final_ppm - want_ppm) < 150.0, "%s: settled on the drift (%+.0f vs %+.0f ppm)", what, r.final_ppm,
          want_ppm);
    CHECK(r.max_slew_ppm_per_s <= USB_DAC_RATE_SLEW_PPM_PER_S + 1.0, "%s: slow ratio changes (%.0f ppm/s)", what,
          r.max_slew_ppm_per_s);
}

/* ---- the resampler alone -------------------------------------------------------- */

static void test_unity_is_exact(void) {
    usb_dac_resampler_reset(&rs);
    int32_t out[OUT_CAP * 2];
    uint32_t seed = 1;
    int32_t sent[4096 * 2];
    size_t sent_n = 0, got_n = 0;
    bool exact = true;
    for (int round = 0; round < 4; round++) {
        size_t room = 0;
        int32_t * dst = usb_dac_resampler_input(&rs, &room);
        size_t frames = 1000 + (size_t) round * 7;
        CHECK(room >= frames, "room for a period (%zu)", room);
        for (size_t i = 0; i < frames * 2; i++) {
            seed = seed * 1103515245u + 12345u;
            int32_t v = (int32_t) (seed >> 8) - (1 << 23); /* any 24-bit value */
            dst[i] = v;
            sent[sent_n * 2 + i] = v;
        }
        sent_n += frames;
        size_t n = usb_dac_resampler_run(&rs, frames, usb_dac_resampler_step(1.0), out, OUT_CAP);
        for (size_t k = 0; k < n * 2; k++)
            if (out[k] != sent[got_n * 2 + k]) exact = false;
        got_n += n;
    }
    CHECK(exact, "a ratio of 1 passes samples unchanged");
    CHECK(got_n + 2 == sent_n, "two frames held back (%zu of %zu)", got_n, sent_n);
}

static void test_clamp_and_counts(void) {
    /* Full-scale square wave: Hermite overshoots, the output must clamp. */
    usb_dac_resampler_reset(&rs);
    int32_t out[OUT_CAP * 2];
    bool in_range = true;
    size_t total_in = 0, total_out = 0, max_out = 0;
    int64_t step = usb_dac_resampler_step(1.0 - USB_DAC_RATE_MAX_PPM / 1e6);
    for (int round = 0; round < 200; round++) {
        size_t room = 0;
        int32_t * dst = usb_dac_resampler_input(&rs, &room);
        size_t frames = PERIOD < room ? PERIOD : room;
        for (size_t i = 0; i < frames; i++) {
            int32_t v = ((total_in + i) / 3) % 2 ? (1 << 23) - 1 : -(1 << 23);
            dst[i * 2] = v;
            dst[i * 2 + 1] = v;
        }
        total_in += frames;
        size_t n = usb_dac_resampler_run(&rs, frames, step, out, OUT_CAP);
        if (n > max_out) max_out = n;
        for (size_t k = 0; k < n * 2; k++)
            if (out[k] > (1 << 23) - 1 || out[k] < -(1 << 23)) in_range = false;
        total_out += n;
    }
    CHECK(in_range, "output clamped to 24 bits");
    CHECK(max_out <= PERIOD + 4, "one period in, at most a few frames more out (%zu)", max_out);
    double ratio = (double) total_in / (double) total_out;
    CHECK(fabs(ratio - (1.0 - USB_DAC_RATE_MAX_PPM / 1e6)) < 1e-5, "frames follow the ratio (%.6f)", ratio);
}

/* DoP and native DSD: the controller learns the level (for the bridge's
 * bounded trim) but never moves the ratio. */
static void test_no_adapt(void) {
    usb_dac_rate_ctl_t ctl;
    usb_dac_rate_ctl_init(&ctl, 176400);
    double occupancy = 176400 * 0.05, ratio = 1.0;
    for (int ms = 0; ms < 60000; ms++) {
        occupancy += 176400 * 0.0009 / 1000.0;
        ratio = usb_dac_rate_ctl_update(&ctl, occupancy, 0.001, false);
        if (ratio != 1.0) break;
    }
    CHECK(ratio == 1.0, "no correction without adapt (%.9f)", ratio);
    CHECK(ctl.learned, "level learned without adapt");
    CHECK((ctl.last_window_min - ctl.baseline) / 176400.0 > 0.020, "the surplus shows above baseline");
}

/* Cost on this machine, for scale (the device is a single in-order core). */
static void bench(void) {
    usb_dac_resampler_reset(&rs);
    int32_t out[OUT_CAP * 2];
    int64_t step = usb_dac_resampler_step(1.0009);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    size_t frames_total = 0;
    for (int round = 0; round < 2000; round++) {
        size_t room = 0;
        int32_t * dst = usb_dac_resampler_input(&rs, &room);
        size_t frames = PERIOD < room ? PERIOD : room;
        for (size_t i = 0; i < frames * 2; i++) dst[i] = (int32_t) ((i * 2654435761u) >> 8) - (1 << 23);
        usb_dac_resampler_run(&rs, frames, step, out, OUT_CAP);
        frames_total += frames;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (double) (b.tv_sec - a.tv_sec) * 1e9 + (double) (b.tv_nsec - a.tv_nsec);
    printf("resampler: %.1f ns per stereo frame here (%.2f%% of a core at 384 kHz)\n", ns / (double) frames_total,
           ns / (double) frames_total * 384000.0 / 1e7);
}

int main(void) {
    test_unity_is_exact();
    test_clamp_and_counts();
    test_no_adapt();
    test_drift(0.0009, 100.0, 150.0, "host +900 ppm, 100 ms bursts");
    test_drift(0.0008, 20.0, 60.0, "host +800 ppm, 20 ms writes");
    test_drift(-0.0009, 100.0, 150.0, "host -900 ppm, 100 ms bursts");
    test_drift(0.0, 100.0, 150.0, "same clocks");
    bench();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("usb_dac_rate: all tests passed\n");
    return 0;
}
