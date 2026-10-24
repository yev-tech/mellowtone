#include "gesture_detector.h"

bool gesture_home_state_is_eligible(const gesture_home_config_t * cfg, int32_t touch_y) {
    if (!cfg) return false;

    bool in_band = (touch_y >= cfg->screen_height - cfg->band_height);
    return cfg->swipe_up_home_enabled &&
           !cfg->quick_drawer_open &&
           !cfg->is_bt_dac_overlay &&
           !cfg->is_usb_dac_overlay &&
           !cfg->has_background_work &&
           in_band;
}
