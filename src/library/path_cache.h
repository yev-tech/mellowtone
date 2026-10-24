#ifndef PATH_CACHE_H
#define PATH_CACHE_H

#include <stdbool.h>

/* Small path-list files next to the tagcache directory. Used for app
 * lists tagcache itself does not store (playlist index). */

#define PATH_CACHE_PLAYLISTS "playlists.list"

void path_cache_replace(const char * name, char * const * paths, int count);
/* Drops in-RAM lists so the next load reads the files on the currently
 * mounted SD card. Used when the card is removed or replaced. */
void path_cache_drop(void);
/* Bind to rootfd/.mellowtone/library; the caller owns rootfd. */
bool path_cache_bind_directory_fd(int rootfd);
void path_cache_load(const char * name, char *** out_paths, int * out_count);
void path_cache_insert(const char * name, const char * path);
void path_cache_delete(const char * name, const char * path);

#endif /* PATH_CACHE_H */
