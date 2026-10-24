#include "mt_features.h"

#include <string.h>

const mt_feature_info_t MT_FEATURES[] = {
    { "feature.airplay",       MT_HAS_FEATURE(AIRPLAY) },
    { "feature.dlna_renderer", MT_HAS_FEATURE(DLNA_RENDERER) },
    { "feature.wifi_import",   MT_HAS_FEATURE(WIFI_IMPORT) },
    { "feature.remote",        MT_HAS_FEATURE(REMOTE) },
    { "feature.codec_opus",    MT_HAS_FEATURE(CODEC_OPUS) },
    { "feature.codec_wma",     MT_HAS_FEATURE(CODEC_WMA) },
    { "feature.i18n",          MT_HAS_FEATURE(I18N) },
};
const size_t MT_FEATURE_COUNT = sizeof(MT_FEATURES) / sizeof(MT_FEATURES[0]);

bool mt_feature_enabled(const char * name) {
    if (!name) return false;
    for (size_t i = 0; i < MT_FEATURE_COUNT; i++)
        if (strcmp(name, MT_FEATURES[i].name) == 0) return MT_FEATURES[i].enabled;
    return false;
}
