#ifndef GUI_RETAG_H
#define GUI_RETAG_H

#include <stdbool.h>

/* "Fix tags from MusicBrainz" for one album: choose
 * the release, review the changes, write them with a backup, then Keep
 * (backup deleted) or Undo (tags restored, backup deleted). A fix that was
 * neither kept nor undone stays pending and is offered again the next time
 * the album's fix is opened. */
void gui_retag_open_album(const char * album, const char * album_artist);
/* The same for one CUE image (audio file + sheet), whatever its tags. */
void gui_retag_open_cue_image(const char * audio_path);
void gui_retag_teardown(void);
/* Lending the card: cancels a lookup. False while a lookup, a tag write or
 * an undo still runs (a write is never interrupted): call again. */
bool gui_retag_card_idle(void);

#endif
