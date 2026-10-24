/* Keys and touch for the menu, straight from evdev (the descriptor's input
 * devices, matched by name). */
#define _GNU_SOURCE
#include "lx_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS 12
#define BITS_PER_LONG (sizeof(unsigned long) * 8)

static struct {
    int fd[MAX_DEVS];
    bool touch[MAX_DEVS];
    int n;
    int screen_h;
    int abs_min, abs_max; /* touch Y range */
    int touch_y;
    bool touching;
} in;

static bool name_listed(const char * name, const char * const * names) {
    for (int i = 0; names && names[i]; i++)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

/* /dev/input/event* whose name is in names; calls fn(fd) for each (fd stays
 * open when fn returns true). */
static void each_device(const char * const * names, bool (*fn)(int fd, void * ctx), void * ctx) {
    DIR * d = opendir("/dev/input");
    if (!d) return;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300], name[128] = "";
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 || !name_listed(name, names) || !fn(fd, ctx)) close(fd);
    }
    closedir(d);
}

static bool add_dev(int fd, void * ctx) {
    if (in.n >= MAX_DEVS) return false;
    bool touch = *(bool *) ctx;
    in.fd[in.n] = fd;
    in.touch[in.n] = touch;
    in.n++;
    if (touch) {
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > ai.minimum) {
            in.abs_min = ai.minimum;
            in.abs_max = ai.maximum;
        } else if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > ai.minimum) {
            in.abs_min = ai.minimum;
            in.abs_max = ai.maximum;
        }
    }
    return true;
}

void lx_input_open(const char * const * buttons, const char * const * touch, int screen_h) {
    in.n = 0;
    in.screen_h = screen_h;
    in.abs_min = 0;
    in.abs_max = screen_h - 1;
    bool t = false;
    each_device(buttons, add_dev, &t);
    t = true;
    each_device(touch, add_dev, &t);
    /* Drop whatever was queued before the menu opened. */
    struct input_event ev;
    for (int i = 0; i < in.n; i++)
        while (read(in.fd[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {}
}

void lx_input_close(void) {
    for (int i = 0; i < in.n; i++) close(in.fd[i]);
    in.n = 0;
}

static lx_key_t map_key(int code, int value) {
    bool press = value == 1, repeat = value == 2;
    switch (code) {
    case KEY_VOLUMEUP: case KEY_UP: case KEY_PREVIOUSSONG: case KEY_LEFT:
        return press || repeat ? LX_KEY_UP : LX_KEY_NONE;
    case KEY_VOLUMEDOWN: case KEY_DOWN: case KEY_NEXTSONG: case KEY_RIGHT:
        return press || repeat ? LX_KEY_DOWN : LX_KEY_NONE;
    case KEY_PLAYPAUSE: case KEY_PLAY: case KEY_PAUSE: case KEY_ENTER: case KEY_OK: case KEY_SELECT:
        return press ? LX_KEY_SELECT : LX_KEY_NONE;
    case KEY_POWER: case KEY_BACK: case KEY_ESC:
        return press ? LX_KEY_BACK : LX_KEY_NONE;
    default:
        return LX_KEY_NONE;
    }
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

lx_key_t lx_input_next(int timeout_ms, int * tap_y) {
    struct pollfd pfd[MAX_DEVS];
    for (int i = 0; i < in.n; i++) pfd[i] = (struct pollfd) { .fd = in.fd[i], .events = POLLIN };
    int64_t end = timeout_ms < 0 ? -1 : mono_ms() + timeout_ms;
    for (;;) {
        int wait = end < 0 ? -1 : (int) (end - mono_ms());
        if (end >= 0 && wait <= 0) return LX_KEY_NONE;
        if (in.n == 0) {
            usleep((useconds_t) (wait < 0 ? 1000 : wait) * 1000u);
            if (end < 0) continue;
            return LX_KEY_NONE;
        }
        int r = poll(pfd, (nfds_t) in.n, wait);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return LX_KEY_NONE;
        for (int i = 0; i < in.n; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(in.fd[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
                if (in.touch[i]) {
                    if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)) {
                        int span = in.abs_max - in.abs_min;
                        in.touch_y = span > 0 ? (int) ((long) (ev.value - in.abs_min) * (in.screen_h - 1) / span) : ev.value;
                    } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                        if (ev.value) in.touching = true;
                        else if (in.touching) {
                            in.touching = false;
                            /* The panel reports in the viewer's orientation
                             * (main.c's touch set-up), like our drawing. */
                            if (tap_y) *tap_y = in.touch_y;
                            return LX_KEY_TAP;
                        }
                    }
                    continue;
                }
                if (ev.type != EV_KEY) continue;
                lx_key_t k = map_key(ev.code, ev.value);
                if (k != LX_KEY_NONE) return k;
            }
        }
    }
}

bool lx_input_key_held(const char * const * devices, int code, int window_ms) {
    if (code <= 0 || code > KEY_MAX) return false;
    int64_t end = mono_ms() + window_ms;
    do {
        DIR * d = opendir("/dev/input");
        if (!d) return false;
        struct dirent * e;
        bool down = false;
        while (!down && (e = readdir(d))) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            char path[300], name[128] = "";
            snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned long keys[KEY_MAX / BITS_PER_LONG + 1];
            memset(keys, 0, sizeof(keys));
            bool match = !devices || (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && name_listed(name, devices));
            if (match && ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
                down = (keys[code / BITS_PER_LONG] >> (code % BITS_PER_LONG)) & 1UL;
            close(fd);
        }
        closedir(d);
        if (down) return true;
        usleep(100000);
    } while (mono_ms() < end);
    return false;
}
