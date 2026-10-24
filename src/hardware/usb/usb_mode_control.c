/* USB device modes for the UI: the board's gadget back end does
 * the work (usb_gadget.h: configfs on X1600, android_usb on the R3 Pro);
 * this file adds what every board shares -- the USB DAC bridge follows DAC
 * mode. */
#include "usb_mode_control.h"
#include "battery.h"
#include "usb_dac_bridge.h"
#include "usb_gadget.h"

#include <stdio.h>

bool usb_mode_control_cable_connected(void) {
    return battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED;
}

bool usb_mode_control_cable_state(bool * connected) {
    battery_external_power_state_t state = battery_get_external_power_state();
    if (state == BATTERY_EXTERNAL_POWER_UNKNOWN) return false;
    *connected = state == BATTERY_EXTERNAL_POWER_CONNECTED;
    return true;
}

bool usb_mode_control_apply(usb_mode_t mode) {
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    if (!ops || !ops->apply) {
        fprintf(stderr, "usb_mode_control: no USB gadget on this board\n");
        return false;
    }
    usb_dac_bridge_stop(); /* no-op unless we're actually leaving DAC mode */
    bool ok = ops->apply(mode);
    if (ok && mode == USB_MODE_DAC) usb_dac_bridge_start();
    return ok;
}

bool usb_mode_control_detect_current(usb_mode_t * out_mode) {
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    return ops && ops->detect && ops->detect(out_mode);
}

bool usb_mode_control_storage_host_configured(void) {
    const struct board_usb_gadget_ops * ops = usb_gadget_ops();
    return ops && ops->storage_host_configured && ops->storage_host_configured();
}
