#ifndef TAG_WRITER_H
#define TAG_WRITER_H

#include <stdbool.h>

/* Safe tag writer for FLAC (Vorbis comments) and MP3 (ID3v2.3 / ID3v2.4),
 * used by "Fix tags from MusicBrainz".
 *
 * The new tag region (FLAC: everything before the first audio frame; MP3: the
 * leading ID3v2 tag) is always built the same way; then one of two modes is
 * chosen automatically (tag_writer_apply_ex() reports which):
 *
 * IN-PLACE (used whenever the new region has exactly the old region's length,
 * i.e. the audio stays at the same offset)
 * ------------------------------------------------------------------------
 * Several region layouts are tried and the one changing the fewest bytes is
 * used. FLAC: (a) blocks stay put, a PADDING block adjacent to the comment
 * block absorbs its size change; (b) the comment block is rewritten in place
 * with all PADDING merged at the end; (c) the old comment block's slot becomes
 * PADDING and the new comment block is written into an existing PADDING block
 * (so a large PICTURE between them never moves). MP3: replaced frames in
 * their old position, or kept frames first and new frames appended; the tag
 * is padded to exactly its old size. Then:
 *   1. the changed byte spans are computed by comparing the new region with
 *      the file (byte-exact; differences < 512 bytes apart are joined, at most
 *      16 spans); no difference -> TAG_WRITE_OK with mode NONE:
 *      nothing is written and NO backup file is created (an existing file at
 *      backup_path is left alone; the caller may delete unused backups);
 *   2. a version-2 backup (old + new bytes of the spans, see below) is made durable,
 *      else TAG_WRITE_BACKUP_FAILED with the file untouched;
 *   3. the file is re-checked unchanged (size/mtime/inode), opened O_RDWR (no
 *      truncation), the spans pwrite()n and fsync()ed;
 *   4. verification from the card (cache dropped): the spans read back, the
 *      file size is unchanged, the region re-parses with the planned block /
 *      frame sequence and the requested fields read back exactly, and the
 *      sampled audio hash is unchanged. On failure the old bytes are written
 *      back at once (fsync, re-read) -> TAG_WRITE_VERIFY_FAILED (TAG_WRITE_IO_ERROR
 *      if even that roll-back failed; tag_writer_restore() with the backup
 *      then repairs it).
 * Not atomic: a power cut during step 3 can leave torn spans; restore() from
 * the backup repairs it byte-exactly. Free space needed: only the backup.
 * The audio is not re-read in full in this mode: the "sampled" audio hash is
 * FNV-1a 64 over the audio length (u64 LE) + the first and last
 * min(A, 1 MiB) audio bytes (the audio is never written, so this only guards
 * against restoring onto a different/re-encoded file).
 *
 * FULL COPY (when the region grows or shrinks)
 * --------------------------------------------
 * The original is only opened read-only. The complete new file is written to
 * a temp file in the same directory (".<name>.tagtmp"), fsync()ed, its page
 * cache dropped (posix_fadvise DONTNEED) and then re-read in full:
 *   - the whole file must hash (FNV-1a 64) to what was written,
 *   - the audio payload (everything after the tag region, including any
 *     ID3v1/APE trailer) must have the same length and hash as the original's,
 *   - the tag region must re-parse, and every requested field must read back
 *     exactly (new Vorbis comment block / new ID3 frames byte-compared and
 *     decoded), with every untouched block/frame kept in order.
 * Just before rename() the original is re-stat()ed: if its size, mtime or
 * inode changed during the operation, nothing is replaced. Only then is the
 * temp renamed over the original and the directory fsync()ed (best effort).
 * Any failure unlinks the temp file and leaves the original untouched.
 * Costs: one full read + write plus one full verification read; free space
 * for one extra copy of the file. Writes a version-1 backup.
 *
 * Memory (both modes): a 64-128 KiB buffer plus the Vorbis comment block / the
 * few ID3 frames that have to be decoded and the changed span (pictures are
 * stream-copied or compared in 64 KiB pieces, never loaded whole).
 *
 * Threading: no global state; any thread may call, one call per file at a
 * time. Do not run it on a file that is currently open for playback on the
 * device's exFAT driver (full copy unlinks the old inode by the rename; in
 * place rewrites tag bytes under the reader).
 *
 * Field keys (case-insensitive on input, written upper-case in FLAC):
 *   ALBUM, ALBUMARTIST, ARTIST, TITLE, TRACKNUMBER, TRACKTOTAL, DISCNUMBER,
 *   DISCTOTAL, DATE, GENRE, MUSICBRAINZ_ALBUMID, MUSICBRAINZ_ALBUMARTISTID,
 *   MUSICBRAINZ_ARTISTID, MUSICBRAINZ_TRACKID (recording id),
 *   MUSICBRAINZ_RELEASETRACKID.
 * value NULL or "" removes the field. Unknown keys, a key given twice,
 * invalid UTF-8 or a value longer than 64 KiB -> TAG_WRITE_BAD_ARGS.
 *
 * FLAC: the VORBIS_COMMENT block is rebuilt keeping the vendor string and all
 *   other comments in their original order. The first comment with a matching
 *   key (case-insensitive) is replaced in place, further instances and these
 *   aliases are removed: ALBUMARTIST <- "ALBUM ARTIST", "ALBUM_ARTIST";
 *   DISCNUMBER <- DISKNUMBER; TRACKTOTAL <- TOTALTRACKS; DISCTOTAL <-
 *   TOTALDISCS. New keys are appended. Without a VORBIS_COMMENT block one is
 *   inserted after STREAMINFO. All other blocks are copied verbatim; PADDING
 *   blocks are merged into one final PADDING block sized so the audio frames
 *   keep their offset when the new metadata fits (else 4 KiB of new padding).
 *   A leading ID3v2 tag before "fLaC" is kept byte-for-byte.
 * MP3: ID3v2.3/2.4 edited in its own version (no tag -> new v2.4 tag); v2.2,
 *   global unsynchronisation, an extended header or unknown header flags ->
 *   TAG_WRITE_UNSUPPORTED. Untouched frames are copied byte-for-byte
 *   (flags included). Replaced frames keep the position of the first frame
 *   they replace. Mapping (Picard conventions):
 *     ALBUM TALB, ALBUMARTIST TPE2, ARTIST TPE1, TITLE TIT2, GENRE TCON,
 *     TRACKNUMBER/TRACKTOTAL TRCK "n/N", DISCNUMBER/DISCTOTAL TPOS "n/N"
 *       (the missing half is merged from the existing frame; an explicitly
 *       given total without a number -> TAG_WRITE_BAD_ARGS; removing the number
 *       also drops a total kept from the old frame; '/' in a value -> BAD_ARGS),
 *     DATE: v2.4 TDRC (TYER/TDAT/TIME/TRDA removed), v2.3 TYER with the
 *       first 4 characters, which must be digits (TDAT/TIME/TRDA/TDRC removed),
 *     MUSICBRAINZ_ALBUMID         TXXX "MusicBrainz Album Id",
 *     MUSICBRAINZ_ALBUMARTISTID   TXXX "MusicBrainz Album Artist Id",
 *     MUSICBRAINZ_ARTISTID        TXXX "MusicBrainz Artist Id",
 *     MUSICBRAINZ_RELEASETRACKID  TXXX "MusicBrainz Release Track Id",
 *     MUSICBRAINZ_TRACKID         UFID owner "http://musicbrainz.org" (<= 64 bytes).
 *   Text is written as UTF-8 (0x03) in v2.4, UTF-16 with BOM (0x01) in v2.3
 *   (TYER as ISO-8859-1 digits). The footer flag of a v2.4 tag is dropped.
 *   If the new frames fit in the old tag, the tag keeps its size (audio offset
 *   unchanged); otherwise 1 KiB of padding is added. ID3v1/APE trailers and
 *   everything after the ID3v2 tag are kept byte-for-byte.
 *
 * Backup files (all integers little-endian; restore() reads both versions)
 * --------------------------------------------------------------------------
 * Version 1 (full-copy mode):
 *   off  size  field
 *     0     8  magic: the bytes 'C','T','W','B','A','K','\r','\n'
 *     8     4  version = 1
 *    12     4  format: 1 = FLAC, 2 = MP3
 *    16     8  original file size S
 *    24     8  tag region length R (FLAC: every byte before the first audio
 *              frame, incl. a leading ID3v2 tag, "fLaC" and all metadata
 *              blocks; MP3: the leading ID3v2 tag incl. header, footer and
 *              padding, 0 if the file had none)
 *    32     8  audio payload length A = S - R
 *    40     8  FNV-1a 64 hash of the A audio payload bytes
 *    48     R  the original tag region bytes
 *  48+R     8  FNV-1a 64 hash of bytes [0, 48+R) of this backup file
 *  restore(): writes R + the current audio via the full-copy procedure; the
 *  current audio must have length A and hash.
 * Version 2 (in-place mode):
 *     0     8  magic (as above)
 *     8     4  version = 2
 *    12     4  format: 1 = FLAC, 2 = MP3
 *    16     4  mode = 1 (in-place)
 *    20     4  audio hash kind = 1 (sampled, see IN-PLACE above)
 *    24     8  file size S (unchanged by the write)
 *    32     8  tag region length R (unchanged by the write)
 *    40     8  audio length A = S - R
 *    48     8  sampled audio hash
 *    56     4  span count N (1..16)
 *    60     4  reserved, 0
 *    64  24*N  per span, ascending and disjoint: offset O_i (u64), length
 *              L_i (u64, > 0), FNV-1a 64 of the span's NEW bytes (u64)
 *     .   sum  ORIGINAL bytes of all spans, concatenated in span order
 *     .   sum  NEW bytes of all spans, concatenated in span order
 *  end-8    8  FNV-1a 64 hash of every preceding byte of this backup file
 *  Several spans are used because the changed bytes need not be contiguous
 *  (e.g. old comment block -> PADDING, new comment block written into the
 *  padding after an unchanged PICTURE); differences less than 512 bytes
 *  apart share one span. The whole span set is at most 32 MiB.
 *  restore(): size must be S and the sampled audio hash match; if the spans
 *  already hold the original bytes -> OK (nothing written); if every byte
 *  of the current spans equals the original or the new byte at that position
 *  (complete or torn write of ours) the original bytes are pwrite()n, fsync()ed
 *  and re-read; anything else -> TAG_WRITE_VERIFY_FAILED, untouched.
 * The backup is written to "<backup_path>.tmp", fsync()ed, re-read and checked,
 * then renamed over backup_path; an existing backup is only replaced once the
 * new one is durable. Callers that want to keep the very first state must not
 * reuse a backup_path across several apply() calls on the same file.
 */

typedef struct {
    const char * key;
    const char * value;
} tag_field_t;

typedef enum {
    TAG_WRITE_OK,
    TAG_WRITE_UNSUPPORTED,     /* not .flac/.mp3, not a regular file, or a tag feature we refuse to touch */
    TAG_WRITE_PARSE_ERROR,     /* malformed file or backup */
    TAG_WRITE_IO_ERROR,        /* read/write/fsync/rename failure, no space, file changed meanwhile */
    TAG_WRITE_VERIFY_FAILED,   /* read-back mismatch, or (restore) audio differs from the backup's */
    TAG_WRITE_BAD_ARGS,
    TAG_WRITE_BACKUP_FAILED,
} tag_write_result_t;

/* True for paths ending in .flac or .mp3 (case-insensitive). */
bool tag_writer_supports(const char * path);

/* Sets/replaces/removes the given fields (see above). If backup_path is
 * non-NULL the original bytes about to change are saved there (durably)
 * before the file is modified; failing that -> TAG_WRITE_BACKUP_FAILED and
 * the file is untouched. When nothing would change, OK is returned and no
 * backup is written. Same as tag_writer_apply_ex(..., NULL). */
tag_write_result_t tag_writer_apply(const char * path, const tag_field_t * fields, int count, const char * backup_path);

/* Undoes tag_writer_apply() from its backup (v1: full-copy procedure; v2:
 * in-place span write, see the backup formats above). Refuses
 * (TAG_WRITE_VERIFY_FAILED, file untouched) when the audio no longer matches
 * the backup, so a successful restore is byte-identical to the pre-write file.
 * A corrupt/foreign backup -> TAG_WRITE_PARSE_ERROR; a backup of another
 * format than the file's extension -> TAG_WRITE_BAD_ARGS. */
tag_write_result_t tag_writer_restore(const char * path, const char * backup_path);

typedef enum {
    TAG_WRITE_MODE_NONE,      /* nothing written (values already as requested, or an error) */
    TAG_WRITE_MODE_IN_PLACE,  /* only the changed byte span of the tag region was rewritten */
    TAG_WRITE_MODE_FULL_COPY, /* temp file + verify + rename */
} tag_write_mode_t;

/* tag_writer_apply() that also reports the mode used (out_mode may be NULL;
 * it is TAG_WRITE_MODE_NONE whenever the result is not TAG_WRITE_OK). */
tag_write_result_t tag_writer_apply_ex(const char * path, const tag_field_t * fields, int count,
                                       const char * backup_path, tag_write_mode_t * out_mode);

/* Stable upper-case name, e.g. "TAG_WRITE_OK". */
const char * tag_write_result_name(tag_write_result_t r);

#ifdef TAG_WRITER_TEST_HOOKS
/* Test builds only: make the next operations fail at a stage.
 * 0 = off, 1 = audio copy write, 2 = verification, 3 = rename,
 * 4 = backup write, 5 = treat the source as modified before rename,
 * 6 = in-place: fail after the backup, before writing,
 * 7 = in-place: write half the new bytes, fsync, then fail without roll-back (torn write),
 * 8 = in-place: fail verification (automatic roll-back). */
void tag_writer_test_fail_at(int stage);
#endif

#endif /* TAG_WRITER_H */
