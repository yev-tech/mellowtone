#ifndef GESTURE_DETECTOR_H
#define GESTURE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_SWIPE_HIT_EXTRA_PX 7

typedef struct {
    bool swipe_up_home_enabled;
    bool quick_drawer_open;
    bool is_bt_dac_overlay;
    bool is_usb_dac_overlay;
    bool has_background_work;
    int32_t screen_height;
    int32_t band_height;
} gesture_home_config_t;

/* Stateless predicate: true if touch_y satisfies all configured eligibility rules */
bool gesture_home_state_is_eligible(const gesture_home_config_t * cfg, int32_t touch_y);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_DETECTOR_H */
