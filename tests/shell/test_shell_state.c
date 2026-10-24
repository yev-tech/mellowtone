/* What the shell re-applies after a UI reload (shell_state.c): the cached
 * now-playing card survives being handed back to its own setter (the reload
 * path does exactly that), clears on "nothing loaded", truncates long text,
 * and the status-bar Bluetooth glyph follows power and connection. */
#include "shell_state.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static void test_track_round_trip(void) {
    shell_now_playing_t np;
    memset(&np, 0, sizeof(np));
    shell_now_playing_set_track(&np, "Ausländer", "Rammstein", "Rammstein");
    CHECK(np.has_track, "a title means a track");
    CHECK(strcmp(np.title, "Ausländer") == 0, "title '%s'", np.title);

    /* Re-applying the cache passes its own fields back in. */
    shell_now_playing_set_track(&np, np.title, np.artist, np.album);
    CHECK(np.has_track, "still a track after re-applying");
    CHECK(strcmp(np.title, "Ausländer") == 0, "title after re-apply '%s'", np.title);
    CHECK(strcmp(np.artist, "Rammstein") == 0, "artist after re-apply '%s'", np.artist);
    CHECK(strcmp(np.album, "Rammstein") == 0, "album after re-apply '%s'", np.album);

    /* Fields may also feed each other. */
    shell_now_playing_set_track(&np, np.artist, np.title, NULL);
    CHECK(strcmp(np.title, "Rammstein") == 0, "swapped title '%s'", np.title);
    CHECK(strcmp(np.artist, "Rammstein") == 0, "swapped artist '%s'", np.artist);
    CHECK(np.album[0] == '\0', "NULL album is empty, got '%s'", np.album);

    shell_now_playing_set_format(&np, "FLAC 44.1 kHz/16 bit");
    shell_now_playing_set_format(&np, np.format);
    CHECK(strcmp(np.format, "FLAC 44.1 kHz/16 bit") == 0, "format after re-apply '%s'", np.format);
}

static void test_nothing_loaded(void) {
    shell_now_playing_t np;
    memset(&np, 0, sizeof(np));
    shell_now_playing_set_track(&np, "Sonne", "Rammstein", "Mutter");
    shell_now_playing_set_track(&np, NULL, NULL, NULL);
    CHECK(!np.has_track, "NULL title means nothing loaded");
    CHECK(np.title[0] == '\0' && np.artist[0] == '\0' && np.album[0] == '\0', "texts cleared");

    /* An empty title is still a loaded track; Home decides how to show it. */
    shell_now_playing_set_track(&np, "", "", "");
    CHECK(np.has_track, "empty title is still a track");
}

static void test_truncation(void) {
    shell_now_playing_t np;
    memset(&np, 0, sizeof(np));
    char longtext[600];
    memset(longtext, 'x', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = '\0';
    shell_now_playing_set_track(&np, longtext, longtext, longtext);
    CHECK(strlen(np.title) == sizeof(np.title) - 1, "title length %zu", strlen(np.title));
    shell_now_playing_set_format(&np, longtext);
    CHECK(strlen(np.format) == sizeof(np.format) - 1, "format length %zu", strlen(np.format));
    shell_now_playing_set_track(&np, np.title, np.artist, np.album);
    CHECK(strlen(np.title) == sizeof(np.title) - 1, "re-applied long title length %zu", strlen(np.title));
}

static void test_bt_glyph(void) {
    CHECK(shell_bt_status_glyph(false, false) == NULL, "radio off hides the icon");
    CHECK(shell_bt_status_glyph(false, true) == NULL, "radio off hides the icon even with a stale link");
    const char * idle = shell_bt_status_glyph(true, false);
    const char * linked = shell_bt_status_glyph(true, true);
    CHECK(idle && strcmp(idle, "topbar/lucide_bluetooth_off.png") == 0, "powered, nothing connected: %s",
          idle ? idle : "(null)");
    CHECK(linked && strcmp(linked, "topbar/m_bt.png") == 0, "connected: %s", linked ? linked : "(null)");
}

int main(void) {
    test_track_round_trip();
    test_nothing_loaded();
    test_truncation();
    test_bt_glyph();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("shell_state: all passed\n");
    return 0;
}
