#ifndef LAUNCHER_HOOKS_COMMON_H
#define LAUNCHER_HOOKS_COMMON_H

/* Helpers shared by the board hooks (libc only; also linked into the player
 * through board.c, see board_launcher.h). */

#include <stdbool.h>
#include <stddef.h>

/* Runs argv[0] (PATH is searched) with stdio on /dev/null; kills it after
 * timeout_ms. Returns its exit status, or -1 (not run, killed, signalled). */
int blh_run(char * const argv[], int timeout_ms);

/* Writes text to a sysfs/proc file. */
bool blh_write(const char * path, const char * text);

/* Reads a small file, newline stripped. "" on failure. */
const char * blh_read(const char * path, char * buf, size_t size);

/* Seconds since boot (/proc/uptime precision is enough here). */
long blh_uptime_s(void);

void blh_sleep_ms(int ms);

#endif
