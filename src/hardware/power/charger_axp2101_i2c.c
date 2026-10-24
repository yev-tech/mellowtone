/* AXP2101 charger back end (charger.h): the charger's registers behind the
 * charger ops. The throttling and the decision what to ask for live in
 * charge_control.c.
 *
 * Caps the AXP2101's charge-termination voltage (register 0x64, I2C bus 0
 * address 0x34) instead of switching the charger off once a percentage is
 * crossed: CC/CV charging tapers off and completes at the lower ceiling. On
 * the R3 Pro II (BOARD_CHARGER_MP2731 in BOARD.power.charge_chips) the
 * AXP2101's cap has no effect on the charge path; the MP2731 (I2C 0x4b) is
 * capped to its matching encoding at the same time. The stock target is a
 * known per-board constant (BOARD.power.axp_voltage_baseline,
 * MP_VOLTAGE_BASELINE below), from each board's stock boot script.
 *
 * The 500 mA "Safe Charging" cap (set_current_cap) captures the pre-cap
 * current registers the first time and writes them back when turned off.
 * The registers keep the cap across restarts and reboots, so the captured
 * values are kept in internal flash (MT_CHARGE_BASELINE_FILE, written once
 * per device), tagged with the board's name: never on the card, which can
 * be swapped, missing or lent. A register at or below the cap is never
 * captured -- it may be this player's own cap from before a restart.
 *
 * set_charging(false) clears REG18H bit 1 (cell battery charge enable, as
 * X-Powers' own driver library does); the charger is otherwise always kept
 * enabled, so a charge never stays disabled across a restart. Not offered
 * with an MP2731, which drives the charge path itself. */
#include "charger.h"
#include "power_sysfs.h"
#include "storage_paths.h"
#include "board.h"
#include "board_config.h"
#include "battery.h"
#include "debug_log.h"

#include <fcntl.h>
#include <errno.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The board's chips (BOARD.power.charge_chips): the AXP2101 on every X1600
 * board, plus the MP2731 on the R3 Pro II. The R3 Pro has no AXP2101 on
 * /dev/i2c-0 (its PMIC is axp_mfd behind a kernel driver) and uses the sysfs
 * back end instead. */
static bool has_axp2101(void) {
    return (power_board()->power.charge_chips & BOARD_CHARGER_AXP2101) != 0;
}
static bool has_mp2731(void) {
    return (power_board()->power.charge_chips & BOARD_CHARGER_MP2731) != 0;
}
#define AXP_BUS "/dev/i2c-0"
#define AXP_ADDR 0x34
#define AXP_REG_MODULE_EN 0x18
#define AXP_CHG_EN (1u << 1)
#define AXP_REG_STATUS 0x01
#define AXP_REG_VOLTAGE 0x64
#define AXP_REG_CURRENT 0x62
#define AXP_CURRENT_MASK 0x1Fu
/* REG62H[4:0] (icc) encoding, confirmed against the real X-Powers AXP2101
 * datasheet (not assumed): N<=8 selects N*25mA (0-200mA), N>8 selects
 * 200+(N-8)*100mA (200-1500mA). 0x08 (8) is therefore 200mA, NOT 500mA --
 * a pre-existing mistake this file had carried forward uncorrected. 500mA
 * is N=11: 200 + (11-8)*100 = 500. */
#define AXP_CURRENT_CAP 0x0Bu

#define MP_BUS "/dev/i2c-0"
#define MP_ADDR 0x4b
#define MP_REG_CURRENT 0x05
#define MP_REG_VOLTAGE 0x07
#define MP_CURRENT_MASK 0x7Fu
#define MP_CURRENT_CAP 0x04u
#define MP_VOLTAGE_MASK 0xFEu

/* R3 Pro II's dedicated charger IC, MP2731 -- its own stock boot script
 * (module_driver/mp2731.sh) sets "vbat_target=4400000" (4.4V), matching the
 * AXP2101's own 4.4V target on this board. 0xC8 (0b11001000) is this
 * register's documented 4.4V encoding (3.4V offset + the bit weights this
 * file's own MP_VOLTAGE_MASK selects: 640mV + 320mV + 40mV = 1.0V on top of
 * the 3.4V offset). */
#define MP_VOLTAGE_BASELINE 0xC8u

/* 3: in internal flash, tagged with the board's name. (1 also carried a
 * voltage baseline, now a per-board constant; 2 lived on the card.) Any
 * other version, or another board's file, is ignored and replaced. */
#define BASELINE_VERSION 3

typedef struct { bool valid; uint8_t current; } chip_baseline_t;
typedef struct {
    bool loaded;
    chip_baseline_t axp;
    chip_baseline_t mp; /* MP2731 only */
} baseline_t;

static baseline_t baseline;
/* set_charging(): false while charging is switched off on purpose. */
static bool charging_enabled = true;

static charger_i2c_xfer_fn test_xfer;
static const char *test_baseline_path;

static bool dev_xfer(const char *bus, uint8_t addr, uint8_t reg, uint8_t *value, bool write) {
    int fd = open(bus, O_RDWR); if (fd < 0) return false;
    bool ok = false;
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) >= 0) {
        union i2c_smbus_data data;
        if (write) data.byte = *value;
        struct i2c_smbus_ioctl_data args = {
            .read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
            .command = reg, .size = I2C_SMBUS_BYTE_DATA, .data = &data
        };
        if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
            if (!write) *value = data.byte;
            ok = true;
        }
    }
    close(fd); return ok;
}
static bool xfer(const char *bus, uint8_t addr, uint8_t reg, uint8_t *value, bool write) {
    return (test_xfer ? test_xfer : dev_xfer)(bus, addr, reg, value, write);
}
static bool axp_read(uint8_t r, uint8_t *v) { return xfer(AXP_BUS, AXP_ADDR, r, v, false); }
static bool axp_write(uint8_t r, uint8_t v) { return xfer(AXP_BUS, AXP_ADDR, r, &v, true); }
static bool mp_read(uint8_t r, uint8_t *v) { return xfer(MP_BUS, MP_ADDR, r, v, false); }
static bool mp_write(uint8_t r, uint8_t v) { return xfer(MP_BUS, MP_ADDR, r, &v, true); }

/* Only the charge-CURRENT registers need a captured baseline: unlike
 * voltage (see BOARD.power.axp_voltage_baseline and MP_VOLTAGE_BASELINE
 * above), neither
 * board's stock boot script pins a specific charge-current register value
 * -- it is whatever the chip's own factory/OTP default leaves it at, so
 * there is no known constant to hardcode and a live pre-cap read remains
 * the only source of truth. */
/* A missing, stale-version, foreign or unparsable file is NOT a capture
 * failure -- it just means there is no usable persisted baseline yet,
 * exactly like a fresh device: capture_baseline() falls through to a live
 * register read for whatever didn't load. (An earlier revision treated a
 * version mismatch as fatal and could then never apply or restore the cap
 * for the rest of that boot.) A value of 0 means "none captured": a
 * baseline is always above the cap. */
static const char *baseline_path(void) {
    return test_baseline_path ? test_baseline_path : MT_CHARGE_BASELINE_FILE;
}

static void load_baseline(void) {
    FILE *f = fopen(baseline_path(), "re");
    if (!f) return;
    unsigned ver, ai, mi = 0;
    char board[64];
    bool ok = fscanf(f, "version=%u board=%63s axp_current=%u", &ver, board, &ai) == 3 &&
              ver == BASELINE_VERSION && strcmp(board, power_board()->name) == 0 && ai <= AXP_CURRENT_MASK;
    if (ok && has_mp2731()) ok = fscanf(f, " mp_current=%u", &mi) == 1 && mi <= MP_CURRENT_MASK;
    fclose(f);
    if (!ok) return;
    if (ai > AXP_CURRENT_CAP) { baseline.axp.valid = true; baseline.axp.current = (uint8_t)ai; }
    if (mi > MP_CURRENT_CAP) { baseline.mp.valid = true; baseline.mp.current = (uint8_t)mi; }
}

static bool save_baseline(void) {
    const char *final_path = baseline_path();
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", final_path, (long)getpid());
    FILE *f = fopen(tmp, "we"); if (!f) return false;
    fprintf(f, "version=%u board=%s axp_current=%u", BASELINE_VERSION, power_board()->name,
            baseline.axp.valid ? baseline.axp.current : 0u);
    if (has_mp2731()) fprintf(f, " mp_current=%u", baseline.mp.valid ? baseline.mp.current : 0u);
    fputc('\n', f);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) { unlink(tmp); return false; }
    if (rename(tmp, final_path) != 0) { unlink(tmp); return false; }
    char path[512];
    snprintf(path, sizeof(path), "%s", final_path);
    char *slash = strrchr(path, '/');
    if (slash) { *slash = '\0'; int fd = open(path, O_RDONLY | O_DIRECTORY); if (fd >= 0) { fsync(fd); close(fd); } }
    return true;
}

/* Captures each chip's pre-cap current, once: only a value above the cap
 * counts (at or below it there is nothing to restore, or it is already
 * capped). A new capture is saved before any cap is applied; when that
 * fails, nothing is capped this time and the capture is tried again. */
static bool capture_baseline(void) {
    if (!baseline.loaded) { load_baseline(); baseline.loaded = true; }
    chip_baseline_t axp = baseline.axp, mp = baseline.mp;
    uint8_t v;
    if (!axp.valid) {
        if (!axp_read(AXP_REG_CURRENT, &v)) return false;
        if ((v & AXP_CURRENT_MASK) > AXP_CURRENT_CAP) { axp.valid = true; axp.current = v & AXP_CURRENT_MASK; }
    }
    if (has_mp2731() && !mp.valid) {
        if (!mp_read(MP_REG_CURRENT, &v)) return false;
        if ((v & MP_CURRENT_MASK) > MP_CURRENT_CAP) { mp.valid = true; mp.current = v & MP_CURRENT_MASK; }
    }
    if (axp.valid == baseline.axp.valid && mp.valid == baseline.mp.valid) return true;
    chip_baseline_t old_axp = baseline.axp, old_mp = baseline.mp;
    baseline.axp = axp; baseline.mp = mp;
    if (!save_baseline()) { baseline.axp = old_axp; baseline.mp = old_mp; return false; }
    return true;
}

static bool set_axp_voltage(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_VOLTAGE, &v)) return false;
    if ((v & 7u) == target) return true;
    if (!axp_write(AXP_REG_VOLTAGE, target) || !axp_read(AXP_REG_VOLTAGE, &v)) return false;
    return (v & 7u) == target;
}
/* "Cap" never raises the current beyond target -- if some other setting
 * already left it lower than our cap, leave it alone rather than bumping
 * it up. "Exact" (restore) always converges to target regardless of
 * direction -- a captured baseline that was higher than the cap must be
 * writable back UP to that value, not just left at the still-capped one. */
static bool set_axp_current_capped(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_CURRENT, &v)) return false;
    if ((v & AXP_CURRENT_MASK) <= target) return true;
    uint8_t desired = (v & (uint8_t)~AXP_CURRENT_MASK) | target;
    if (!axp_write(AXP_REG_CURRENT, desired) || !axp_read(AXP_REG_CURRENT, &v)) return false;
    return (v & AXP_CURRENT_MASK) == target;
}
static bool set_axp_current_exact(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_CURRENT, &v)) return false;
    if ((v & AXP_CURRENT_MASK) == target) return true;
    uint8_t desired = (v & (uint8_t)~AXP_CURRENT_MASK) | target;
    if (!axp_write(AXP_REG_CURRENT, desired) || !axp_read(AXP_REG_CURRENT, &v)) return false;
    return (v & AXP_CURRENT_MASK) == target;
}
static bool enable_axp_charger(void) {
    uint8_t v; if (!axp_read(AXP_REG_MODULE_EN, &v)) return false;
    if (v & AXP_CHG_EN) return true;
    v |= AXP_CHG_EN;
    if (!axp_write(AXP_REG_MODULE_EN, v) || !axp_read(AXP_REG_MODULE_EN, &v)) return false;
    return (v & AXP_CHG_EN) != 0;
}
static bool set_mp_voltage(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_VOLTAGE, &v)) return false;
    uint8_t desired = (v & (uint8_t)~MP_VOLTAGE_MASK) | target;
    if ((v & MP_VOLTAGE_MASK) == target) return true;
    if (!mp_write(MP_REG_VOLTAGE, desired) || !mp_read(MP_REG_VOLTAGE, &v)) return false;
    return (v & MP_VOLTAGE_MASK) == target;
}
static bool set_mp_current_capped(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_CURRENT, &v)) return false;
    if ((v & MP_CURRENT_MASK) <= target) return true;
    uint8_t desired = (v & (uint8_t)~MP_CURRENT_MASK) | target;
    if (!mp_write(MP_REG_CURRENT, desired) || !mp_read(MP_REG_CURRENT, &v)) return false;
    return (v & MP_CURRENT_MASK) == target;
}
static bool set_mp_current_exact(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_CURRENT, &v)) return false;
    if ((v & MP_CURRENT_MASK) == target) return true;
    uint8_t desired = (v & (uint8_t)~MP_CURRENT_MASK) | target;
    if (!mp_write(MP_REG_CURRENT, desired) || !mp_read(MP_REG_CURRENT, &v)) return false;
    return (v & MP_CURRENT_MASK) == target;
}

static bool disable_axp_charger(void) {
    uint8_t v; if (!axp_read(AXP_REG_MODULE_EN, &v)) return false;
    if (!(v & AXP_CHG_EN)) return true;
    v &= (uint8_t)~AXP_CHG_EN;
    if (!axp_write(AXP_REG_MODULE_EN, v) || !axp_read(AXP_REG_MODULE_EN, &v)) return false;
    return (v & AXP_CHG_EN) == 0;
}

/* --- charger ops --------------------------------------------------------- */

static unsigned axp_supported(void) {
    if (!has_axp2101()) return 0;
    unsigned ops = CHARGER_OP_CHARGE_VOLTAGE | CHARGER_OP_CURRENT_CAP | CHARGER_OP_CHARGE_DONE;
    if (!has_mp2731()) ops |= CHARGER_OP_SET_CHARGING;
    return ops;
}

/* REG64H[2:0]: 1 = 4.0 V, 2 = 4.1 V, 3 = 4.2 V, 4 = 4.35 V, 5 = 4.4 V. The
 * MP2731's encoding is only known for 4.2 V (0xA0) and its stock 4.4 V. */
static bool voltage_codes(int millivolts, uint8_t *axp, uint8_t *mp) {
    if (millivolts == CHARGER_VOLTAGE_STOCK) {
        *axp = power_board()->power.axp_voltage_baseline;
        *mp = MP_VOLTAGE_BASELINE;
        return true;
    }
    static const struct { int mv; uint8_t axp; } axp_codes[] = {
        { 4000, 1 }, { 4100, 2 }, { 4200, 3 }, { 4350, 4 }, { 4400, 5 },
    };
    for (size_t i = 0; i < sizeof(axp_codes) / sizeof(axp_codes[0]); i++) {
        if (axp_codes[i].mv != millivolts) continue;
        *axp = axp_codes[i].axp;
        if (has_mp2731()) {
            if (millivolts == 4200) *mp = 0xA0u; /* MP2731's matching 4.2V encoding */
            else if (millivolts == 4400) *mp = MP_VOLTAGE_BASELINE;
            else return false;
        }
        return true;
    }
    return false;
}

/* The AXP2101 target, the MP2731's, then make sure the charger is enabled
 * (unless charging was switched off on purpose). */
static bool axp_set_charge_voltage(int millivolts) {
    if (!has_axp2101()) return false;
    uint8_t axp_code = 0, mp_code = 0;
    if (!voltage_codes(millivolts, &axp_code, &mp_code)) return false;
    bool ok = set_axp_voltage(axp_code);
    if (has_mp2731()) ok = set_mp_voltage(mp_code) && ok;
    if (ok && charging_enabled) ok = enable_axp_charger();
    return ok;
}

static bool axp_set_charging(bool enable) {
    if (!(axp_supported() & CHARGER_OP_SET_CHARGING)) return false;
    charging_enabled = enable;
    return enable ? enable_axp_charger() : disable_axp_charger();
}

/* After charge_control.c's throttle: capture the baseline, then cap or
 * restore. */
static bool axp_set_current_cap(bool on) {
    if (!has_axp2101()) return false;
    if (!capture_baseline()) return false;
    bool ok = true;
    if (on) {
        ok = set_axp_current_capped(AXP_CURRENT_CAP);
        if (has_mp2731()) ok = set_mp_current_capped(MP_CURRENT_CAP) && ok;
    } else {
        /* Without a baseline there is nothing known to go back to: left as
         * it is (capped at worst, the safe side). */
        if (baseline.axp.valid) ok = set_axp_current_exact(baseline.axp.current) && ok;
        if (has_mp2731() && baseline.mp.valid) ok = set_mp_current_exact(baseline.mp.current) && ok;
    }
    return ok;
}

static bool axp_charge_done(void) {
    if (!has_axp2101()) return false;
    if (has_mp2731()) {
        /* On R3 Pro II, the MP2731 -- not the AXP2101 -- is the chip actually
         * driving the charge cycle (see battery.c's own comment on why its
         * "battery" status node is unreliable there for the same reason).
         * battery_is_full() already reads the MP2731-corrected status node, so
         * it is the primary, not a fallback: trusting AXP2101's own REG01 here
         * first risks a false "done" if the AXP2101's charge state machine
         * settles independently of what the MP2731 is actually doing. */
        return battery_is_full();
    }
    uint8_t status;
    return axp_read(AXP_REG_STATUS, &status) && (status & 7u) == 4;
}

const struct board_charger_ops charger_axp2101_i2c_ops = {
    .name = "axp2101-i2c",
    .supported = axp_supported,
    .set_charging = axp_set_charging,
    .set_input_current = NULL, /* no input-current policy on these boards */
    .set_charge_voltage = axp_set_charge_voltage,
    .set_current_cap = axp_set_current_cap,
    .charge_done = axp_charge_done,
};

void charger_axp2101_test_hooks(charger_i2c_xfer_fn xfer_fn, const char *path) {
    test_xfer = xfer_fn;
    test_baseline_path = path;
    memset(&baseline, 0, sizeof(baseline));
    charging_enabled = true;
}
