#ifndef POWER_SUSPEND_H
#define POWER_SUSPEND_H

#include <stdbool.h>

/* Initiates suspend-to-RAM via /sys/power/state. Wraps suspension with
 * framebuffer blanking (FB_BLANK_POWERDOWN / FB_BLANK_UNBLANK) to prevent
 * display driver timeouts on resume.
 *
 * Tears down active Wi-Fi and Bluetooth radios before suspend, and restores
 * previously active radios in a background thread upon waking so the GUI
 * can unblank the display immediately without waiting for radio re-initialization. */
bool power_suspend_now(void); /* true: the kernel suspended (the "mem" write succeeded) */

/* Developer Options "Allow suspend" (on by default): lets a board marked
 * BOARD_POWER_NO_SUSPEND (the R3 Pro) use the stock player's standby
 * suspend. */
void power_suspend_set_allowed(bool allowed);

/* Whether power_suspend_now() really writes /sys/power/state on this board:
 * no BOARD_POWER_NO_SUSPEND quirk, or the developer override above. */
bool power_suspend_available(void);

#endif /* POWER_SUSPEND_H */
