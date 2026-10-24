#ifndef CHARGER_H
#define CHARGER_H

/* Charger back ends: the descriptor's .charger points at one of
 *
 *   charger_axp2101_i2c_ops  AXP2101 registers over /dev/i2c-0, plus the
 *                            MP2731 next to it where BOARD.power.charge_chips
 *                            says so (R1, R3 Pro II, R3 II 2025)
 *   charger_sysfs_ops        generic power_supply attributes, whichever exist
 *                            and are writable, and the USB input-current
 *                            policy table (BOARD.power.input_current; R3 Pro)
 *
 * Not every back end can do every op on every board: supported() says which
 * ones work right now (CHARGER_OP_*). Callers (charge_control.c, the
 * input-current policy below) check it; an op that is not supported returns
 * false without touching anything. */

#include "board.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    CHARGER_OP_SET_CHARGING = 1u << 0,   /* switch charging on/off */
    CHARGER_OP_INPUT_CURRENT = 1u << 1,  /* USB input-current policy */
    CHARGER_OP_CHARGE_VOLTAGE = 1u << 2, /* charge-termination voltage */
    CHARGER_OP_CURRENT_CAP = 1u << 3,    /* the 500 mA "Safe Charging" cap */
    CHARGER_OP_CHARGE_DONE = 1u << 4,    /* "charging finished" status */
};

/* What is on the USB port, for the input-current policy. */
typedef enum {
    CHARGER_INPUT_NONE = 0, /* unplugged (or unknown): nothing is written */
    CHARGER_INPUT_HOST,     /* a computer enumerated the gadget */
    CHARGER_INPUT_CHARGER,  /* VBUS without enumeration: a wall charger */
    CHARGER_INPUT_HOST_NO_CHARGE, /* HOST while "Don't charge in USB DAC mode" holds
                                     (policy.dac_no_charge, charger_input_current_set_inhibit()) */
} charger_input_t;

/* set_charge_voltage(CHARGER_VOLTAGE_STOCK): back to the board's stock target. */
#define CHARGER_VOLTAGE_STOCK 0

struct board_charger_ops {
    const char * name;
    unsigned (*supported)(void); /* CHARGER_OP_* usable now */
    /* Each returns true once the hardware is in the requested state (read
     * back where the back end can). Safe to repeat: unchanged values are not
     * rewritten. */
    bool (*set_charging)(bool enable);
    bool (*set_input_current)(charger_input_t input);
    bool (*set_charge_voltage)(int millivolts); /* or CHARGER_VOLTAGE_STOCK */
    bool (*set_current_cap)(bool on);           /* on: <= 500 mA; off: what it was before */
    bool (*charge_done)(void);
};

/* The board's back end (power_board()->charger) or NULL. */
const struct board_charger_ops * charger_ops(void);
/* charger_ops()->supported(), 0 without a back end. */
unsigned charger_supported(void);

/* --- USB input-current policy (CAP_INPUT_CURRENT) ---------------------------
 * What the stock R3 Pro player did in a 2 s loop (hiby_player FUN_0046e820),
 * now driven by power_events: uevents (power_supply, android_usb), CHARGE:*
 * messages, a 2 s re-check for 10 s after each of those (the driver may still
 * reset its limits right after a plug), and the 60 s fallback poll. */

/* From the policy's gadget state and usb_node/online. */
charger_input_t charger_detect_input(void);
const char * charger_input_name(charger_input_t input);
/* Detects and applies once; false if a write failed. */
bool charger_input_current_apply(void);
/* Subscribes the policy to power_events (no-op without CAP_INPUT_CURRENT or
 * a back end that can apply it). */
void charger_input_current_start(void);

/* "Don't charge in USB DAC mode" through the policy (the stock R3 Pro's
 * dac_charge_disable: a host row that draws no charge current), where the
 * descriptor has that row. set_inhibit re-applies at once when it changes. */
bool charger_input_inhibit_supported(void);
void charger_input_current_set_inhibit(bool inhibit);

/* --- test hooks --------------------------------------------------------- */

/* One SMBus byte transfer; the AXP2101 back end's only way to the chips. */
typedef bool (*charger_i2c_xfer_fn)(const char * bus, uint8_t addr, uint8_t reg, uint8_t * value, bool write);
/* xfer NULL: the real /dev/i2c-* ioctl. baseline_path non-NULL: the current
 * baseline file lives there (no card checks). Also forgets the loaded
 * baseline and the charging switch state. */
void charger_axp2101_test_hooks(charger_i2c_xfer_fn xfer, const char * baseline_path);
/* Forgets the captured original values; enable/disable the R3 Pro axp_battery
 * chgmilli* candidate (off by default, see charger_sysfs.c). */
void charger_sysfs_test_reset(bool enable_axp_chgmilli);

#endif /* CHARGER_H */
