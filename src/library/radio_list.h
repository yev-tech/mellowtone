#ifndef RADIO_LIST_H
#define RADIO_LIST_H

#include <stdbool.h>
#include <stddef.h>

/* Radio stations.
 *
 *   MT_RADIO_BASE_FILE    install root, read-only base list
 *   MT_RADIO_SD_FILE      <card>/Radio.txt: your additions (a base station
 *                         listed there with another name renames it)
 *   MT_RADIO_HIDDEN_FILE  config/radio_hidden.txt: base stations you removed
 *
 * What you see: base stations minus the hidden ones, then your additions,
 * without duplicates (stations are the same when their normalized URLs are).
 * Without a card the base list plays read-only.
 *
 * File format (both lists): "Station Name | http://stream-url" per line; a
 * bare http(s) URL is a station named after its URL; blank lines and
 * # comments are ignored. The hidden list holds one URL per line. */

#define RADIO_URL_MAX 1024
#define RADIO_NAME_MAX 256

typedef struct {
    char * name;
    char * url;
    bool base;   /* from the base list (removing it hides it) */
    bool in_sd;  /* listed in the card's Radio.txt (as an addition or a rename) */
    bool hidden; /* only in radio_list_load(..., true) results */
} radio_station_t;

typedef struct {
    radio_station_t * items;
    int count;
    int cap;
} radio_entries_t;

void radio_entries_free(radio_entries_t * list);

/* Parses a Radio.txt-style file. Returns false when it cannot be read. */
bool radio_entries_parse_file(const char * path, radio_entries_t * out);

/* Lower-case scheme and host, default port dropped, surrounding blanks and a
 * trailing '/' dropped. */
void radio_url_normalize(const char * url, char * out, size_t out_size);
bool radio_url_is_http(const char * url);

/* The merged view from explicit files (sd_path, hidden_path may be NULL). */
bool radio_list_build(const char * base_path, const char * sd_path, const char * hidden_path,
                      bool include_hidden, radio_entries_t * out);

/* ---- the player's list (MT_* paths) ----------------------------------- */

/* Thread-safe. include_hidden adds the hidden base stations, flagged. */
bool radio_list_load(radio_entries_t * out, bool include_hidden);
int radio_list_hidden_count(void);
/* The card can take changes (a writable card is mounted). */
bool radio_list_editable(void);

typedef enum {
    RADIO_OK = 0,
    RADIO_ADDED_RESTORED, /* a removed base station: shown again */
    RADIO_EXISTS,
    RADIO_NOT_FOUND,
    RADIO_NO_CARD,        /* no writable card */
    RADIO_BAD_URL,
    RADIO_IO_ERROR,
} radio_result_t;

radio_result_t radio_list_add(const char * name, const char * url);
/* A card station is deleted from Radio.txt; a base station is hidden. */
radio_result_t radio_list_remove(const char * url);
/* Shows a hidden base station again. */
radio_result_t radio_list_restore(const char * url);
const char * radio_result_text(radio_result_t result);

#endif
