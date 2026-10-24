#ifndef LRCLIB_H
#define LRCLIB_H

/* Synced-lyrics lookup on LRCLIB (lrclib.net: free, no key). */
typedef enum {
    LRCLIB_OK,          /* .lrc sidecar written next to the track */
    LRCLIB_OK_PLAIN,    /* unsynced lyrics written as a timestamp-free .lrc */
    LRCLIB_NOT_FOUND,
    LRCLIB_PLAIN_ONLY,  /* only unparseable synced text; nothing written */
    LRCLIB_NET_ERROR,
    LRCLIB_WRITE_ERROR,
    LRCLIB_EXISTS,      /* a sidecar is already there; left untouched */
    LRCLIB_BUSY,        /* LRCLIB answered with an error (rate limit, server error) */
} lrclib_result_t;

/* Blocking: run it off the UI thread. duration_s <= 0 when unknown. */
lrclib_result_t lrclib_fetch_sidecar(const char * track_path, const char * title, const char * artist,
                                     const char * album, int duration_s);

#endif
