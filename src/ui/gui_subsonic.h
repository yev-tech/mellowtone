#pragma once
#include <lvgl/lvgl.h>

typedef struct {
    char url[1536]; /* the exact playlist[i] this entry describes */
    char title[128];
    char artist[128];
    char album[128];
    char cover_url[1536];
    bool verify_tls;
    char suffix[16];
    int track;
    int disc;
    int duration_seconds;
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
} subsonic_stream_song_meta_t;


void gui_subsonic_init(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_subsonic_init() again from a clean slate. */
void gui_subsonic_teardown(void);
extern subsonic_stream_song_meta_t * subsonic_stream_meta;
extern int subsonic_stream_meta_count;
extern bool subsonic_library_download_active;
extern bool subsonic_connect_active;
extern void subsonic_tile_cb(lv_event_t * e);

extern subsonic_stream_song_meta_t * subsonic_stream_meta;

void poll_subsonic_download(void);
void poll_subsonic_library_download(void);
void poll_subsonic_connect(void);
void poll_subsonic_browse(void);

bool gui_subsonic_has_background_work(void);
void gui_subsonic_handle_wifi_disabled(void);
void gui_subsonic_cancel_background_work(void);

/* Entry points for the stream home (gui_stream_home.c) and Settings >
 * Streaming. The active server is the one in current_settings. */
#include "subsonic_client.h"
subsonic_server_t gui_subsonic_active_server(void);
/* Makes `server` the active one (saved in settings); no network request. */
void gui_subsonic_activate_server(const subsonic_server_t * server);
bool gui_subsonic_busy(void);
/* Opens the album's song list; autoplay_index >= 0 also starts that track. */
void gui_subsonic_open_album(const char * id, const char * title, int autoplay_index);
void gui_subsonic_open_artists(void);
void gui_subsonic_open_playlists(void);
void gui_subsonic_open_quality(void);
void gui_subsonic_open_download_settings(void);
/* Plays `songs` (ownership taken) from `start`, queueing the rest. */
void gui_subsonic_play_songs(subsonic_song_t * songs, int count, int start);
/* The last album streamed from the active server, for "Continue". */
bool gui_subsonic_continue_info(char * album_id, size_t id_size, char * title, size_t title_size,
                                char * artist, size_t artist_size, char * cover, size_t cover_size, int * track);
