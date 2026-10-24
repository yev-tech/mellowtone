#ifndef CUE_PARSER_H
#define CUE_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/* One TRACK entry from a .cue sheet -- title/performer default to empty
 * strings (not every track, or every sheet, sets both), start_seconds is
 * always set (falls back to 0.0 for the first track if the sheet somehow
 * omits INDEX 01, matching how a real player would still want SOME start
 * point rather than dropping the track). */
typedef struct {
    int number;
    char title[128];
    char performer[128];
    double start_seconds;
    bool has_index_01;
} cue_track_t;

/* A parsed .cue sheet -- single-FILE sheets only (the overwhelming common
 * case: one big lossless rip of a whole album/disc, split into tracks by
 * INDEX time rather than separate files). A sheet with more than one FILE
 * command is rejected outright (cue_parse_file() returns false) rather than
 * silently mis-attributing tracks from the second FILE onward to the first
 * -- see cue_parser.c's own comment on why this is the right call for now. */
typedef struct {
    char audio_path[600]; /* resolved: the .cue's own directory + FILE's own filename */
    cue_track_t * tracks; /* caller-owned, sorted by INDEX 01 time ascending -- see cue_parser.c */
    int track_count;
    /* Album level (before the first TRACK): TITLE, PERFORMER, REM DATE, REM GENRE; "" when absent. */
    char title[128];
    char performer[128];
    char date[16];
    char genre[64];
} cue_sheet_t;

/* Parses cue_path (already resolved, real path). On success, out->audio_path
 * points at the referenced audio file (existence NOT verified here -- see
 * cue_parser.c's own comment); out->tracks is a freshly malloc'd array the
 * caller owns, validated in strictly ascending start-time order, always at least 1 entry when true is
 * returned. Returns false (out left zeroed) on a missing/unreadable file, a
 * sheet with no FILE command, no TRACK AUDIO entries, or more than one FILE
 * command (see cue_sheet_t's own comment). Text that is not valid UTF-8 is
 * read as Windows-1251 when it looks Cyrillic, else Windows-1252 (what ripping tools on Windows write). */
bool cue_parse_file(const char * cue_path, cue_sheet_t * out);

/* The same from sheet text. cue_path resolves FILE relative to its folder;
 * audio_path, when set, is used as out->audio_path instead (an embedded
 * sheet always describes the file that carries it). */
bool cue_parse_text(const char * text, size_t len, const char * cue_path, const char * audio_path, cue_sheet_t * out);

/* Frees out->tracks and zeroes the struct -- safe on an already-zeroed or
 * partially-parsed (cue_parse_file() returned false) cue_sheet_t. */
void cue_sheet_free(cue_sheet_t * out);

typedef enum {
    CUE_SOURCE_NONE = 0,
    CUE_SOURCE_FILE,           /* a .cue in the same folder whose FILE is this audio file */
    CUE_SOURCE_EMBEDDED_TEXT,  /* FLAC: a CUESHEET Vorbis comment (foobar2000, EAC, CUETools) */
    CUE_SOURCE_EMBEDDED_BLOCK, /* FLAC: only a CUESHEET metadata block (track times, no names) */
} cue_source_t;

/* Finds the sheet describing audio_path, in the order above; only sheets
 * with at least two tracks count. When cue_path is given it receives the
 * .cue's path (CUE_SOURCE_FILE) or "". When text_out is given it receives
 * the sheet as UTF-8 text (malloc'd; for a block-only sheet a generated
 * one), for cue_rewrite(). */
cue_source_t cue_find_for_audio(const char * audio_path, cue_sheet_t * out, char * cue_path, size_t cue_path_size,
                                char ** text_out);

/* Every .cue beside audio_path whose FILE is it (some rips carry two, e.g.
 * one with ReplayGain lines); returns the count. */
int cue_sheets_for_audio(const char * audio_path, char paths[][1024], int max);

/* Changes for cue_rewrite(): NULL leaves a value as it is. Track arrays are
 * indexed by AUDIO track order (as in cue_sheet_t.tracks). */
typedef struct {
    const char * title;
    const char * performer;
    const char * date;
    const char * genre;
    const char * const * track_titles;
    const char * const * track_performers;
    int track_count;
} cue_edit_t;

/* The sheet text with only the edited lines changed (missing ones are
 * added: album lines before FILE, track lines after their TRACK), line
 * endings and every other line kept. text must be UTF-8 (cue_find_for_audio
 * converts). Returns malloc'd text, NULL on failure. */
char * cue_rewrite(const char * text, const cue_edit_t * edit);

/* Text as Windows-1252 when it is not valid UTF-8, converted to UTF-8
 * (malloc'd, NUL-terminated; *converted tells whether it changed). */
char * cue_text_to_utf8(const char * text, size_t len, bool * converted);

#endif /* CUE_PARSER_H */
