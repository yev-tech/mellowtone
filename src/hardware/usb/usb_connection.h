#ifndef USB_CONNECTION_H
#define USB_CONNECTION_H

/* The USB mode of one connection.
 *
 * Charge only is the mode at start-up and after every unplug: the computer
 * gets nothing (with ADB on, ADB keeps the port). Storage or USB DAC is
 * chosen by the user once a computer is on the cable -- from the "Connected
 * to a computer" prompt or Settings > System > USB Mode -- and lasts for
 * that connection only: it is never saved, and pulling the cable takes the
 * port back to charge only. ADB, a Developer Options setting, survives both.
 *
 * A computer is told from a wall charger by the port being configured
 * (the gadget's host_connected(), usb_gadget.h), which needs a gadget the
 * computer can configure: the R3 Pro keeps its idle set-up enabled; the
 * X1600 boards bind a charge-only gadget whose only function is a mass
 * storage LUN with no medium. A charger never configures either.
 *
 * usb_conn_step() is the pure state machine (tests drive it directly); the
 * UI runs it every poll and applies the modes (gui_usb.c). */

#include "usb_mode_control.h"

#include <stdbool.h>
#include <stdint.h>

/* The mode the port is verifiably in (set once a switch lands or the live
 * mode is read back). Runtime only: USB_MODE_CHARGE at start-up. */
usb_mode_t usb_mode_active(void);
void usb_mode_set_active(usb_mode_t mode);
const char * usb_mode_name(usb_mode_t mode);

/* A cable glitch shorter than this is not an unplug: the chosen mode and
 * the prompt state stay. */
#define USB_CONN_UNPLUG_DEBOUNCE_MS 1000

typedef struct {
    bool cable_known;    /* the power state could be read (unknown is not an unplug) */
    bool cable;          /* USB power present */
    bool host;           /* a computer has the port configured */
    bool adb;            /* ADB owns the port */
    bool modes_offered;  /* the board offers Storage or USB DAC */
    usb_mode_t mode;     /* usb_mode_active() */
} usb_conn_inputs_t;

typedef enum {
    USB_CONN_NOTHING = 0,
    USB_CONN_PROMPT,    /* a computer is on the cable, still charge only: offer the modes (once) */
    USB_CONN_UNPLUGGED, /* the cable is out in Storage or USB DAC: back to charge only
                           (again every poll until usb_conn_revert_started()) */
} usb_conn_event_t;

typedef struct {
    bool cable;           /* debounced */
    bool computer;        /* a computer configured the port on this connection */
    bool prompted;        /* the prompt was offered, or is not to be, on this connection */
    bool revert_started;  /* the switch back to charge only is under way */
    bool unplug_seen;
    uint32_t unplug_since_ms;
} usb_conn_t;

void usb_conn_init(usb_conn_t * c);
usb_conn_event_t usb_conn_step(usb_conn_t * c, const usb_conn_inputs_t * in, uint32_t now_ms);
/* The switch to charge only that USB_CONN_UNPLUGGED asked for was accepted. */
void usb_conn_revert_started(usb_conn_t * c);
/* A computer is on the cable now: the modes may be chosen. */
bool usb_conn_computer(const usb_conn_t * c);
/* The cable is in (debounced). */
bool usb_conn_cable(const usb_conn_t * c);

#endif /* USB_CONNECTION_H */
