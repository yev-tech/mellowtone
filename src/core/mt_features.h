#ifndef MT_FEATURES_H
#define MT_FEATURES_H

/* Optional software features, chosen per board preset at
 * build time: CMake options MT_FEATURE_AIRPLAY, MT_FEATURE_DLNA_RENDERER,
 * MT_FEATURE_WIFI_IMPORT, MT_FEATURE_REMOTE, MT_CODEC_OPUS, MT_CODEC_WMA and
 * MT_I18N, defaulted by boards/<name>/board.cmake (MT_PRESET_*), passed to
 * every player source as 0/1.
 *
 *   if (MT_HAS_FEATURE(CODEC_OPUS)) ...   (or #if, where code must not compile)
 *
 * Hardware is not a feature: see board.h (board_has()). Requirements on
 * capabilities are static-asserted in each board.c.
 *
 * A feature that is off is not in the binary: CMake leaves its sources and
 * libraries out (cmake/features.cmake), its header declares inert inline
 * stubs instead, and the UI does not build its screens, rows or toggles;
 * plain if (MT_HAS_FEATURE(X)) tests are enough for the compiler to drop
 * the code behind them. MT_FEATURES below is therefore exactly what is
 * built; `mellowtone --features` prints it. */

#include <stdbool.h>
#include <stddef.h>

#if !defined(MT_FEATURE_AIRPLAY) || !defined(MT_FEATURE_DLNA_RENDERER) || \
    !defined(MT_FEATURE_WIFI_IMPORT) || !defined(MT_FEATURE_REMOTE) ||    \
    !defined(MT_CODEC_OPUS) || !defined(MT_CODEC_WMA) || !defined(MT_I18N)
#error "feature options missing: they come from CMakeLists.txt (MT_FEATURE_*, MT_CODEC_*, MT_I18N)"
#endif

#define MT_FEAT_AIRPLAY       MT_FEATURE_AIRPLAY
#define MT_FEAT_DLNA_RENDERER MT_FEATURE_DLNA_RENDERER
#define MT_FEAT_WIFI_IMPORT   MT_FEATURE_WIFI_IMPORT
#define MT_FEAT_REMOTE        MT_FEATURE_REMOTE
#define MT_FEAT_CODEC_OPUS    MT_CODEC_OPUS
#define MT_FEAT_CODEC_WMA     MT_CODEC_WMA
#define MT_FEAT_I18N          MT_I18N

/* X: AIRPLAY, DLNA_RENDERER, WIFI_IMPORT, REMOTE, CODEC_OPUS, CODEC_WMA, I18N. */
#define MT_HAS_FEATURE(X) (MT_FEAT_##X)

/* Feature tokens for plugins: "feature.airplay", ... */
typedef struct {
    const char * name;
    bool enabled;
} mt_feature_info_t;

extern const mt_feature_info_t MT_FEATURES[];
extern const size_t MT_FEATURE_COUNT;

/* True when `name` is a known feature token and it is built in. */
bool mt_feature_enabled(const char * name);

#endif /* MT_FEATURES_H */
