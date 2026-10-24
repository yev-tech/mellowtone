/* Charger back-end selection and the USB input-current policy. */
#include "charger.h"
#include "power_events.h"
#include "power_sysfs.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const struct board_charger_ops * charger_ops(void) { return power_board()->charger; }

unsigned charger_supported(void) {
    const struct board_charger_ops * ops = charger_ops();
    return ops && ops->supported ? ops->supported() : 0;
}

const char * charger_input_name(charger_input_t input) {
    switch (input) {
        case CHARGER_INPUT_HOST: return "host";
        case CHARGER_INPUT_CHARGER: return "charger";
        case CHARGER_INPUT_HOST_NO_CHARGE: return "host (no charge)";
        case CHARGER_INPUT_NONE: break;
    }
    return "none";
}

/* Reads the inputs of the decision: gadget state ("" if unreadable) and
 * usb_node/online (-1 if unreadable), as the stock R3 Pro player does. */
static charger_input_t detect(char * state, size_t state_size, int * online) {
    const board_desc_t * board = power_board();
    const board_input_current_policy_t * policy = board->power.input_current;
    state[0] = '\0';
    *online = -1;
    if (policy && policy->gadget_state && !power_sysfs_read(policy->gadget_state, state, state_size))
        state[0] = '\0';
    if (!power_supply_read_int(board->power.usb_node, "online", online)) *online = -1;
    bool data_link = strcmp(state, "CONFIGURED") == 0 || strcmp(state, "CONNECTED") == 0;
    if (data_link) return CHARGER_INPUT_HOST;
    if (*online == 1) return CHARGER_INPUT_CHARGER;
    return CHARGER_INPUT_NONE;
}

charger_input_t charger_detect_input(void) {
    char state[32];
    int online;
    return detect(state, sizeof(state), &online);
}

static pthread_mutex_t policy_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_logged[48];
static atomic_bool input_inhibit;

bool charger_input_inhibit_supported(void) {
    const board_desc_t * board = power_board();
    const board_input_current_policy_t * policy = board->power.input_current;
    const struct board_charger_ops * ops = charger_ops();
    return (board->caps & CAP_INPUT_CURRENT) && policy && policy->has_dac_no_charge && ops &&
           ops->set_input_current;
}

bool charger_input_current_apply(void) {
    const struct board_charger_ops * ops = charger_ops();
    if (!ops || !ops->set_input_current) return false;
    pthread_mutex_lock(&policy_lock);
    char state[32];
    int online;
    charger_input_t input = detect(state, sizeof(state), &online);
    if (input == CHARGER_INPUT_HOST && atomic_load(&input_inhibit) && charger_input_inhibit_supported())
        input = CHARGER_INPUT_HOST_NO_CHARGE;
    bool ok = input == CHARGER_INPUT_NONE || ops->set_input_current(input);

    char now[48];
    snprintf(now, sizeof(now), "%.30s/%d%s", state, online, input == CHARGER_INPUT_HOST_NO_CHARGE ? "/i" : "");
    if (strcmp(now, last_logged) != 0) {
        fprintf(stderr, "usb_power: state=%s online=%d -> %s\n", state, online,
                input == CHARGER_INPUT_HOST              ? "host policy"
                : input == CHARGER_INPUT_HOST_NO_CHARGE ? "host policy, no charging (USB DAC)"
                : input == CHARGER_INPUT_CHARGER         ? "charger policy"
                                                         : "unplugged");
        snprintf(last_logged, sizeof(last_logged), "%s", now);
    }
    pthread_mutex_unlock(&policy_lock);
    return ok;
}

/* How long a plug/unplug is followed by 2 s re-checks: the stock player
 * re-applied every 2 s forever; the window keeps that for the moments a
 * driver can still be settling (charger type detection after VBUS, gadget
 * enumeration) without waking the CPU every 2 s for the rest of the day. */
#define INPUT_POLICY_SETTLE_MS 10000

static void on_power_event(unsigned reasons, void * ctx) {
    (void) ctx;
    if (reasons & (POWER_EVT_UEVENT | POWER_EVT_SYS_SERVER | POWER_EVT_START | POWER_EVT_RESUME))
        power_events_fast_window(INPUT_POLICY_SETTLE_MS);
    charger_input_current_apply();
}

void charger_input_current_set_inhibit(bool inhibit) {
    if (!charger_input_inhibit_supported()) return;
    if (atomic_exchange(&input_inhibit, inhibit) != inhibit) charger_input_current_apply();
}

void charger_input_current_start(void) {
    const board_desc_t * board = power_board();
    const struct board_charger_ops * ops = charger_ops();
    if (!(board->caps & CAP_INPUT_CURRENT) || !board->power.input_current || !ops || !ops->set_input_current)
        return;
    /* Called once at start-up (main.c). */
    if (!power_events_subscribe(on_power_event, NULL))
        fprintf(stderr, "usb_power: cannot subscribe to power events\n");
}
