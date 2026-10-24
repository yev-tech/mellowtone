/* The USB mode of one connection: see usb_connection.h. */
#include "usb_connection.h"

#include <stdatomic.h>

static atomic_int active_mode = USB_MODE_CHARGE;

usb_mode_t usb_mode_active(void) { return (usb_mode_t) atomic_load(&active_mode); }
void usb_mode_set_active(usb_mode_t mode) { atomic_store(&active_mode, (int) mode); }

const char * usb_mode_name(usb_mode_t mode) {
    switch (mode) {
        case USB_MODE_CHARGE: return "charge only";
        case USB_MODE_STORAGE: return "storage";
        case USB_MODE_DAC: return "USB DAC";
        case USB_MODE_ADB: return "ADB";
    }
    return "?";
}

void usb_conn_init(usb_conn_t * c) { *c = (usb_conn_t) { 0 }; }

usb_conn_event_t usb_conn_step(usb_conn_t * c, const usb_conn_inputs_t * in, uint32_t now_ms) {
    if (!in->cable_known) return USB_CONN_NOTHING;
    if (!in->cable) {
        if (!c->unplug_seen) {
            c->unplug_seen = true;
            c->unplug_since_ms = now_ms;
        }
        if (now_ms - c->unplug_since_ms < USB_CONN_UNPLUG_DEBOUNCE_MS) return USB_CONN_NOTHING;
        /* The connection is over: the next one starts from scratch. */
        c->cable = false;
        c->computer = false;
        c->prompted = false;
        if ((in->mode == USB_MODE_STORAGE || in->mode == USB_MODE_DAC) && !c->revert_started)
            return USB_CONN_UNPLUGGED;
        return USB_CONN_NOTHING;
    }
    c->unplug_seen = false;
    c->revert_started = false;
    c->cable = true;
    if (in->host && !c->computer) {
        c->computer = true;
        /* Nothing to ask while ADB has the port, when a mode was already
         * chosen, or when the board has nothing but charging to offer. */
        if (in->adb || in->mode != USB_MODE_CHARGE || !in->modes_offered) c->prompted = true;
    }
    if (c->computer && !c->prompted && !in->adb && in->mode == USB_MODE_CHARGE) {
        c->prompted = true;
        return USB_CONN_PROMPT;
    }
    return USB_CONN_NOTHING;
}

void usb_conn_revert_started(usb_conn_t * c) { c->revert_started = true; }
bool usb_conn_computer(const usb_conn_t * c) { return c->cable && c->computer; }
bool usb_conn_cable(const usb_conn_t * c) { return c->cable; }
