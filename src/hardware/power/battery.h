#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stddef.h>

/* Battery reader: one for every board, nodes and quirks from the
 * descriptor. battery_init() hooks the cache to power_events (uevents,
 * CHARGE:* messages, 60 s fallback poll); without it values are cached 5 s. */
void battery_init(void);
/* Drops the cached capacity/status: the next read goes to sysfs. */
void battery_invalidate(void);

/* Battery percentage (0-100), or -1 if no battery power_supply device could
 * be found (e.g. running on the host simulator, or the class just doesn't
 * exist). Never faked -- callers should treat -1 as "no data", not 0%. */
int battery_get_percent(void);

/* UI-only stabilized percentage. Keeps physically impossible direction
 * changes and fuel-gauge recalibration jumps out of the top bar while the
 * raw getter above remains available to safety/charge-limit logic. */
int battery_get_display_percent(void);

/* Physical external-power state, independent of the PMIC charger's own
 * status reporting -- charge control (see charge_control.h) can switch
 * charging off (stop/resume boards, "Don't charge in DAC mode"), which makes
 * the status read "Discharging"/"Not charging" with the cable in, and a
 * status node can also be stale or momentarily unreadable.
 *
 * UNKNOWN is deliberately distinct from DISCONNECTED.  Callers that can
 * trigger destructive actions (such as powering the device off) must ignore a
 * transient sysfs read failure instead of treating it as a cable removal. */
typedef enum {
    BATTERY_EXTERNAL_POWER_UNKNOWN = -1,
    BATTERY_EXTERNAL_POWER_DISCONNECTED = 0,
    BATTERY_EXTERNAL_POWER_CONNECTED = 1,
} battery_external_power_state_t;

battery_external_power_state_t battery_get_external_power_state(void);

/* True if the selected battery status says "Charging" or "Full".  This is
 * charger activity/status, not a reliable physical-cable detector; use
 * battery_get_external_power_state() for that. */
bool battery_is_charging(void);

/* True only for "Full" specifically (plugged in, topped up) -- distinct
 * from battery_is_charging(), which also counts "Full" as charging. False
 * on the host simulator or if no battery device is found. */
bool battery_is_full(void);

/* The selected status string ("Charging", "Full", "Discharging",
 * "Not charging", ...), after the descriptor's status quirk. */
bool battery_get_status(char * out, size_t out_size);

/* CAP_BATTERY_CURRENT: current_now of BOARD.power.current_node (or the
 * battery node), in microamps as the driver reports it (sign varies by
 * driver). False without the capability or the attribute. */
bool battery_get_current_ua(int * out_ua);

/* Tests only: forget the discovered node, the cache and the display state. */
void battery_test_reset(void);

#endif /* BATTERY_H */
