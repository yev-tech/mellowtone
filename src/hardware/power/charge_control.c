/* Charge control (CAP_CHARGE_CONTROL): see charge_control.h. */
#include "charge_control.h"
#include "charger.h"
#include "power_events.h"
#include "power_sysfs.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHARGE_CONTROL_REEVALUATE_SECONDS 5
#define CHARGE_RETRY_SECONDS 1

typedef struct {
    struct timespec next_run;
    struct timespec retry_deadline;
} throttle_t;

static throttle_t limit_throttle, current_throttle;
static bool voltage_limited;   /* VOLTAGE_CAP: the cap was applied */
static charge_hysteresis_t hysteresis;
static bool charging_off;      /* STOP_RESUME: set_charging(false) is in effect */
static bool last_limit_enabled;
static bool requested_inhibit; /* "Don't charge in DAC mode" wants charging off */
static bool applied_inhibit;   /* ... and it is in effect */

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static bool events_dirty;
static bool events_subscribed;

static board_charge_method_t method(void) { return power_board()->power.charge_method; }

static bool control_active(void) {
    const board_desc_t * board = power_board();
    return (board->caps & CAP_CHARGE_CONTROL) && board->charger && method() != BOARD_CHARGE_METHOD_NONE;
}

static bool before(const struct timespec * a, const struct timespec * b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}
static struct timespec plus_seconds(struct timespec t, int n) {
    t.tv_sec += n;
    return t;
}
static void retry_at(struct timespec * deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    *deadline = plus_seconds(now, CHARGE_RETRY_SECONDS);
}

/* The throttle: at most every 5 s, sooner while a failed attempt waits for
 * its 1 s retry. True when this call should run. */
static bool throttle_due(throttle_t * t, bool force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && t->next_run.tv_sec && before(&now, &t->next_run) &&
        (t->retry_deadline.tv_sec == 0 || before(&now, &t->retry_deadline)))
        return false;
    t->next_run = plus_seconds(now, CHARGE_CONTROL_REEVALUATE_SECONDS);
    return true;
}
static void throttle_result(throttle_t * t, bool ok) {
    if (ok) t->retry_deadline.tv_sec = 0;
    else retry_at(&t->retry_deadline);
}

bool charge_hysteresis_step(charge_hysteresis_t * h, int capacity, battery_external_power_state_t power,
                            int limit, int resume) {
    if (power == BATTERY_EXTERNAL_POWER_DISCONNECTED) {
        h->stopped = false;
        return true;
    }
    if (capacity < 0) return !h->stopped;
    if (!h->stopped && capacity >= limit) h->stopped = true;
    else if (h->stopped && capacity <= resume) h->stopped = false;
    return !h->stopped;
}

/* Charging switched off by the charger (CAP_CHARGE_CONTROL), or -- the
 * stock R3 Pro's way -- a host row of the input-current policy that draws no
 * charge current (CAP_INPUT_CURRENT, policy.dac_no_charge). */
static bool switch_inhibit_supported(void) {
    return control_active() && (charger_supported() & CHARGER_OP_SET_CHARGING);
}

bool charge_control_inhibit_supported(void) {
    return switch_inhibit_supported() || charger_input_inhibit_supported();
}

bool charge_control_safe_current_supported(void) {
    return control_active() && (charger_supported() & CHARGER_OP_CURRENT_CAP);
}

void charge_control_set_inhibit(bool inhibit) {
    requested_inhibit = inhibit;
    charger_input_current_set_inhibit(inhibit); /* no-op without the policy row */
}

/* VOLTAGE_CAP: switch charging off for the inhibit (and back on), only
 * while it is wanted or still in effect -- never otherwise, so boards
 * without the DAC setting see only the voltage cap's register traffic. */
static bool apply_inhibit(const struct board_charger_ops * ops) {
    bool want = requested_inhibit && switch_inhibit_supported();
    if (!want && !applied_inhibit) return true;
    bool ok = ops->set_charging(!want);
    if (ok) applied_inhibit = want;
    return ok;
}

static void voltage_cap_poll(const struct board_charger_ops * ops, bool enabled, bool force) {
    bool want_inhibit = requested_inhibit && switch_inhibit_supported();
    if (want_inhibit != applied_inhibit) force = true;
    if (!throttle_due(&limit_throttle, force)) return;
    bool ok = ops->set_charge_voltage(enabled ? CHARGE_LIMIT_MILLIVOLTS : CHARGER_VOLTAGE_STOCK);
    voltage_limited = enabled && ok;
    ok = apply_inhibit(ops) && ok;
    throttle_result(&limit_throttle, ok);
}

static void stop_resume_poll(const struct board_charger_ops * ops, bool enabled, bool force) {
    pthread_mutex_lock(&event_lock);
    bool dirty = events_dirty;
    events_dirty = false;
    bool subscribed = events_subscribed;
    pthread_mutex_unlock(&event_lock);
    /* Without power events, fall back to the 5 s re-evaluation. */
    if (!dirty && !subscribed) dirty = throttle_due(&limit_throttle, false);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool retry_due = limit_throttle.retry_deadline.tv_sec && !before(&now, &limit_throttle.retry_deadline);
    bool want_inhibit = requested_inhibit && switch_inhibit_supported();
    if (!force && !dirty && !retry_due && want_inhibit == applied_inhibit && enabled == last_limit_enabled) return;
    last_limit_enabled = enabled;

    bool allow = true;
    if (enabled)
        allow = charge_hysteresis_step(&hysteresis, battery_get_percent(), battery_get_external_power_state(),
                                       CHARGE_LIMIT_PERCENT, CHARGE_RESUME_PERCENT);
    else
        hysteresis.stopped = false;
    bool want = allow && !want_inhibit;
    bool ok = ops->set_charging(want);
    if (ok) {
        if (charging_off != !want)
            fprintf(stderr, "charge_control: charging %s (limit %s, inhibit %d)\n", want ? "resumed" : "stopped",
                    enabled ? "on" : "off", want_inhibit);
        charging_off = !want;
        applied_inhibit = want_inhibit;
    }
    throttle_result(&limit_throttle, ok);
}

void charge_control_poll(bool limit_enabled, bool force) {
    if (!control_active()) return;
    const struct board_charger_ops * ops = power_board()->charger;
    if (method() == BOARD_CHARGE_METHOD_VOLTAGE_CAP) {
        if (ops->set_charge_voltage) voltage_cap_poll(ops, limit_enabled, force);
    } else if (method() == BOARD_CHARGE_METHOD_STOP_RESUME) {
        if (ops->set_charging && (charger_supported() & CHARGER_OP_SET_CHARGING))
            stop_resume_poll(ops, limit_enabled, force);
    }
}

void charge_control_safe_current_poll(bool enabled, bool force) {
    if (!charge_control_safe_current_supported()) return;
    if (!throttle_due(&current_throttle, force)) return;
    bool ok = power_board()->charger->set_current_cap(enabled);
    throttle_result(&current_throttle, ok);
}

bool charge_control_is_holding(void) {
    if (method() == BOARD_CHARGE_METHOD_STOP_RESUME) return last_limit_enabled && hysteresis.stopped;
    return voltage_limited;
}

bool charge_control_is_confirmed_off(void) {
    if (!control_active()) return false;
    if (method() == BOARD_CHARGE_METHOD_STOP_RESUME)
        return last_limit_enabled && hysteresis.stopped && charging_off;
    if (!voltage_limited) return false;
    const struct board_charger_ops * ops = power_board()->charger;
    return ops->charge_done && ops->charge_done();
}

static void on_power_event(unsigned reasons, void * ctx) {
    (void) reasons;
    (void) ctx;
    pthread_mutex_lock(&event_lock);
    events_dirty = true;
    pthread_mutex_unlock(&event_lock);
}

void charge_control_init(void) {
    if (!control_active() || method() != BOARD_CHARGE_METHOD_STOP_RESUME) return;
    bool ok = power_events_subscribe(on_power_event, NULL);
    pthread_mutex_lock(&event_lock);
    events_subscribed = ok && power_events_running();
    pthread_mutex_unlock(&event_lock);
    if (!ok) fprintf(stderr, "charge_control: cannot subscribe to power events; re-checking every 5 s\n");
}

void charge_control_test_reset(void) {
    memset(&limit_throttle, 0, sizeof(limit_throttle));
    memset(&current_throttle, 0, sizeof(current_throttle));
    voltage_limited = false;
    hysteresis.stopped = false;
    charging_off = false;
    last_limit_enabled = false;
    requested_inhibit = false;
    applied_inhibit = false;
    pthread_mutex_lock(&event_lock);
    events_dirty = false;
    events_subscribed = false;
    pthread_mutex_unlock(&event_lock);
}
