#ifndef POWER_EVENTS_H
#define POWER_EVENTS_H

/* Power change notification: one worker thread that wakes the
 * battery reader, the input-current policy and the charge control when
 * something may have changed, instead of each of them polling sysfs.
 *
 * Sources:
 *   - kernel uevents (NETLINK_KOBJECT_UEVENT) for SUBSYSTEM=power_supply on
 *     every board (plug/unplug, status, and capacity where the gauge driver
 *     calls power_supply_changed()), and SUBSYSTEM=android_usb (the legacy
 *     gadget's USB_STATE=CONNECTED/CONFIGURED/DISCONNECTED, R3 Pro);
 *   - power_events_notify(): the R3 Pro's CHARGE:* messages on our sys_client
 *     socket (early_mount forwards its own power_supply uevents there as
 *     CHARGE:USB_INSTD / USB_RMVD / AC_INSTD / AC_RMVD), suspend/resume;
 *   - a slow fallback poll (60 s): not every gauge emits a uevent when the
 *     capacity drifts by a percent, and a uevent can be lost (ENOBUFS).
 *
 * Bursts are coalesced: subscribers run once per ~100 ms window, on the
 * worker thread, with the OR of the reasons. They must be thread-safe and
 * quick (flag work for later, or a few sysfs accesses). */

#include <stdbool.h>
#include <stddef.h>

enum {
    POWER_EVT_UEVENT = 1u << 0,     /* a relevant kernel uevent (or lost uevents) */
    POWER_EVT_SYS_SERVER = 1u << 1, /* CHARGE:* from the R3 Pro's sys_client socket */
    POWER_EVT_POLL = 1u << 2,       /* slow fallback poll, or the fast window after a change */
    POWER_EVT_RESUME = 1u << 3,     /* back from suspend */
    POWER_EVT_START = 1u << 4,      /* first run after start, and after a subscribe */
};

typedef void (*power_event_cb)(unsigned reasons, void * ctx);

typedef struct {
    /* Datagram source of uevents. -1: open the kernel's NETLINK_KOBJECT_UEVENT
     * group. Tests pass one end of a socketpair and write fake uevents
     * ("change@/devpath\0KEY=value\0...") to the other. The worker owns it. */
    int uevent_fd;
    int poll_interval_ms;     /* 0: 60000 */
    int fast_interval_ms;     /* poll interval inside a fast window; 0: 2000 */
    int coalesce_ms;          /* 0: 100 */
} power_events_config_t;

/* NULL: defaults. False if the worker could not start (callers keep their
 * own fallbacks: the battery cache then expires on its old 5 s TTL). */
bool power_events_start(const power_events_config_t * config);
void power_events_stop(void);
bool power_events_running(void);

/* Up to 8 subscribers. Each is called once with POWER_EVT_START soon after
 * subscribing (when the worker runs). */
bool power_events_subscribe(power_event_cb cb, void * ctx);

/* From any thread: something power-related happened. */
void power_events_notify(unsigned reasons);

/* Poll every fast_interval_ms for the next window_ms (on top of events):
 * settings a driver may still change shortly after a plug event are
 * re-checked quickly, then the slow poll takes over again. */
void power_events_fast_window(int window_ms);

/* Bumped once per dispatch. */
unsigned power_events_generation(void);

/* Uevent parsing (exposed for tests). */
typedef struct {
    char action[16];
    char subsystem[32];
    char name[64];      /* POWER_SUPPLY_NAME, or "" */
    char usb_state[24]; /* USB_STATE (android_usb), or "" */
} power_uevent_t;

/* Parses one netlink uevent datagram. False for anything that is not a
 * kernel "action@devpath" message (e.g. udev's "libudev" ones). */
bool power_uevent_parse(const char * buf, size_t len, power_uevent_t * out);
/* power_supply or android_usb. */
bool power_uevent_relevant(const power_uevent_t * ev);

/* sys_client message hook: true (and notifies) for CHARGE:* / USB:INSERT /
 * USB:REMOVE. */
bool power_events_sys_message(const char * msg);

#endif /* POWER_EVENTS_H */
