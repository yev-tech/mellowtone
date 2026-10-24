#ifndef POWER_SYSFS_H
#define POWER_SYSFS_H

/* Shared sysfs access for the battery reader, the charger back ends and the
 * charge-control logic.
 *
 * Every path is relative to one sysfs root ("/sys" on the device) and every
 * board value comes from one descriptor (BOARD), so the host tests can run
 * the real code against a fake tree in a temp dir and a fake descriptor:
 * power_test_setup(). */

#include "board.h"

#include <stdbool.h>
#include <stddef.h>

/* The descriptor the power code reads: &BOARD unless a test replaced it. */
const board_desc_t * power_board(void);
/* "/sys" unless a test replaced it. */
const char * power_sysfs_root(void);

/* Tests only: board NULL = &BOARD, root NULL = "/sys". Not thread-safe;
 * call before starting anything that reads them. */
void power_test_setup(const board_desc_t * board, const char * sysfs_root);

/* rel is relative to the root, e.g. "class/power_supply/battery/capacity".
 * Reads the first line, without the trailing newline; false if missing or
 * empty. */
bool power_sysfs_read(const char * rel, char * out, size_t out_size);
bool power_sysfs_read_int(const char * rel, int * out);
/* Writes "<value>\n". */
bool power_sysfs_write_int(const char * rel, int value);
bool power_sysfs_write_str(const char * rel, const char * value);
/* Writes only when the current value differs (or cannot be read). True when
 * the attribute holds the value afterwards as far as we know. */
bool power_sysfs_write_int_if_changed(const char * rel, int value);
/* Exists and is writable by us. */
bool power_sysfs_writable(const char * rel);

/* /sys/class/power_supply/<node>/<attr> shorthands. */
bool power_supply_read(const char * node, const char * attr, char * out, size_t out_size);
bool power_supply_read_int(const char * node, const char * attr, int * out);
bool power_supply_writable(const char * node, const char * attr);
/* Builds "class/power_supply/<node>/<attr>" into out; false if it does not fit. */
bool power_supply_rel(const char * node, const char * attr, char * out, size_t out_size);

#endif /* POWER_SYSFS_H */
