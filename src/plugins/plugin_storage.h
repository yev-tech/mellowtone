#ifndef PLUGIN_STORAGE_H
#define PLUGIN_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Namespaced per-plugin key/value files on the card
 * (<card>/.mellowtone/plugin-data/<plugin id>). plugin.storage and
 * plugin.secrets share this tree; hashed directory/file names keep a
 * plugin id + key from becoming a path component.
 *
 * Threat model: this device has no hardware keystore. "Secrets" means
 * owner-only 0600/0700 files plus plugin_storage_path_is_reserved() on
 * every Lua io/os path and every native plugin.* path argument. That
 * guard is the real isolation: every plugin runs in the same process
 * and user, so file mode bits alone would not stop io.open() of another
 * plugin's secrets file. Neither layer defends against root or physical
 * access. This is not encryption.
 *
 * Quotas (combined storage + secrets per plugin): 256 KiB per value,
 * 500 keys, 2 MiB; 8 MiB across all plugins. Tracked in an in-memory
 * cache seeded from a directory scan on first touch. */

/* True when path is in, or is, a reserved tree: <card>/.mellowtone/
 * plugin-data (plugin.storage and plugin.secrets) or <card>/.mellowtone/
 * config (settings, credentials). Compared after resolving symlinks,
 * ignoring case, and by directory identity, since the card's file system
 * ignores case. A path that cannot be checked counts as reserved. */
bool plugin_storage_path_is_reserved(const char * path);
/* plugin_storage_path_is_reserved(), or path is an ancestor of a reserved
 * tree (e.g. <card>/.mellowtone): moving it would move that tree. For
 * rename's source and destination. */
bool plugin_storage_path_holds_reserved(const char * path);

/* The player's install root in flash (read-only for plugins). */
bool plugin_path_in_install_root(const char * path);

bool plugin_storage_get(const char * plugin_id, const char * key, char ** out_value, size_t * out_len);
bool plugin_storage_set(const char * plugin_id, const char * key, const char * value, size_t value_len);
bool plugin_storage_delete(const char * plugin_id, const char * key);
int plugin_storage_list(const char * plugin_id, const char * prefix, char *** out_keys);

bool plugin_secrets_get(const char * plugin_id, const char * key, char ** out_value, size_t * out_len);
bool plugin_secrets_set(const char * plugin_id, const char * key, const char * value, size_t value_len);
bool plugin_secrets_exists(const char * plugin_id, const char * key);
bool plugin_secrets_delete(const char * plugin_id, const char * key);

#endif /* PLUGIN_STORAGE_H */