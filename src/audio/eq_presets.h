#ifndef EQ_PRESETS_H
#define EQ_PRESETS_H

#include <stdbool.h>

/* Winamp-style graphic EQ on top of the parametric engine (peq.h): ten
 * peaking bands fixed at Winamp's centres with one shared Q, plus named
 * presets. Each band's Q follows its spacing to its neighbours, and the
 * band gains are solved so the response AT each centre equals its slider
 * (Winamp's 12k/14k/16k are a quarter octave apart and would otherwise stack
 * to more than twice the boost). Presets beyond the sliders' range are
 * scaled to fit, and the preamp is set to the curve's peak so they do not
 * clip. */
#define EQ_GRAPHIC_BANDS 10
#define EQ_GRAPHIC_GAIN_MAX_DB 12.0

extern const double eq_graphic_freq_hz[EQ_GRAPHIC_BANDS];
extern const char * const eq_graphic_freq_label[EQ_GRAPHIC_BANDS];

typedef struct {
    const char * name;
    float gain_db[EQ_GRAPHIC_BANDS];
    bool winamp; /* one of Winamp's own defaults */
} eq_preset_t;

int eq_preset_count(void);
const eq_preset_t * eq_preset_get(int index);
/* A preset's gains as applied: scaled into +-EQ_GRAPHIC_GAIN_MAX_DB. */
void eq_preset_fitted_gains(int index, double out[EQ_GRAPHIC_BANDS]);

/* How eq_graphic_apply() sets the preamp. */
typedef enum {
    EQ_PREAMP_BALANCED, /* peak of the curve weighted by a typical music
                         * spectrum, less 3 dB the limiter absorbs */
    EQ_PREAMP_SAFE,     /* the curve's full peak: never clips */
    EQ_PREAMP_OFF,      /* 0 dB, as Winamp; the limiter catches overs */
} eq_preamp_mode_t;
void eq_graphic_set_preamp_mode(eq_preamp_mode_t mode);

/* Writes the ten bands (peaking, enabled, Winamp centres, solved gains) and
 * the automatic preamp. Does not save. */
void eq_graphic_apply(const double gains_db[EQ_GRAPHIC_BANDS]);
/* Slider values: the combined response at the centres, preamp excluded.
 * Returns true when the bands are on the graphic grid; otherwise the values
 * approximate a parametric curve. */
bool eq_graphic_read(double gains_db[EQ_GRAPHIC_BANDS]);
/* Index of the preset the bands currently match, or -1. */
int eq_preset_match(void);
void eq_preset_apply(int index);

#endif
