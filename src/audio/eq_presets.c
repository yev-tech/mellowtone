#include "eq_presets.h"
#include "peq.h"

#include <math.h>

const double eq_graphic_freq_hz[EQ_GRAPHIC_BANDS] = { 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 };
const char * const eq_graphic_freq_label[EQ_GRAPHIC_BANDS] = { "60", "170", "310", "600", "1k", "3k", "6k", "12k", "14k", "16k" };

/* Winamp 5 defaults as exported to XMMS presets (wiki.daveking.com, "XMMS
 * Equalizer Presets from WinAmp"); Jazz, Acoustic and Vocal are common
 * extras Winamp never shipped. */
static const eq_preset_t presets[] = {
    { "Rock", { 8, 4.8, -5.6, -8, -3.2, 4, 8.8, 11.2, 11.2, 11.2 }, true },
    { "Pop", { -1.6, 4.8, 7.2, 8, 5.6, 0, -2.4, -2.4, -1.6, -1.6 }, true },
    { "Jazz", { 4, 3, 1, 2, -1.5, -1.5, 0, 1.5, 3, 4 }, false },
    { "Classical", { 0, 0, 0, 0, 0, 0, -7.2, -7.2, -7.2, -9.6 }, true },
    { "Dance", { 9.6, 7.2, 2.4, 0, 0, -5.6, -7.2, -7.2, 0, 0 }, true },
    { "Club", { 0, 0, 3.2, 5.6, 5.6, 5.6, 3.2, 0, 0, 0 }, true },
    { "Full Bass", { 9.6, 9.6, 9.6, 5.6, 1.6, -4, -8, -10.4, -11.2, -11.2 }, true },
    { "Full Bass & Treble", { 7.2, 5.6, 0, -7.2, -4.8, 1.6, 8, 11.2, 12, 12 }, true },
    { "Full Treble", { -9.6, -9.6, -9.6, -4, 2.4, 11.2, 16, 16, 16, 16.8 }, true },
    { "Headphones", { 4.8, 11.2, 5.6, -3.2, -2.4, 1.6, 4.8, 9.6, 12.8, 14.4 }, true },
    { "Large Hall", { 10.4, 10.4, 5.6, 5.6, 0, -4.8, -4.8, -4.8, 0, 0 }, true },
    { "Live", { -4.8, 0, 4, 5.6, 5.6, 5.6, 4, 2.4, 2.4, 2.4 }, true },
    { "Party", { 7.2, 7.2, 0, 0, 0, 0, 0, 0, 7.2, 7.2 }, true },
    { "Reggae", { 0, 0, 0, -5.6, 0, 6.4, 6.4, 0, 0, 0 }, true },
    { "Ska", { -2.4, -4.8, -4, 0, 4, 5.6, 8.8, 9.6, 11.2, 9.6 }, true },
    { "Soft", { 4.8, 1.6, 0, -2.4, 0, 4, 8, 9.6, 11.2, 12 }, true },
    { "Soft Rock", { 4, 4, 2.4, 0, -4, -5.6, -3.2, 0, 2.4, 8.8 }, true },
    { "Techno", { 8, 5.6, 0, -5.6, -4.8, 0, 8, 9.6, 9.6, 8.8 }, true },
    { "Acoustic", { 4.5, 4.5, 3.5, 1, 1.5, 1.5, 3, 3.5, 3, 1.5 }, false },
    { "Vocal", { -2, -3, -3, 1.5, 3.5, 3.5, 3, 1.5, 0, -1.5 }, false },
};

int eq_preset_count(void) {
    return (int) (sizeof(presets) / sizeof(presets[0]));
}

const eq_preset_t * eq_preset_get(int index) {
    return index >= 0 && index < eq_preset_count() ? &presets[index] : 0;
}

void eq_preset_fitted_gains(int index, double out[EQ_GRAPHIC_BANDS]) {
    const eq_preset_t * p = eq_preset_get(index);
    double peak = 0;
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        out[i] = p ? p->gain_db[i] : 0;
        if (fabs(out[i]) > peak) peak = fabs(out[i]);
    }
    if (peak > EQ_GRAPHIC_GAIN_MAX_DB)
        for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) out[i] = round(out[i] * EQ_GRAPHIC_GAIN_MAX_DB / peak * 10) / 10;
}

/* Q from each band's average octave distance to its neighbours. */
static double band_q(int i) {
    const double * f = eq_graphic_freq_hz;
    double lo = i ? log2(f[i] / f[i - 1]) : log2(f[1] / f[0]);
    double hi = i < EQ_GRAPHIC_BANDS - 1 ? log2(f[i + 1] / f[i]) : log2(f[i] / f[i - 1]);
    double bw = (lo + hi) / 2, p = pow(2.0, bw);
    return sqrt(p) / (p - 1);
}

/* Response at the centres, preamp excluded. */
static void centre_response(double out[EQ_GRAPHIC_BANDS]) {
    peq_get_response_db(eq_graphic_freq_hz, out, EQ_GRAPHIC_BANDS, 48000);
    double pre = peq_get_preamp_db();
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) out[i] -= pre;
}

static eq_preamp_mode_t preamp_mode = EQ_PREAMP_BALANCED;

void eq_graphic_set_preamp_mode(eq_preamp_mode_t mode) {
    preamp_mode = mode;
}

/* Long-term music spectrum, relative: flat to 500 Hz, then about
 * -4.5 dB/octave. A treble boost rarely reaches full scale because there is
 * little energy up there to boost. */
static double music_weight_db(double f) {
    return f > 500 ? -4.5 * log2(f / 500) : 0;
}

/* Headroom the curve needs, preamp excluded, under the current mode. */
static double needed_cut_db(void) {
    enum { N = 96 };
    double f[N], db[N];
    for (int i = 0; i < N; i++) f[i] = 20.0 * pow(1000.0, (double) i / (N - 1));
    peq_get_response_db(f, db, N, 48000);
    double pre = peq_get_preamp_db(), peak = -1e9;
    for (int i = 0; i < N; i++) {
        double v = db[i] - pre + (preamp_mode == EQ_PREAMP_BALANCED ? music_weight_db(f[i]) : 0);
        if (v > peak) peak = v;
    }
    if (preamp_mode == EQ_PREAMP_OFF) return 0;
    if (preamp_mode == EQ_PREAMP_BALANCED) peak -= 3; /* left to the limiter */
    return peak > 0 ? peak : 0;
}

void eq_graphic_apply(const double gains_db[EQ_GRAPHIC_BANDS]) {
    double g[EQ_GRAPHIC_BANDS], r[EQ_GRAPHIC_BANDS];
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        g[i] = gains_db[i];
        peq_set_band_type(i, PEQ_TYPE_PEAKING);
        peq_set_band_enabled(i, true);
    }
    /* Neighbouring bands overlap; correct each gain by the error at its own
     * centre. Converges in a few passes for every preset (simulated). */
    for (int pass = 0; pass < 8; pass++) {
        for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) peq_set_band(i, eq_graphic_freq_hz[i], g[i], band_q(i));
        centre_response(r);
        for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
            g[i] += 0.8 * (gains_db[i] - r[i]);
            if (g[i] > 24) g[i] = 24;
            if (g[i] < -24) g[i] = -24;
        }
    }
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) peq_set_band(i, eq_graphic_freq_hz[i], g[i], band_q(i));
    peq_set_preamp_db(-round(needed_cut_db() * 10) / 10);
}

static bool on_grid(void) {
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        const peq_band_t * b = peq_get_band(i);
        if (!b || !b->enabled || b->type != PEQ_TYPE_PEAKING || fabs(b->q - band_q(i)) > 0.01 ||
            fabs(b->freq_hz - eq_graphic_freq_hz[i]) > eq_graphic_freq_hz[i] * 0.01) return false;
    }
    return true;
}

bool eq_graphic_read(double gains_db[EQ_GRAPHIC_BANDS]) {
    centre_response(gains_db);
    for (int i = 0; i < EQ_GRAPHIC_BANDS; i++) {
        gains_db[i] = round(gains_db[i] * 10) / 10;
        if (gains_db[i] > EQ_GRAPHIC_GAIN_MAX_DB) gains_db[i] = EQ_GRAPHIC_GAIN_MAX_DB;
        if (gains_db[i] < -EQ_GRAPHIC_GAIN_MAX_DB) gains_db[i] = -EQ_GRAPHIC_GAIN_MAX_DB;
    }
    return on_grid();
}

int eq_preset_match(void) {
    double cur[EQ_GRAPHIC_BANDS];
    if (!eq_graphic_read(cur)) return -1;
    for (int p = 0; p < eq_preset_count(); p++) {
        double g[EQ_GRAPHIC_BANDS];
        eq_preset_fitted_gains(p, g);
        bool same = true;
        for (int i = 0; i < EQ_GRAPHIC_BANDS && same; i++) same = fabs(cur[i] - g[i]) < 0.15;
        if (same) return p;
    }
    return -1;
}

void eq_preset_apply(int index) {
    double g[EQ_GRAPHIC_BANDS];
    eq_preset_fitted_gains(index, g);
    eq_graphic_apply(g);
}
