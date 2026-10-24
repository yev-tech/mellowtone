#include "shell_state.h"

#include <stdio.h>
#include <string.h>

/* snprintf() with overlapping source and destination is undefined; glibc
 * empties the destination first, so re-applying the cache through
 * gui_shell_update_quick_drawer_track() used to blank the cached title and
 * turn Home into "Nothing playing". Copy through a scratch buffer. */
static void copy_text(char * dst, size_t size, const char * src) {
    if (!size) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    size_t len = strlen(tmp);
    if (len >= size) len = size - 1;
    memcpy(dst, tmp, len);
    dst[len] = '\0';
}

void shell_now_playing_set_track(shell_now_playing_t * np, const char * title, const char * artist,
                                 const char * album) {
    if (!np) return;
    bool has_track = title != NULL;
    copy_text(np->title, sizeof(np->title), title);
    copy_text(np->artist, sizeof(np->artist), artist);
    copy_text(np->album, sizeof(np->album), album);
    np->has_track = has_track;
}

void shell_now_playing_set_format(shell_now_playing_t * np, const char * text) {
    if (!np) return;
    copy_text(np->format, sizeof(np->format), text);
}

const char * shell_bt_status_glyph(bool powered, bool connected) {
    if (!powered) return NULL;
    return connected ? "topbar/m_bt.png" : "topbar/lucide_bluetooth_off.png";
}
