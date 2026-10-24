#ifndef USB_STORAGE_LEND_H
#define USB_STORAGE_LEND_H

/* Lending the SD card to a computer (USB storage on boards whose
 * .storage_export has lends_card):
 *
 *   IDLE        the card is ours. A computer configures the port while the
 *               USB mode is Storage (ADB off, no DAC) and the cable is in:
 *   CLOSING     UI thread, hooks.close_all(): save the queue (with the
 *               resume point) and settings, stop playback and every worker
 *               on the card, close the library database, queue and file
 *               browser, mt_state_set_card_lent(true) so every writer pauses
 *               and stdout/stderr go to RAM, close the logs; only then check
 *               nothing (in any process) still has a file on the card, so
 *               the check can only find other processes (named to the
 *               user). May take a few polls (a running scan, the playback
 *               thread or a queue write has to stop first).
 *   EXPORTING   worker: storage_export->export_card() (unmount, SETLUN,
 *               mass-storage function)
 *   LENT        "Connected as USB storage" (hooks.show). Ends on the cable
 *               pulled (1 s debounce), the computer ejecting the card, or
 *               the screen's Disconnect button:
 *   RECLAIMING  worker: storage_export->reclaim_card() (idle function,
 *               SETLUN empty, mount), retried twice
 *   REOPENING   UI thread, hooks.reopen_all(): mt_state_set_card_lent(false),
 *               reload the library and queue, offer a rescan when the
 *               computer may have changed files; then IDLE.
 *
 * After an eject, the Disconnect button or a failure the card is not lent
 * again until the cable is pulled (the stock player's "soft remove"), or the
 * computer, still configured, would get it straight back.
 *
 * usb_lend_step() is the pure state machine (tests drive it directly); the
 * rest runs it from the UI poll with the board's back ends. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    USB_LEND_IDLE = 0,
    USB_LEND_CLOSING,
    USB_LEND_EXPORTING,
    USB_LEND_LENT,
    USB_LEND_RECLAIMING,
    USB_LEND_REOPENING,
} usb_lend_state_t;

typedef enum {
    USB_LEND_END_NONE = 0,
    USB_LEND_END_UNPLUG,  /* the cable was pulled */
    USB_LEND_END_EJECT,   /* the computer ejected the card */
    USB_LEND_END_USER,    /* Disconnect on the device */
    USB_LEND_END_FAILED,  /* closing or exporting failed: the computer never had the card */
} usb_lend_end_t;

typedef enum {
    USB_LEND_PENDING = 0, /* still running / not yet */
    USB_LEND_OK,
    USB_LEND_FAILED,
} usb_lend_result_t;

typedef struct {
    bool cable;            /* USB power present */
    bool host;             /* a computer configured the port */
    bool want;             /* the user's USB mode is Storage and nothing else is switching */
    bool ejected;          /* LENT: the LUN is empty again */
    bool user_disconnect;  /* LENT: the Disconnect button */
    usb_lend_result_t close;  /* CLOSING: hooks.close_all() */
    usb_lend_result_t work;   /* EXPORTING / RECLAIMING: the worker */
    bool reopened;            /* REOPENING: hooks.reopen_all() returned */
} usb_lend_inputs_t;

typedef enum {
    USB_LEND_DO_NOTHING = 0,
    USB_LEND_DO_CLOSE,   /* (again) hooks.close_all() */
    USB_LEND_DO_EXPORT,  /* start the export worker */
    USB_LEND_DO_SHOW,    /* the card is lent: show the storage screen */
    USB_LEND_DO_RECLAIM, /* start the reclaim worker */
    USB_LEND_DO_REOPEN,  /* hooks.reopen_all(end) */
} usb_lend_action_t;

#define USB_LEND_UNPLUG_DEBOUNCE_MS 1000
#define USB_LEND_RECLAIM_TRIES 3

typedef struct {
    usb_lend_state_t state;
    usb_lend_end_t end;   /* why the current/last lend ended */
    bool declined;        /* no new lend until the cable is pulled */
    int reclaim_tries;
    bool unplug_seen;
    uint32_t unplug_since_ms;
} usb_lend_t;

/* already_lent: the player started while the card was lent (it restarted
 * during a session): resume in LENT. */
void usb_lend_init(usb_lend_t * l, bool already_lent);
usb_lend_action_t usb_lend_step(usb_lend_t * l, const usb_lend_inputs_t * in, uint32_t now_ms);
const char * usb_lend_state_name(usb_lend_state_t state);

/* ---- running it (UI thread) --------------------------------------------- */

typedef struct {
    /* Close everything on the card; USB_LEND_PENDING to be called again on
     * the next poll. On USB_LEND_FAILED, reopen_all() follows. */
    usb_lend_result_t (*close_all)(void);
    /* Reopen what close_all() closed; `end` says whether the computer had
     * the card (UNPLUG/EJECT/USER: files may have changed). */
    void (*reopen_all)(usb_lend_end_t end);
    /* The storage screen on (lent) or off. */
    void (*show)(bool on);
} usb_lend_hooks_t;

/* Boards without a lending back end ignore all of this. */
bool usb_storage_lend_supported(void);
void usb_storage_lend_set_hooks(const usb_lend_hooks_t * hooks);
/* Once at start-up, after the hooks: resumes a lend in progress. */
void usb_storage_lend_start(void);
/* Every UI poll. want: as usb_lend_inputs_t. */
void usb_storage_lend_poll(bool want, uint32_t now_ms);
/* Not IDLE: the card is (being) lent or coming back; the USB mode must not
 * change meanwhile. */
bool usb_storage_lend_busy(void);
usb_lend_state_t usb_storage_lend_state(void);
/* The storage screen's Disconnect button. */
void usb_storage_lend_user_disconnect(void);

/* Close-time check: processes with files open on the card root. Logs them
 * (for this process also each file it still holds: close_all() missed it);
 * true when there are none. other (may be NULL): the name of the first
 * other process holding the card, "" when there is none. */
bool usb_storage_lend_card_free(const char * card_root, char * other, size_t other_size);

#endif /* USB_STORAGE_LEND_H */
