#ifndef DLNA_CLIENT_H
#define DLNA_CLIENT_H

/* Minimal UPnP AV / DLNA ContentDirectory client:
 * SSDP discovery of media servers on the local network, the device
 * description (name, ContentDirectory control URL), and Browse of one
 * container a page at a time. Playback uses the plain HTTP links the server
 * returns, through the same stream path as Subsonic.
 *
 * Every call blocks; run them on a worker thread. Not thread-safe across
 * concurrent callers only for dlna_last_error(). */

#include <stdbool.h>
#include <stdint.h>

#include "http_client.h"

typedef struct {
    char location[512];    /* device description URL; the saved key */
    char udn[96];          /* uuid:..., finds the server again after an address change */
    char name[96];         /* friendlyName */
    char model[96];        /* modelName, else manufacturer */
    char control_url[512]; /* absolute ContentDirectory control URL */
} dlna_server_t;

typedef enum { DLNA_CODEC_NONE = 0, DLNA_CODEC_FLAC, DLNA_CODEC_MP3, DLNA_CODEC_AAC } dlna_codec_t;

typedef struct {
    char id[128];
    char title[128];
    char artist[128];
    char album[128];
    char art_url[512];
    char res_url[1024]; /* the best playable resource, empty for containers */
    char mime[48];      /* of res_url, or of the first audio resource when none is playable */
    dlna_codec_t codec; /* NONE: a format this player can't stream */
    uint32_t duration_ms;
    unsigned sample_rate, bits, channels;
    int track_no;    /* -1 unknown */
    int child_count; /* containers; -1 unknown */
    bool is_container;
    bool is_album; /* object.container.album.* */
    bool is_audio; /* object.item.audioItem.* */
} dlna_object_t;

/* Sends M-SEARCH for media servers and collects answers for timeout_ms, then
 * reads each one's description. *out is malloc()ed (free it); returns the
 * count (0 when nothing answered). */
int dlna_discover(dlna_server_t ** out, int timeout_ms, http_cancel_token_t * cancel);

/* Reads a device description. location may also be just "http://host:port",
 * in which case the usual description paths are tried. */
bool dlna_describe(const char * location, dlna_server_t * out, http_cancel_token_t * cancel);

/* BrowseDirectChildren of object_id ("0" = root). *out is malloc()ed (free
 * it, may be NULL when empty); *total is the server's TotalMatches. */
bool dlna_browse(const dlna_server_t * server, const char * object_id, int start, int count, dlna_object_t ** out,
                 int * n, int * total, http_cancel_token_t * cancel);

const char * dlna_last_error(void);

/* "mp3" / "flac" / "aac" for remote_track_meta_t.codec, "" for NONE. */
const char * dlna_codec_name(dlna_codec_t c);

#endif
