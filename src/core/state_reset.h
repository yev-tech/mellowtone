#ifndef STATE_RESET_H
#define STATE_RESET_H

/* The state root on the card, shared by the player (storage_paths.h) and
 * the launcher's recovery menu. No card checks here: the caller knows the
 * card is writable. Builds without a board (the roots are parameters).
 *
 * Settings reset: moves <sd_root>/.mellowtone/config to
 * .mellowtone/backup/reset-<date>[-n]/. Library, cache, logs and music
 * stay. */

#include <stdbool.h>
#include <stddef.h>

/* Creates <sd_root>/.mellowtone and its standard subdirectories (config,
 * plugins, plugin-data, library, cache, logs, backup, update). Idempotent. */
bool mt_state_create_layout_at(const char * sd_root);

/* True when there was nothing to move or it was moved; backup_out (may be
 * NULL) gets the backup directory. */
bool mt_state_reset_config_at(const char * sd_root, char * backup_out, size_t backup_size);

#endif
