/* Matching a local album against a MusicBrainz release, and the tag
 * changes that follow from it. Pure logic: no network, no LVGL, no I/O. */
#ifndef RETAG_MATCH_H
#define RETAG_MATCH_H

#include <stdbool.h>
#include <stddef.h>

#include "musicbrainz.h"

typedef struct {
    const char * path;
    const char * title;          /* current tags, may be "" */
    const char * artist;
    const char * album;
    const char * album_artist;   /* "" when the file has none */
    int track_number;            /* -1 unknown */
    int disc_number;             /* -1 unknown */
    int year;                    /* -1 unknown */
    const char * genre;          /* may be "" or "Unknown Genre" */
    double duration_s;           /* 0 if unknown */
} retag_local_t;

typedef struct {
    int score;                   /* 0..100 overall confidence */
    int matched;                 /* files mapped to a track */
    int * track_for_file;        /* malloc'd [file_count], index into detail->tracks or -1 */
} retag_match_t;

/* Score how well `detail` fits the local files and map files to tracks.
 *
 * Per file/track pair: duration agreement (within 3 s is a full match,
 * fading to nothing at 10 s; weighted most), normalised title similarity
 * (case, punctuation and diacritics ignored, "(Remastered 2011)",
 * "- Bonus Track", "(feat. X)"-style suffixes and a leading "01 - " are
 * dropped, then a character-bigram Dice coefficient), the track artist
 * when the file names one other than the album artist (tells apart a
 * compilation's two "99 Luftballons"), and the file's track/disc numbers
 * when it has them. Within 3 s, the closer duration wins a tie.
 *
 * Mapping: with equal counts, files in (disc, track number, input) order
 * are mapped by position when at least 90 % of the known durations agree
 * (or, with no durations at all, when position scores as well as the
 * greedy mapping); otherwise a greedy best-pair assignment that leaves a
 * file unmapped (-1) rather than accept a weak pair. No two files share a
 * track.
 *
 * Overall score: track-count agreement, coverage, mean title similarity
 * and album-title similarity, then scaled by the share of durations that
 * agree, so a release of different cuts (same titles, different lengths)
 * scores low. As a guide: >= 85 same release, 60..85 likely (check the
 * edition), < 60 probably a different release or cut. */
void retag_match_score(const retag_local_t * files, int file_count, const mb_release_detail_t * detail,
                       retag_match_t * out);
void retag_match_free(retag_match_t * m);

/* Proposed changes for the review screen. */
typedef enum { RETAG_FIELD_ALBUM, RETAG_FIELD_ALBUMARTIST, RETAG_FIELD_ARTIST, RETAG_FIELD_TITLE, RETAG_FIELD_TRACK,
               RETAG_FIELD_DISC, RETAG_FIELD_DATE, RETAG_FIELD_GENRE, RETAG_FIELD_MBIDS, RETAG_FIELD_COUNT } retag_field_t;

typedef struct {
    retag_field_t field;
    int files_changed;           /* how many files get a different value */
    char old_example[128];       /* a representative current value ("" if empty, "mixed" if files differ) */
    char new_example[128];       /* representative new value, e.g. "Various Artists", "1–16 of 16", "2017-07-21" */
    bool default_on;             /* pre-ticked: true when old is empty/"Unknown…"/clearly wrong (e.g. missing album
                                    artist, missing track numbers or year, album title differing between files);
                                    false for titles and when the old value is merely spelled differently */
} retag_change_t;

/* Builds the summary rows (only fields with files_changed > 0), in
 * retag_field_t order; returns the row count. Only mapped files count.
 * Comparison rules match retag_fields_for_file(): DATE compares the year
 * only, TRACK compares the track number, MBIDS always counts (the local
 * tags' MBIDs are not known). */
int retag_summarize(const retag_local_t * files, int file_count, const mb_release_detail_t * detail,
                    const retag_match_t * m, retag_change_t * out, int max);

/* Key/value pair for tag_writer. Layout-identical to tag_writer.h's
 *   typedef struct { const char * key; const char * value; } tag_field_t;
 * so an array of one can be passed as the other with a pointer cast. */
typedef struct {
    const char * key;
    const char * value;
} retag_kv_t;

/* Per-file field list for tag_writer: keys ALBUM, ALBUMARTIST, ARTIST, TITLE, TRACKNUMBER, TRACKTOTAL, DISCNUMBER,
 * DISCTOTAL, DATE, GENRE, MUSICBRAINZ_ALBUMID, MUSICBRAINZ_ALBUMARTISTID, MUSICBRAINZ_ARTISTID,
 * MUSICBRAINZ_TRACKID, MUSICBRAINZ_RELEASETRACKID -- only for enabled fields (enabled[RETAG_FIELD_*]) and only
 * values that differ.
 *  - TRACKTOTAL (tracks on the file's medium) goes with TRACKNUMBER; DISCNUMBER/DISCTOTAL only when
 *    detail->disc_count > 1.
 *  - DATE is the release date as MusicBrainz has it ("YYYY[-MM[-DD]]"), written when the year differs.
 *  - MUSICBRAINZ_TRACKID is the recording MBID and MUSICBRAINZ_RELEASETRACKID the track MBID (Picard's
 *    convention); the artist IDs are the first credited artist's.
 *  - track == NULL yields the album-level fields only (ALBUM, ALBUMARTIST, DATE, GENRE, album MBIDs).
 * Keys are static strings; values are copied into `buf` (size `buf_size`), so they stay valid after
 * `detail` is freed. Returns the number of fields, or -1 if buf or max_fields is too small. */
int retag_fields_for_file(const retag_local_t * file, const mb_release_detail_t * detail, const mb_track_t * track,
                          const bool enabled[RETAG_FIELD_COUNT], char * buf, size_t buf_size,
                          retag_kv_t * out_fields, int max_fields);

/* Normalises a title for comparison (exposed for tests): lower-case ASCII,
 * Latin diacritics folded, punctuation dropped, version suffixes removed. */
void retag_normalize_title(const char * in, char * out, size_t out_size);
/* 0..1 similarity of two titles after retag_normalize_title(). */
double retag_title_similarity(const char * a, const char * b);

#endif /* RETAG_MATCH_H */
