#include "hw_buttons.h"
#include "board.h"
#include "debug_log.h"
#include "input_device_utils.h"
#include "loop_wake.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* One physical button press = one percentage point (0-100 range). */
#define VOLUME_STEP_PERCENT 1

/* Typematic repeat for volume keys: initial delay followed by periodic repeats. */
#define VOLUME_REPEAT_INITIAL_DELAY_MS 350
#define VOLUME_REPEAT_INTERVAL_MS 60

/* Threshold to trigger power long-press (power-off menu) instead of a short tap (screen toggle). */
#define POWER_LONG_PRESS_MS 700

/* Hold-to-seek threshold/repeat for the Next button, matching LVGL's own
 * default long-press timing (LV_INDEV_DEF_LONG_PRESS_TIME/_REP_TIME) so
 * holding the physical button feels the same as holding the touch one. */
#define NEXT_SEEK_LONG_PRESS_MS 400
#define NEXT_SEEK_REPEAT_MS 100

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Tracks press counts between GUI poll intervals to reliably detect multi-clicks. */
static int play_pause_press_count = 0;
static bool next_requested = false;
static bool prev_requested = false;
static bool power_requested = false;
static bool power_long_press_requested = false;
/* Double-tap to wake: the touch panel is polled here alongside the keys
 * (the thread sleeps in poll(), so this costs nothing while idle). Taps
 * only count while gui.c has armed it, i.e. while the screen is off. */
#define TAP_WAKE_DOUBLE_TAP_MS 450
static bool tap_wake_armed = false;
static bool tap_wake_requested = false;
static uint32_t tap_wake_last_down_ms = 0;
static int volume_delta = 0;

/* The R3 II knob is reported as arrow-key pairs. Track each physical key per
 * evdev fd so a repeated KEY_DOWN without a release cannot become multiple
 * detents; unlike ordinary volume keys, these inputs never enter the hold
 * repeat path below. */
static void handle_knob_key_event(unsigned short code, int value, bool * left_held, bool * right_held) {
    bool * held = code == KEY_LEFT ? left_held : code == KEY_RIGHT ? right_held : NULL;
    if (!held) return;

    pthread_mutex_lock(&state_mutex);
    if (value == 1 && !*held) {
        /* Reverse the stock arrow mapping to match the reported wheel direction. */
        volume_delta += code == KEY_RIGHT ? VOLUME_STEP_PERCENT : -VOLUME_STEP_PERCENT;
        *held = true;
    } else if (value == 0) {
        *held = false;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* SYN_DROPPED invalidates the remainder of the current evdev packet. Discard
 * through SYN_REPORT and clear held guards there so a lost release cannot
 * suppress future knob impulses. */
static bool knob_consume_sync_event(unsigned short type, unsigned short code,
                                    bool * sync_dropped, bool * left_held, bool * right_held) {
    if (*sync_dropped) {
        if (type == EV_SYN && code == SYN_REPORT) {
            *sync_dropped = false;
            *left_held = false;
            *right_held = false;
        }
        return true;
    }
    if (type == EV_SYN && code == SYN_DROPPED) {
        *sync_dropped = true;
        return true;
    }
    return false;
}

/* Held-state + next-repeat-due tracking for volume up/down specifically --
 * the only two keys that repeat while held. Everything else (play/pause,
 * next/prev) stays single-shot: repeating a track skip while a finger
 * lingers on the button would be actively wrong, not just unnecessary. */
static bool volume_up_held = false;
static bool volume_down_held = false;
static uint32_t volume_up_next_repeat_ms = 0;
static uint32_t volume_down_next_repeat_ms = 0;

/* Power held-state + long-press-due tracking, same shape as the volume
 * repeat state above -- power_long_press_fired guards against firing the
 * long-press flag more than once per physical press, and (in
 * handle_key_event()'s value==0 branch) against also firing the short-tap
 * flag once the same press has already turned into a long-press. */
static bool power_held = false;
static bool power_long_press_fired = false;
static uint32_t power_long_press_due_ms = 0;

/* Next-button hold-to-seek state, same shape as the power long-press state
 * above: next_seek_fired guards against firing the "first step" flag more
 * than once per hold, and against also firing the short-press flag once the
 * hold has taken over. next_seek_step_count accumulates on this thread at
 * NEXT_SEEK_REPEAT_MS granularity; the GUI thread drains it independently of
 * its own (coarser) poll interval, exactly like volume_delta above. */
static bool next_held = false;
static bool next_seek_fired = false;
static uint32_t next_seek_due_ms = 0;
static int next_seek_step_count = 0;
static bool next_seek_step_is_first = false;

/* A key's release can be lost: its device went away (an accessory
 * unplugged, an fd error) or the kernel dropped events (SYN_DROPPED). A
 * held flag left set would keep repeating -- volume climbing to 100 % -- so
 * every hold is forgotten then, whichever device it came from; a key still
 * down only needs pressing again. Fires nothing (no short press, no
 * long-press). */
static void release_held_keys(void) {
    pthread_mutex_lock(&state_mutex);
    volume_up_held = false;
    volume_down_held = false;
    power_held = false;
    next_held = false;
    pthread_mutex_unlock(&state_mutex);
}

/* SYN_DROPPED on a key device: discard the rest of the packet through
 * SYN_REPORT, and release every hold at once. */
static bool key_consume_sync_event(unsigned short type, unsigned short code, bool * sync_dropped) {
    if (*sync_dropped) {
        if (type == EV_SYN && code == SYN_REPORT) *sync_dropped = false;
        return true;
    }
    if (type == EV_SYN && code == SYN_DROPPED) {
        *sync_dropped = true;
        release_held_keys();
        return true;
    }
    return false;
}

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* value: 1 = key down, 0 = key up (repeat events, value 2, are ignored --
 * this device's own repeat pacing below replaces whatever the kernel might
 * otherwise synthesize, and only actually fires for the two keys that
 * should repeat at all). */
static void handle_key_event(unsigned short code, int value) {
    pthread_mutex_lock(&state_mutex);
    if (value == 1) {
        switch (code) {
            case KEY_POWER: {
                uint32_t now = monotonic_ms();
                power_held = true;
                power_long_press_fired = false;
                power_long_press_due_ms = now + POWER_LONG_PRESS_MS;
                DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", now);
                break;
            }
            /* Accessory remotes do not all speak the same code for the same
             * button: HID consumer-page transport controls arrive as any of
             * these depending on the cable's descriptor. */
            case KEY_PLAYPAUSE:
            case KEY_PLAY:
            case KEY_PLAYCD:
            case KEY_PAUSECD:        play_pause_press_count++; break;
            case KEY_FASTFORWARD:
            case KEY_NEXTSONG: {
                uint32_t now = monotonic_ms();
                next_held = true;
                next_seek_fired = false;
                next_seek_due_ms = now + NEXT_SEEK_LONG_PRESS_MS;
                break;
            }
            case KEY_REWIND:
            case KEY_PREVIOUSSONG:   prev_requested = true; break;
            case KEY_VOLUMEUP:
                volume_delta += VOLUME_STEP_PERCENT;
                volume_up_held = true;
                volume_up_next_repeat_ms = monotonic_ms() + VOLUME_REPEAT_INITIAL_DELAY_MS;
                break;
            case KEY_VOLUMEDOWN:
                volume_delta -= VOLUME_STEP_PERCENT;
                volume_down_held = true;
                volume_down_next_repeat_ms = monotonic_ms() + VOLUME_REPEAT_INITIAL_DELAY_MS;
                break;
            default: break;
        }
    } else if (value == 0) {
        switch (code) {
            case KEY_POWER:
                /* Only a short tap (released before the long-press
                 * threshold fired) sets the screen-toggle flag -- if
                 * power_long_press_fired is already true, that flag
                 * (hw_buttons_consume_power_long_press()) already told
                 * gui.c to show the power-off countdown for this same
                 * press, and the release shouldn't also toggle the screen
                 * out from under it. */
                if (power_held && !power_long_press_fired) power_requested = true;
                power_held = false;
                break;
            case KEY_VOLUMEUP:   volume_up_held = false; break;
            case KEY_VOLUMEDOWN: volume_down_held = false; break;
            case KEY_FASTFORWARD:
            case KEY_NEXTSONG:
                /* Same suppression as KEY_POWER above: only a release that
                 * never crossed the seek threshold counts as a short press. */
                if (next_held && !next_seek_fired) next_requested = true;
                next_held = false;
                break;
            default: break;
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every pass of the main loop below while a volume key is held
 * -- applies a repeat step for whichever key(s) are due, independently, so
 * both keys held at once (unusual, but not prevented) repeat on their own
 * separate schedules rather than one blocking the other. */
static void apply_due_volume_repeats(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (volume_up_held && (int32_t) (now - volume_up_next_repeat_ms) >= 0) {
        volume_delta += VOLUME_STEP_PERCENT;
        volume_up_next_repeat_ms = now + VOLUME_REPEAT_INTERVAL_MS;
    }
    if (volume_down_held && (int32_t) (now - volume_down_next_repeat_ms) >= 0) {
        volume_delta -= VOLUME_STEP_PERCENT;
        volume_down_next_repeat_ms = now + VOLUME_REPEAT_INTERVAL_MS;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every pass of the main loop below while the power button is
 * held -- fires the long-press flag exactly once per press, the
 * moment the hold crosses POWER_LONG_PRESS_MS, independent of when (or
 * whether) the button is ever released. */
static void apply_due_power_long_press(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (power_held && !power_long_press_fired && (int32_t) (now - power_long_press_due_ms) >= 0) {
        power_long_press_fired = true;
        power_long_press_requested = true;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every pass of the main loop below while the Next button is
 * held -- accumulates one seek step every NEXT_SEEK_REPEAT_MS once
 * the hold has passed NEXT_SEEK_LONG_PRESS_MS, marking the first such step
 * per hold via next_seek_step_is_first. */
static void apply_due_next_seek(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (next_held && (int32_t) (now - next_seek_due_ms) >= 0) {
        next_seek_step_count++;
        if (!next_seek_fired) {
            next_seek_fired = true;
            next_seek_step_is_first = true;
        }
        next_seek_due_ms = now + NEXT_SEEK_REPEAT_MS;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Media keys are what an accessory remote sends: a USB DSP cable or dongle
 * carries its own transport buttons, and its device name is the vendor's, so
 * these are found by what they can send rather than by what they are called. */
static bool device_sends_media_keys(int fd) {
    unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
#define HW_BUTTONS_HAS_KEY(code) \
    ((bits[(code) / (8 * sizeof(unsigned long))] >> ((code) % (8 * sizeof(unsigned long)))) & 1UL)
    return HW_BUTTONS_HAS_KEY(KEY_PLAYPAUSE) || HW_BUTTONS_HAS_KEY(KEY_NEXTSONG) ||
           HW_BUTTONS_HAS_KEY(KEY_PREVIOUSSONG) || HW_BUTTONS_HAS_KEY(KEY_PLAY) ||
           HW_BUTTONS_HAS_KEY(KEY_PLAYCD) || HW_BUTTONS_HAS_KEY(KEY_PAUSECD) ||
           HW_BUTTONS_HAS_KEY(KEY_FASTFORWARD) || HW_BUTTONS_HAS_KEY(KEY_REWIND);
#undef HW_BUTTONS_HAS_KEY
}

#define HW_BUTTONS_MAX_FDS 12
/* Accessories are found through inotify on /dev/input (a new event node
 * triggers a rescan) and the poll below then blocks indefinitely. Only if
 * inotify is unavailable does it fall back to rescanning on this cadence:
 * a readdir plus one ioctl per unopened event node. */
#define MEDIA_KEY_RESCAN_INTERVAL_MS 2000
/* A node can appear a moment before its device answers EVIOCGBIT: one more
 * rescan this long after an inotify event. */
#define MEDIA_KEY_SETTLE_RESCAN_MS 500

/* True when a press is queued for gui.c to consume. Caller holds state_mutex. */
static bool request_pending_locked(void) {
    return play_pause_press_count > 0 || next_requested || prev_requested || power_requested ||
           power_long_press_requested || tap_wake_requested || volume_delta != 0 || next_seek_step_count > 0;
}

/* gui.c drains the queue from its timers; with the screen off those only
 * run when the main loop is woken. */
static void wake_gui_if_pending(void) {
    pthread_mutex_lock(&state_mutex);
    bool pending = request_pending_locked();
    pthread_mutex_unlock(&state_mutex);
    if (pending) loop_wake_signal();
}

static int open_input_dir_watch(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    if (inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Drains the watch; true if anything named event* appeared or changed. */
static bool input_dir_watch_drain(int fd) {
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool relevant = false;
    ssize_t len;
    while ((len = read(fd, buf, sizeof(buf))) > 0) {
        for (char * p = buf; p < buf + len;) {
            const struct inotify_event * ev = (const struct inotify_event *) p;
            if (ev->len && strncmp(ev->name, "event", 5) == 0) relevant = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return relevant;
}

/* A Bluetooth remote's uinput keyboard belongs to bt_media_player.c's AVRCP
 * reader: BlueZ 4 names it after the peer address ("00:11:22:33:44:55"),
 * BlueZ 5 puts "(AVRCP)" in the name. Reading it here as well turned every
 * Play press into two toggles, play then pause at once. */
static bool device_is_bluetooth_remote(int fd) {
    char name[64] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return false;
    if (strstr(name, "(AVRCP)")) return true;
    if (strlen(name) != 17) return false;
    for (int i = 0; i < 17; i++) {
        char c = name[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (i % 3 == 2 ? c != ':' : !hex) return false;
    }
    return true;
}

/* Adds any event device with transport keys that is not already open. Called
 * again on a cadence because an accessory can be plugged in long after boot,
 * which the original one-shot enumeration could never see. */
static int scan_media_key_devices(struct pollfd * fds, int nfds, char paths[][64]) {
    DIR * dir = opendir("/dev/input");
    if (!dir) return nfds;

    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL && nfds < HW_BUTTONS_MAX_FDS) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[64]; /* the size of paths[] entries; event nodes are short */
        if ((size_t) snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name) >= sizeof(path)) continue;

        bool already_open = false;
        for (int i = 0; i < nfds; i++)
            if (strcmp(paths[i], path) == 0) { already_open = true; break; }
        if (already_open) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!device_sends_media_keys(fd) || device_is_bluetooth_remote(fd)) {
            close(fd);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        memcpy(paths[nfds], path, sizeof(path));
        DBG_LOG("hw_buttons: media-key device %s opened at fd_index=%d\n", path, nfds);
        nfds++;
    }
    closedir(dir);
    return nfds;
}

static void * hw_buttons_thread_func(void * arg) {
    (void) arg;

    /* The board's key devices (BOARD.input.buttons): the R3 Pro adds
     * gpio-keys/adc_key, the R3 II 2025 its volume knob (sa-ring-keys). */
    const board_input_device_t * const BUTTON_DEVICES = BOARD.input.buttons;

    /* O_NONBLOCK prevents empty read queues on one device from blocking poll
     * and starving inputs from the other button devices. */
    /* One extra slot past the devices: the /dev/input watch. */
    struct pollfd fds[HW_BUTTONS_MAX_FDS + 1];
    char paths[HW_BUTTONS_MAX_FDS][64];
    /* Arrow keys mean volume only on the knob, never on an accessory. */
    bool knob[HW_BUTTONS_MAX_FDS] = { false };
    bool knob_left_held[HW_BUTTONS_MAX_FDS] = { false };
    bool knob_right_held[HW_BUTTONS_MAX_FDS] = { false };
    bool sync_dropped[HW_BUTTONS_MAX_FDS] = { false };
    bool touch[HW_BUTTONS_MAX_FDS] = { false };
    int nfds = 0;
    int found = 0;
    for (size_t i = 0; BUTTON_DEVICES[i].name && nfds < HW_BUTTONS_MAX_FDS; i++) {
        char path[64];
        if (!find_input_device_by_name(BUTTON_DEVICES[i].name, path, sizeof(path))) continue;
        found++;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "hw_buttons: failed to open %s\n", path);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        knob[nfds] = BUTTON_DEVICES[i].knob;
        snprintf(paths[nfds], sizeof(paths[nfds]), "%s", path);
        DBG_LOG("hw_buttons: fd_index=%d -> %s (%s)\n", nfds, path, BUTTON_DEVICES[i].name);
        nfds++;
    }

    /* Touch panel, for double-tap to wake. A separate open gets its own
     * copy of every event, so LVGL's own reader is unaffected. */
    const char * const * const TOUCH_DEVICES = BOARD.input.touch;
    for (size_t i = 0; TOUCH_DEVICES[i] && nfds < HW_BUTTONS_MAX_FDS; i++) {
        char path[64];
        if (!find_input_device_by_name(TOUCH_DEVICES[i], path, sizeof(path))) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        touch[nfds] = true;
        snprintf(paths[nfds], sizeof(paths[nfds]), "%s", path);
        nfds++;
        break;
    }

    /* The built-in keys are named and known; an accessory's remote is not, so
     * it is picked up by capability here and on every rescan below. */
    nfds = scan_media_key_devices(fds, nfds, paths);

    if (found == 0 && nfds == 0) {
        fprintf(stderr, "hw_buttons: no physical button input devices found, hardware keys disabled\n");
        return NULL;
    }

    if (nfds == 0) return NULL;

    printf("hw_buttons: listening for physical button presses\n");

    int watch_fd = open_input_dir_watch();
    if (watch_fd < 0) DBG_LOG("hw_buttons: no inotify on /dev/input, rescanning every %d ms\n",
                              MEDIA_KEY_RESCAN_INTERVAL_MS);
    uint32_t settle_rescan_due_ms = 0; /* 0 = none pending */

    while (1) {
        pthread_mutex_lock(&state_mutex);
        bool volume_held = volume_up_held || volume_down_held;
        bool power_pending_long_press = power_held && !power_long_press_fired;
        bool next_pending_seek = next_held;
        pthread_mutex_unlock(&state_mutex);

        /* Blocks until a key, a new /dev/input node (an accessory plugged in
         * after boot) or a held key's next repeat step or long-press
         * threshold. Without the watch, rescans on a timeout instead. */
        int timeout = watch_fd < 0 ? MEDIA_KEY_RESCAN_INTERVAL_MS : -1;
        if (settle_rescan_due_ms) {
            int32_t left = (int32_t) (settle_rescan_due_ms - monotonic_ms());
            timeout = left > 0 ? left : 0;
        }
        if (volume_held || power_pending_long_press || next_pending_seek) timeout = VOLUME_REPEAT_INTERVAL_MS;
        int poll_count = nfds;
        if (watch_fd >= 0) {
            fds[poll_count].fd = watch_fd;
            fds[poll_count].events = POLLIN;
            fds[poll_count].revents = 0;
            poll_count++;
        }
        int ret = poll(fds, (nfds_t) poll_count, timeout);
        /* Due repeats run on every pass, not only on a timeout: a stream
         * of events from another device (the touch panel under a moving
         * finger) would otherwise hold them back. */
        apply_due_volume_repeats();
        apply_due_power_long_press();
        apply_due_next_seek();
        wake_gui_if_pending();
        bool rescan = false;
        if (watch_fd >= 0 && ret > 0 && (fds[nfds].revents & POLLIN) && input_dir_watch_drain(watch_fd)) {
            rescan = true;
            settle_rescan_due_ms = monotonic_ms() + MEDIA_KEY_SETTLE_RESCAN_MS;
            if (!settle_rescan_due_ms) settle_rescan_due_ms = 1;
        } else if (settle_rescan_due_ms && (int32_t) (monotonic_ms() - settle_rescan_due_ms) >= 0) {
            rescan = true;
            settle_rescan_due_ms = 0;
        }
        if (ret == 0 && watch_fd < 0) rescan = true;
        if (rescan) nfds = scan_media_key_devices(fds, nfds, paths);
        if (ret <= 0) continue;

        for (int i = 0; i < nfds; i++) {
            /* An unplugged accessory reports an error rather than data. Its
             * slot is closed and the last one moved down, so a cable can come
             * and go without leaking descriptors or wedging the poll set. */
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                DBG_LOG("hw_buttons: %s went away, closing fd_index=%d\n", paths[i], i);
                close(fds[i].fd);
                /* Its key releases will never come. */
                if (!touch[i]) release_held_keys();
                nfds--;
                if (i != nfds) {
                    fds[i] = fds[nfds];
                    knob[i] = knob[nfds];
                    knob_left_held[i] = knob_left_held[nfds];
                    knob_right_held[i] = knob_right_held[nfds];
                    sync_dropped[i] = sync_dropped[nfds];
                    touch[i] = touch[nfds];
                    memcpy(paths[i], paths[nfds], sizeof(paths[i]));
                }
                /* A rescan may reuse the freed slot for an accessory. */
                knob[nfds] = false;
                knob_left_held[nfds] = false;
                knob_right_held[nfds] = false;
                sync_dropped[nfds] = false;
                touch[nfds] = false;
                i--;
                continue;
            }
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event ev;
            if (touch[i]) {
                /* Always drained; finger-down edges count only while armed. */
                while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
                    if (ev.type != EV_KEY || ev.code != BTN_TOUCH || ev.value != 1) continue;
                    uint32_t now = (uint32_t) (ev.input_event_sec * 1000 + ev.input_event_usec / 1000);
                    pthread_mutex_lock(&state_mutex);
                    if (tap_wake_armed) {
                        if (tap_wake_last_down_ms && now - tap_wake_last_down_ms <= TAP_WAKE_DOUBLE_TAP_MS) {
                            tap_wake_requested = true;
                            tap_wake_last_down_ms = 0;
                        } else {
                            tap_wake_last_down_ms = now;
                        }
                    }
                    pthread_mutex_unlock(&state_mutex);
                }
                continue;
            }
            while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
                DBG_LOG("hw_buttons: raw event fd_index=%d type=%u code=%u value=%d\n", i, ev.type, ev.code, ev.value);
                if (knob[i] && knob_consume_sync_event(ev.type, ev.code,
                                                       &sync_dropped[i],
                                                       &knob_left_held[i],
                                                       &knob_right_held[i])) continue;
                if (!knob[i] && key_consume_sync_event(ev.type, ev.code, &sync_dropped[i])) continue;
                if (ev.type == EV_KEY && ev.value != 2) { /* key down/up, ignore kernel autorepeat */
                    unsigned short code = ev.code;
                    if (knob[i]) {
                        handle_knob_key_event(code, ev.value,
                                              &knob_left_held[i], &knob_right_held[i]);
                    } else {
                        handle_key_event(code, ev.value);
                    }
                }
            }
        }
        wake_gui_if_pending();
    }

    return NULL;
}

void hw_buttons_init(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, hw_buttons_thread_func, NULL);
    pthread_detach(thread);
}

void hw_buttons_set_tap_wake_armed(bool armed) {
    pthread_mutex_lock(&state_mutex);
    tap_wake_armed = armed;
    tap_wake_last_down_ms = 0;
    if (!armed) tap_wake_requested = false;
    pthread_mutex_unlock(&state_mutex);
}

bool hw_buttons_consume_tap_wake(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = tap_wake_requested;
    tap_wake_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_power(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = power_requested;
    power_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_power_long_press(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = power_long_press_requested;
    power_long_press_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_play_pause(void) {
    pthread_mutex_lock(&state_mutex);
    int result = play_pause_press_count;
    play_pause_press_count = 0;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_next(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = next_requested;
    next_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_prev(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = prev_requested;
    prev_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_next_seek_steps(bool * out_is_first) {
    pthread_mutex_lock(&state_mutex);
    int result = next_seek_step_count;
    next_seek_step_count = 0;
    if (out_is_first) *out_is_first = next_seek_step_is_first;
    next_seek_step_is_first = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_volume_delta(void) {
    pthread_mutex_lock(&state_mutex);
    int result = volume_delta;
    volume_delta = 0;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
