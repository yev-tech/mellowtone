#ifndef LAUNCHER_CLIENT_H
#define LAUNCHER_CLIENT_H

/* The player's side of mellowtone-launcher: what is waiting on
 * the card for the recovery menu, and the one-shot "open the menu at the
 * next start" flag. Paths from BOARD.launcher (board_launcher.h). */

#include <stdbool.h>
#include <stddef.h>

/* A firmware image or a player update package for this board is on the
 * card; name (may be NULL) gets the file name. */
bool launcher_update_on_card(char * name, size_t size);

/* Writes the flag (flash, BOARD.launcher->boot_dir). The caller restarts
 * (idle_shutdown_reboot_now()): under mellowtone-launcher the player's exit
 * opens the menu without a reboot. */
bool launcher_request_menu(void);

/* MELLOWTONE_NOTICE from the launcher ("rolled_back", "boot_loop",
 * "factory"), or NULL. */
const char * launcher_notice(void);

#endif
