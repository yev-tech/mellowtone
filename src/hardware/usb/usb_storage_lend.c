/* Lending the SD card to a computer: see usb_storage_lend.h. */
#include "usb_storage_lend.h"
#include "storage_paths.h"
#include "usb_gadget.h"
#include "usb_mode_control.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

/* ---- the state machine ------------------------------------------------- */

void usb_lend_init(usb_lend_t * l, bool already_lent) {
    *l = (usb_lend_t) { .state = already_lent ? USB_LEND_LENT : USB_LEND_IDLE };
}

const char * usb_lend_state_name(usb_lend_state_t state) {
    switch (state) {
        case USB_LEND_IDLE: return "idle";
        case USB_LEND_CLOSING: return "closing";
        case USB_LEND_EXPORTING: return "exporting";
        case USB_LEND_LENT: return "lent";
        case USB_LEND_RECLAIMING: return "reclaiming";
        case USB_LEND_REOPENING: return "reopening";
    }
    return "?";
}

static usb_lend_action_t to_reclaim(usb_lend_t * l, usb_lend_end_t end) {
    l->state = USB_LEND_RECLAIMING;
    l->end = end;
    l->reclaim_tries = 1;
    return USB_LEND_DO_RECLAIM;
}

static usb_lend_action_t to_reopen(usb_lend_t * l, usb_lend_end_t end) {
    l->state = USB_LEND_REOPENING;
    if (end != USB_LEND_END_NONE) l->end = end;
    return USB_LEND_DO_REOPEN;
}

usb_lend_action_t usb_lend_step(usb_lend_t * l, const usb_lend_inputs_t * in, uint32_t now_ms) {
    switch (l->state) {
        case USB_LEND_IDLE:
            if (!in->cable) l->declined = false;
            if (in->want && in->cable && in->host && !l->declined) {
                l->state = USB_LEND_CLOSING;
                l->end = USB_LEND_END_NONE;
                l->unplug_seen = false;
                return USB_LEND_DO_CLOSE;
            }
            return USB_LEND_DO_NOTHING;

        case USB_LEND_CLOSING:
            if (in->close == USB_LEND_OK) {
                l->state = USB_LEND_EXPORTING;
                return USB_LEND_DO_EXPORT;
            }
            if (in->close == USB_LEND_FAILED) {
                l->declined = true;
                return to_reopen(l, USB_LEND_END_FAILED);
            }
            /* Still closing (a scan stopping): give up if the computer left. */
            if (!in->cable) return to_reopen(l, USB_LEND_END_FAILED);
            return USB_LEND_DO_CLOSE;

        case USB_LEND_EXPORTING:
            if (in->work == USB_LEND_OK) {
                l->state = USB_LEND_LENT;
                return USB_LEND_DO_SHOW;
            }
            if (in->work == USB_LEND_FAILED) {
                /* Undo whatever part of the export happened. */
                l->declined = true;
                return to_reclaim(l, USB_LEND_END_FAILED);
            }
            return USB_LEND_DO_NOTHING;

        case USB_LEND_LENT:
            if (!in->cable) {
                if (!l->unplug_seen) {
                    l->unplug_seen = true;
                    l->unplug_since_ms = now_ms;
                }
                if (now_ms - l->unplug_since_ms >= USB_LEND_UNPLUG_DEBOUNCE_MS) return to_reclaim(l, USB_LEND_END_UNPLUG);
                return USB_LEND_DO_NOTHING;
            }
            l->unplug_seen = false;
            if (in->ejected) {
                l->declined = true;
                return to_reclaim(l, USB_LEND_END_EJECT);
            }
            if (in->user_disconnect) {
                l->declined = true;
                return to_reclaim(l, USB_LEND_END_USER);
            }
            return USB_LEND_DO_NOTHING;

        case USB_LEND_RECLAIMING:
            if (in->work == USB_LEND_PENDING) return USB_LEND_DO_NOTHING;
            if (in->work == USB_LEND_FAILED && l->reclaim_tries < USB_LEND_RECLAIM_TRIES) {
                l->reclaim_tries++;
                return USB_LEND_DO_RECLAIM;
            }
            /* Back, or as far as it goes: the card hot-plug code reports a
             * card that did not mount. */
            return to_reopen(l, USB_LEND_END_NONE);

        case USB_LEND_REOPENING:
            if (!in->reopened) return USB_LEND_DO_REOPEN;
            l->state = USB_LEND_IDLE;
            if (l->end == USB_LEND_END_UNPLUG) l->declined = false;
            return USB_LEND_DO_NOTHING;
    }
    return USB_LEND_DO_NOTHING;
}

/* ---- running it ---------------------------------------------------------- */

static usb_lend_t lend;
static const usb_lend_hooks_t * lend_hooks;
static usb_lend_result_t close_result;
static bool reopened;
static atomic_int work_result = USB_LEND_PENDING;
static pthread_t work_thread;
static bool work_joinable;
static atomic_bool disconnect_requested;
static bool cable_last;

bool usb_storage_lend_supported(void) {
    const struct board_storage_export_ops * ops = usb_storage_export_ops();
    return ops && ops->lends_card && ops->export_card && ops->reclaim_card;
}

void usb_storage_lend_set_hooks(const usb_lend_hooks_t * hooks) { lend_hooks = hooks; }

static void * export_thread(void * arg) {
    (void) arg;
    bool ok = usb_storage_export_ops()->export_card();
    atomic_store(&work_result, ok ? USB_LEND_OK : USB_LEND_FAILED);
    return NULL;
}

static void * reclaim_thread(void * arg) {
    (void) arg;
    bool ok = usb_storage_export_ops()->reclaim_card();
    atomic_store(&work_result, ok ? USB_LEND_OK : USB_LEND_FAILED);
    return NULL;
}

static void start_work(void * (*fn)(void *)) {
    if (work_joinable) {
        pthread_join(work_thread, NULL); /* finished: its result was read */
        work_joinable = false;
    }
    atomic_store(&work_result, USB_LEND_PENDING);
    if (pthread_create(&work_thread, NULL, fn, NULL) == 0) work_joinable = true;
    else atomic_store(&work_result, USB_LEND_FAILED);
}

void usb_storage_lend_start(void) {
    if (!usb_storage_lend_supported()) return;
    const struct board_storage_export_ops * ops = usb_storage_export_ops();
    /* The LUN still holds the card: the player restarted during a lend. */
    bool lent = ops->host_ejected && !ops->host_ejected();
    usb_lend_init(&lend, lent);
    cable_last = lent;
    if (!lent) return;
    fprintf(stderr, "usb_storage: the card is still lent to a computer\n");
    mt_state_set_card_lent(true);
    if (lend_hooks && lend_hooks->show) lend_hooks->show(true);
}

void usb_storage_lend_poll(bool want, uint32_t now_ms) {
    if (!usb_storage_lend_supported() || !lend_hooks) return;
    const struct board_usb_gadget_ops * gadget = usb_gadget_ops();
    const struct board_storage_export_ops * ops = usb_storage_export_ops();

    usb_lend_inputs_t in = { 0 };
    /* An unreadable power state is not an unplug: while the card is away the
     * cable keeps its last known state, otherwise nothing starts on it. */
    bool cable = false;
    if (usb_mode_control_cable_state(&cable)) cable_last = cable;
    else cable = lend.state != USB_LEND_IDLE && cable_last;
    in.cable = cable;
    in.host = lend.state == USB_LEND_IDLE && gadget && gadget->host_connected && gadget->host_connected();
    in.want = want;
    in.ejected = lend.state == USB_LEND_LENT && ops->host_ejected && ops->host_ejected();
    in.user_disconnect = atomic_exchange(&disconnect_requested, false);
    in.close = close_result;
    in.work = (usb_lend_result_t) atomic_load(&work_result);
    in.reopened = reopened;

    usb_lend_state_t before = lend.state;
    usb_lend_action_t action = usb_lend_step(&lend, &in, now_ms);
    if (lend.state != before)
        fprintf(stderr, "usb_storage: %s -> %s\n", usb_lend_state_name(before), usb_lend_state_name(lend.state));

    switch (action) {
        case USB_LEND_DO_NOTHING:
            break;
        case USB_LEND_DO_CLOSE:
            close_result = lend_hooks->close_all ? lend_hooks->close_all() : USB_LEND_FAILED;
            reopened = false;
            break;
        case USB_LEND_DO_EXPORT:
            start_work(export_thread);
            break;
        case USB_LEND_DO_SHOW:
            if (lend_hooks->show) lend_hooks->show(true);
            break;
        case USB_LEND_DO_RECLAIM:
            start_work(reclaim_thread);
            break;
        case USB_LEND_DO_REOPEN:
            if (lend_hooks->reopen_all) lend_hooks->reopen_all(lend.end);
            if (lend_hooks->show) lend_hooks->show(false);
            reopened = true;
            close_result = USB_LEND_PENDING;
            break;
    }
}

bool usb_storage_lend_busy(void) { return lend.state != USB_LEND_IDLE; }
usb_lend_state_t usb_storage_lend_state(void) { return lend.state; }
void usb_storage_lend_user_disconnect(void) { atomic_store(&disconnect_requested, true); }

bool usb_storage_lend_card_free(const char * card_root, char * other, size_t other_size) {
    int pids[8];
    char names[8][32];
    if (other && other_size) other[0] = '\0';
    int n = usb_busy_scan(card_root, pids, names, 8);
    for (int i = 0; i < n && i < 8; i++) {
        fprintf(stderr, "usb_storage: %s still in use by pid %d (%s)\n", card_root, pids[i], names[i]);
        if (pids[i] == (int) getpid()) {
            /* Everything of ours should be closed by now: say what was missed. */
            usb_busy_log_files(card_root, pids[i]);
        } else if (other && other_size && !other[0]) {
            snprintf(other, other_size, "%s", names[i][0] ? names[i] : "?");
        }
    }
    return n == 0;
}
