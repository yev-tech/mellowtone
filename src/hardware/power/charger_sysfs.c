/* Generic power_supply sysfs charger back end (charger.h).
 *
 * Each op uses whichever standard attribute exists and is writable on the
 * charge path (BOARD.power.charger_node, then battery_node; never usb_node,
 * whose limits belong to the input-current policy):
 *
 *   set_charging        charge_behaviour ("auto" / "inhibit-charge", Linux
 *                       5.17+), else the vendor charge_enabled (1/0)
 *   set_charge_voltage  constant_charge_voltage (uV); CHARGER_VOLTAGE_STOCK
 *                       writes back the value found before our first write
 *   set_current_cap     constant_charge_current_max (uA) lowered to 500 mA
 *                       (never raised), then written back
 *   set_input_current   the descriptor's policy table (BOARD.power.input_current)
 *                       on the gadget's MaxPower and usb_node's
 *                       charge_control_limit_max / constant_charge_current_max
 *   charge_done         battery status "Full"
 *
 * charge_control_limit is not used to stop charging: it is a throttle level
 * (0..charge_control_limit_max), and its lowest current is not "off".
 *
 * Originals are kept in memory only: after a restart the first write
 * captures whatever is there (normally the driver default, as the voltage
 * cap and current cap are put back whenever their setting is off). */
#include "charger.h"
#include "battery.h"
#include "power_sysfs.h"

#include <stdio.h>
#include <string.h>

#define REL_MAX 512
#define SAFE_CURRENT_UA 500000

/* R3 Pro candidate: HiBy's axp_mfd driver exposes writable chgmillicur,
 * chgmilliclscur, chgmilliearcur and chgmillisuscur on power_supply/
 * axp_battery. All read 0, nothing documents
 * them, and no stock binary writes them. DISABLED: unverified. Before
 * enabling, the reversible on-device test from the capability probe must
 * show that a write while charging (watching battery/status and
 * axp_battery/current_now) really stops charging, which value does it, and
 * that writing the old value back resumes it. 0 below is a placeholder:
 * it is what the probe read, so it may well mean "driver default" rather
 * than "stop". Only then can the R3 Pro list CAP_CHARGE_CONTROL with
 * BOARD_CHARGE_METHOD_STOP_RESUME. */
#define AXP_CHGMILLI_NODE "axp_battery"
#define AXP_CHGMILLI_ATTR "chgmillicur"
#define AXP_CHGMILLI_STOP_MA 0
static bool axp_chgmilli_enabled = false;

typedef struct {
    bool valid;
    char rel[REL_MAX];
    int value;
} saved_value_t;

static saved_value_t saved_voltage, saved_current, saved_chgmilli;

/* The first writable `attr` on the charge path. */
static bool find_attr(const char * attr, char * rel, size_t size) {
    const board_desc_t * board = power_board();
    const char * nodes[2] = { board->power.charger_node, board->power.battery_node };
    for (size_t i = 0; i < 2; i++) {
        if (!nodes[i] || (i == 1 && nodes[0] && strcmp(nodes[0], nodes[1]) == 0)) continue;
        if (power_supply_rel(nodes[i], attr, rel, size) && power_sysfs_writable(rel)) return true;
    }
    return false;
}

static bool chgmilli_rel(char * rel, size_t size) {
    return axp_chgmilli_enabled && power_supply_rel(AXP_CHGMILLI_NODE, AXP_CHGMILLI_ATTR, rel, size) &&
           power_sysfs_writable(rel);
}

static unsigned sysfs_supported(void) {
    const board_desc_t * board = power_board();
    char rel[REL_MAX];
    unsigned ops = CHARGER_OP_CHARGE_DONE;
    if (board->power.input_current && board->power.usb_node) ops |= CHARGER_OP_INPUT_CURRENT;
    if (find_attr("charge_behaviour", rel, sizeof(rel)) || find_attr("charge_enabled", rel, sizeof(rel)) ||
        chgmilli_rel(rel, sizeof(rel)))
        ops |= CHARGER_OP_SET_CHARGING;
    if (find_attr("constant_charge_voltage", rel, sizeof(rel))) ops |= CHARGER_OP_CHARGE_VOLTAGE;
    if (find_attr("constant_charge_current_max", rel, sizeof(rel))) ops |= CHARGER_OP_CURRENT_CAP;
    return ops;
}

/* Remembers rel's current value once, before our first write to it. */
static bool capture(saved_value_t * saved, const char * rel) {
    if (saved->valid && strcmp(saved->rel, rel) == 0) return true;
    int v;
    if (!power_sysfs_read_int(rel, &v)) return false;
    snprintf(saved->rel, sizeof(saved->rel), "%s", rel);
    saved->value = v;
    saved->valid = true;
    return true;
}

static bool restore(saved_value_t * saved) {
    if (!saved->valid) return true; /* never changed by us */
    if (!power_sysfs_write_int_if_changed(saved->rel, saved->value)) return false;
    saved->valid = false;
    return true;
}

/* charge_behaviour reads "[auto] inhibit-charge force-discharge" (the
 * selected one in brackets) or just the selected one. */
static bool behaviour_is(const char * rel, const char * want) {
    char cur[128];
    if (!power_sysfs_read(rel, cur, sizeof(cur))) return false;
    char bracketed[48];
    snprintf(bracketed, sizeof(bracketed), "[%s]", want);
    return strcmp(cur, want) == 0 || strstr(cur, bracketed) != NULL;
}

static bool sysfs_set_charging(bool enable) {
    char rel[REL_MAX];
    if (find_attr("charge_behaviour", rel, sizeof(rel))) {
        const char * want = enable ? "auto" : "inhibit-charge";
        if (behaviour_is(rel, want)) return true;
        return power_sysfs_write_str(rel, want) && behaviour_is(rel, want);
    }
    if (find_attr("charge_enabled", rel, sizeof(rel))) return power_sysfs_write_int_if_changed(rel, enable ? 1 : 0);
    if (chgmilli_rel(rel, sizeof(rel))) {
        if (enable) return restore(&saved_chgmilli);
        return capture(&saved_chgmilli, rel) && power_sysfs_write_int_if_changed(rel, AXP_CHGMILLI_STOP_MA);
    }
    return false;
}

static bool sysfs_set_charge_voltage(int millivolts) {
    char rel[REL_MAX];
    if (millivolts == CHARGER_VOLTAGE_STOCK) return restore(&saved_voltage);
    if (millivolts < 0 || !find_attr("constant_charge_voltage", rel, sizeof(rel))) return false;
    return capture(&saved_voltage, rel) && power_sysfs_write_int_if_changed(rel, millivolts * 1000);
}

static bool sysfs_set_current_cap(bool on) {
    if (!on) return restore(&saved_current);
    char rel[REL_MAX];
    if (!find_attr("constant_charge_current_max", rel, sizeof(rel))) return false;
    int cur;
    if (!power_sysfs_read_int(rel, &cur)) return false;
    if (cur > 0 && cur <= SAFE_CURRENT_UA) return true; /* already at or under the cap */
    return capture(&saved_current, rel) && power_sysfs_write_int_if_changed(rel, SAFE_CURRENT_UA);
}

/* One row of the policy, in the stock player's order: MaxPower, then the
 * limit, then the current; each written only when it differs. */
static bool sysfs_set_input_current(charger_input_t input) {
    const board_desc_t * board = power_board();
    const board_input_current_policy_t * policy = board->power.input_current;
    if (!policy || !board->power.usb_node) return false;
    if (input == CHARGER_INPUT_NONE) return true;
    const board_input_current_row_t * row = input == CHARGER_INPUT_HOST              ? &policy->host
                                            : input == CHARGER_INPUT_HOST_NO_CHARGE ? &policy->dac_no_charge
                                                                                    : &policy->charger;
    if (input == CHARGER_INPUT_HOST_NO_CHARGE && !policy->has_dac_no_charge) row = &policy->host;
    char rel[REL_MAX];
    bool ok = true;
    if (row->max_power >= 0 && policy->gadget_max_power)
        ok = power_sysfs_write_int_if_changed(policy->gadget_max_power, row->max_power) && ok;
    if (row->limit_max >= 0 && power_supply_rel(board->power.usb_node, "charge_control_limit_max", rel, sizeof(rel)))
        ok = power_sysfs_write_int_if_changed(rel, row->limit_max) && ok;
    if (row->current_max >= 0 &&
        power_supply_rel(board->power.usb_node, "constant_charge_current_max", rel, sizeof(rel)))
        ok = power_sysfs_write_int_if_changed(rel, row->current_max) && ok;
    return ok;
}

static bool sysfs_charge_done(void) { return battery_is_full(); }

const struct board_charger_ops charger_sysfs_ops = {
    .name = "power_supply-sysfs",
    .supported = sysfs_supported,
    .set_charging = sysfs_set_charging,
    .set_input_current = sysfs_set_input_current,
    .set_charge_voltage = sysfs_set_charge_voltage,
    .set_current_cap = sysfs_set_current_cap,
    .charge_done = sysfs_charge_done,
};

void charger_sysfs_test_reset(bool enable_axp_chgmilli) {
    memset(&saved_voltage, 0, sizeof(saved_voltage));
    memset(&saved_current, 0, sizeof(saved_current));
    memset(&saved_chgmilli, 0, sizeof(saved_chgmilli));
    axp_chgmilli_enabled = enable_axp_chgmilli;
}
