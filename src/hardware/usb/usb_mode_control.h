#ifndef USB_MODE_CONTROL_H
#define USB_MODE_CONTROL_H

#include <stdbool.h>

/* The USB device (gadget) modes: Charge only (nothing for the computer, the
 * default), Storage (mass storage), DAC (USB Audio Class 2) and ADB. The
 * board's back ends do the work (usb_gadget.h: configfs on X1600, android_usb
 * on the R3 Pro); where the card is lent rather than shared,
 * usb_storage_lend.h hands it over once Storage is chosen and a computer
 * has the port. Which mode the port is in is runtime state only
 * (usb_connection.h): Charge only at start-up and after every unplug. */
typedef enum {
    USB_MODE_CHARGE = 0,
    USB_MODE_STORAGE,
    USB_MODE_DAC,
    USB_MODE_ADB,
} usb_mode_t;

/* Switches the USB gadget to `mode`. Tears down existing gadget configuration
 * before setting up the target mode. Blocking operation; call off the UI thread.
 *
 * Returns true only if the mode was applied and verified bound to the UDC. */
bool usb_mode_control_apply(usb_mode_t mode);

/* Inspects live configfs/sysfs gadget state to determine which mode is
 * ACTUALLY active right now, independent of what the player last recorded
 * (usb_mode_active(), usb_connection.h) -- the two can disagree after the
 * player restarts mid-connection, a switch made outside this app, or the
 * corruption usb_mode_control_apply() now defends against. Returns true and
 * sets *out_mode only when a gadget is unambiguously bound with its expected
 * function linked; returns false (leaving *out_mode untouched) if nothing is
 * clearly active, so callers keep what they have rather than guessing. */
bool usb_mode_control_detect_current(usb_mode_t * out_mode);

/* Read-only host-enumeration check: true only for a live Storage gadget whose
 * bound controller reports exactly "configured" in /sys/class/udc/<name>/state.
 * Binding or cable power alone is insufficient. Missing/unreadable state,
 * invalid controller names, and DAC/ADB modes return false; no legacy fallback. */
bool usb_mode_control_storage_host_configured(void);

/* Best-effort physical USB-power/cable presence. Does not tell a computer
 * from a charge-only adapter: the gadget's host_connected() does
 * (usb_gadget.h). */
bool usb_mode_control_cable_connected(void);
/* The same, but false (and *connected untouched) when the power state could
 * not be read, which is not an unplug. */
bool usb_mode_control_cable_state(bool * connected);

#endif /* USB_MODE_CONTROL_H */
