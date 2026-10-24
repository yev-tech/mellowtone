#include "idle_shutdown.h"
#include "board.h"
#include "board_launcher.h"

#ifndef HOST_BUILD
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

/* mellowtone-launcher reboots on this exit status (board_launcher.h), after
 * releasing the card -- or opens its menu when the player asked for it. */
#define IDLE_SHUTDOWN_REBOOT_EXIT_CODE BOARD_LAUNCHER_EXIT_REBOOT

static bool parent_is_launcher(void) {
    char path[32];
    char target[256];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) getppid());
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) return false;
    target[length] = '\0';
    const char * base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strcmp(base, "mellowtone-launcher") == 0;
}
#endif

void idle_shutdown_now(void) {
#ifndef HOST_BUILD
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    /* Must not go through subprocess_run(): that helper waits 15s then
     * SIGKILLs the child. /sbin/poweroff (busybox, talks to init or waits
     * on other processes) routinely outlives that budget, so the 3-2-1
     * countdown reached zero, the child was killed, and the device stayed
     * on -- only a hardware power-button hold actually cut power. */
    sync();
    execl("/sbin/poweroff", "poweroff", (char *) NULL);
    reboot(RB_POWER_OFF);
    for (;;) pause();
#endif
}

void idle_shutdown_reboot_now(void) {
#ifndef HOST_BUILD
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    /* Under mellowtone-launcher this must NOT execl() an external
     * /sbin/reboot on X1600: the launcher treats a clean (status 0) exit of
     * the player as "power off" there (the X1600 launcher's rule),
     * and /sbin/reboot exits 0 once it hands off to init -- a real "reboot
     * just shuts down instead" report. Exiting with the reboot status lets
     * the launcher release the card and reboot (or open its menu). */
    sync();
    /* Under the launcher, exit instead: once this process is gone the
     * launcher can unmount the SD card before rebooting, which clears the
     * FAT dirty mark. Rebooting from here leaves the card looking unsafely
     * removed on the next boot. */
    if (parent_is_launcher()) _exit(IDLE_SHUTDOWN_REBOOT_EXIT_CODE);
    if (BOARD.system.reboot_via_init) {
        /* The shell launcher (/ui_data/player): busybox reboot lets init run
         * rcK and unmount the SD card, as the stock player does; it waits out
         * a clean (status 0) exit instead of restarting the player. */
        execl("/sbin/reboot", "reboot", (char *) NULL);
    }
    reboot(RB_AUTOBOOT);
    for (;;) pause();
#endif
}
