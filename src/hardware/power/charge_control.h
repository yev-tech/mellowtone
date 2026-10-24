#ifndef CHARGE_CONTROL_H
#define CHARGE_CONTROL_H

/* Charge control (CAP_CHARGE_CONTROL): the "Charge Limit (85%)"
 * and "Safe Charging (500mA)" settings and "Don't charge in DAC mode", on
 * top of the board's charger back end (charger.h). How the limit is held
 * comes from the descriptor (BOARD.power.charge_method):
 *
 *   VOLTAGE_CAP   the charge-termination voltage is lowered to 4.2 V (about
 *                 85 %) while the setting is on and put back to the board's
 *                 stock target when it is off (AXP2101 boards).
 *                 Re-asserted every 5 s, retried after 1 s on an I2C
 *                 failure.
 *   STOP_RESUME   for chargers that only switch on/off: charging stops at
 *                 85 % and resumes at 80 % (charge_hysteresis_step()).
 *                 Evaluated on power events (uevents, CHARGE:*, the 60 s
 *                 fallback poll) instead of a timer.
 *
 * All calls come from the UI thread (gui.c's tick and the settings
 * switches). */

#include "battery.h"

#include <stdbool.h>

#define CHARGE_LIMIT_PERCENT 85
#define CHARGE_RESUME_PERCENT 80
#define CHARGE_LIMIT_MILLIVOLTS 4200

/* Hooks the stop/resume method to power_events (no-op otherwise). */
void charge_control_init(void);

/* The charge limit: called every UI tick with the setting (throttled
 * inside); force applies now (start-up, the switch). */
void charge_control_poll(bool limit_enabled, bool force);

/* The 500 mA cap: same calling pattern (5 s re-assert, 1 s retry). */
void charge_control_safe_current_poll(bool enabled, bool force);

/* "Don't charge in DAC mode": while inhibit is true charging is switched
 * off (charger set_charging(false)); applied by the next charge_control_poll().
 * Ignored where !charge_control_inhibit_supported(). */
void charge_control_set_inhibit(bool inhibit);
bool charge_control_inhibit_supported(void);
bool charge_control_safe_current_supported(void);

/* True while the limit is in effect: the voltage cap was applied
 * (VOLTAGE_CAP), or charging is stopped at the limit (STOP_RESUME).
 * Software intent, not tied to a percentage (idle-shutdown checks). */
bool charge_control_is_holding(void);

/* True once the limit holds AND charging has actually stopped under it
 * (VOLTAGE_CAP: the charger's "done" status; STOP_RESUME: charging switched
 * off at the limit): "capped and no longer charging", for the LED, the
 * status-bar bolt and plugins. */
bool charge_control_is_confirmed_off(void);

/* The STOP_RESUME state machine (pure; exposed for tests). Unplugged resets
 * it (the next plug-in starts charging unless already at the limit); an
 * unknown capacity or power state keeps the current decision. Returns
 * whether charging should be on. */
typedef struct {
    bool stopped;
} charge_hysteresis_t;

bool charge_hysteresis_step(charge_hysteresis_t * h, int capacity, battery_external_power_state_t power,
                            int limit, int resume);

/* Tests only: forget all state. */
void charge_control_test_reset(void);

#endif /* CHARGE_CONTROL_H */
