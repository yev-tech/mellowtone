/* Host test tool for src/library/tag_writer.c (driven by run_tests.py).
 *
 *   test_tag_writer apply   FILE BACKUP|- KEY=VALUE...   (KEY= removes; mode on stderr)
 *   test_tag_writer fail    STAGE FILE BACKUP|- KEY=VALUE...
 *   test_tag_writer restore FILE BACKUP
 *   test_tag_writer mdread  FILE        (app reader: full + text-only paths)
 *   test_tag_writer unit                (argument validation unit tests)
 *
 * apply/fail/restore print the tag_write_result_t name and exit 0 when it is
 * TAG_WRITE_OK, 1 otherwise. Built with -DTAG_WRITER_TEST_HOOKS. */
#include "tag_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WITH_METADATA_READER
#include "metadata.h"

static void print_md(const char * mode, const track_metadata_t * m) {
    printf("%s.title=%s\n", mode, m->has_title ? m->title : "");
    printf("%s.artist=%s\n", mode, m->has_artist ? m->artist : "");
    printf("%s.album=%s\n", mode, m->has_album ? m->album : "");
    printf("%s.albumartist=%s\n", mode, m->has_album_artist ? m->album_artist : "");
    printf("%s.genre=%s\n", mode, m->has_genre ? m->genre : "");
    printf("%s.track=%d\n", mode, m->has_track_number ? m->track_number : 0);
    printf("%s.disc=%d\n", mode, m->has_disc_number ? m->disc_number : 0);
    printf("%s.year=%d\n", mode, m->has_release_year ? m->release_year : 0);
    printf("%s.picture=%u\n", mode, m->picture_data ? (unsigned) m->picture_size : 0u);
}
#endif

/* Peak RSS of this process (VmHWM is per address space, so it is not
 * inflated by the python parent that forked us). */
static void print_hwm(void) {
    FILE * st = fopen("/proc/self/status", "r");
    char line[256];
    while (st && fgets(line, sizeof(line), st))
        if (strncmp(line, "VmHWM:", 6) == 0) fprintf(stderr, "%s", line);
    if (st) fclose(st);
}

static int do_apply(const char * file, const char * backup, int argc, char ** argv) {
    tag_field_t * f = calloc((size_t) (argc > 0 ? argc : 1), sizeof(*f));
    for (int i = 0; i < argc; i++) {
        char * eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "bad field %s\n", argv[i]); return 2; }
        *eq = 0;
        f[i].key = argv[i];
        f[i].value = eq + 1;
    }
    tag_write_mode_t mode = (tag_write_mode_t) 99;
    tag_write_result_t r = tag_writer_apply_ex(file, f, argc, strcmp(backup, "-") == 0 ? NULL : backup, &mode);
    static const char * const names[] = { "NONE", "IN_PLACE", "FULL_COPY" };
    fprintf(stderr, "MODE: %s\n", (unsigned) mode < 3 ? names[mode] : "INVALID");
    printf("%s\n", tag_write_result_name(r));
    free(f);
    print_hwm();
    return r == TAG_WRITE_OK ? 0 : 1;
}

static int fails;
#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static int do_unit(const char * flac) {
    tag_field_t ok1[] = { { "TITLE", "x" } };
    EXPECT(tag_writer_supports("a/b/C.FLAC"));
    EXPECT(tag_writer_supports("x.mp3"));
    EXPECT(tag_writer_supports("x.Mp3"));
    EXPECT(!tag_writer_supports("x.ogg"));
    EXPECT(!tag_writer_supports("flac"));
    EXPECT(!tag_writer_supports(".flac"));
    EXPECT(!tag_writer_supports("dir.flac/x"));
    EXPECT(!tag_writer_supports(NULL));
    EXPECT(tag_writer_apply("x.ogg", ok1, 1, NULL) == TAG_WRITE_UNSUPPORTED);
    EXPECT(tag_writer_apply(NULL, ok1, 1, NULL) == TAG_WRITE_BAD_ARGS);
    EXPECT(tag_writer_apply(flac, ok1, 0, NULL) == TAG_WRITE_BAD_ARGS);
    EXPECT(tag_writer_apply(flac, NULL, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t unk[] = { { "COMMENT", "x" } };
    EXPECT(tag_writer_apply(flac, unk, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t alias[] = { { "ALBUM ARTIST", "x" } };
    EXPECT(tag_writer_apply(flac, alias, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t dup[] = { { "TITLE", "a" }, { "title", "b" } };
    EXPECT(tag_writer_apply(flac, dup, 2, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t bad_utf8[] = { { "TITLE", "\xC3\x28" } };
    EXPECT(tag_writer_apply(flac, bad_utf8, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t overlong[] = { { "TITLE", "\xC0\xAF" } };
    EXPECT(tag_writer_apply(flac, overlong, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t surrogate[] = { { "TITLE", "\xED\xA0\x80" } };
    EXPECT(tag_writer_apply(flac, surrogate, 1, NULL) == TAG_WRITE_BAD_ARGS);
    tag_field_t truncated[] = { { "TITLE", "ab\xE2\x82" } };
    EXPECT(tag_writer_apply(flac, truncated, 1, NULL) == TAG_WRITE_BAD_ARGS);
    char * big = malloc(70000);
    memset(big, 'a', 69999);
    big[69999] = 0;
    tag_field_t toolong[] = { { "TITLE", big } };
    EXPECT(tag_writer_apply(flac, toolong, 1, NULL) == TAG_WRITE_BAD_ARGS);
    free(big);
    tag_field_t nullkey[] = { { NULL, "x" } };
    EXPECT(tag_writer_apply(flac, nullkey, 1, NULL) == TAG_WRITE_BAD_ARGS);
    EXPECT(tag_writer_restore(flac, NULL) == TAG_WRITE_BAD_ARGS);
    EXPECT(tag_writer_restore("x.wav", "b") == TAG_WRITE_UNSUPPORTED);
    EXPECT(strcmp(tag_write_result_name(TAG_WRITE_BACKUP_FAILED), "TAG_WRITE_BACKUP_FAILED") == 0);
    EXPECT(strcmp(tag_write_result_name((tag_write_result_t) 99), "TAG_WRITE_UNKNOWN") == 0);
    printf(fails ? "UNIT FAIL\n" : "UNIT OK\n");
    return fails ? 1 : 0;
}

int main(int argc, char ** argv) {
    if (argc >= 3 && strcmp(argv[1], "unit") == 0) return do_unit(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "apply") == 0) return do_apply(argv[2], argv[3], argc - 4, argv + 4);
    if (argc >= 5 && strcmp(argv[1], "fail") == 0) {
        tag_writer_test_fail_at(atoi(argv[2]));
        return do_apply(argv[3], argv[4], argc - 5, argv + 5);
    }
    if (argc == 4 && strcmp(argv[1], "restore") == 0) {
        tag_write_result_t r = tag_writer_restore(argv[2], argv[3]);
        printf("%s\n", tag_write_result_name(r));
        return r == TAG_WRITE_OK ? 0 : 1;
    }
    if (argc == 5 && strcmp(argv[1], "failrestore") == 0) {
        tag_writer_test_fail_at(atoi(argv[2]));
        tag_write_result_t r = tag_writer_restore(argv[3], argv[4]);
        printf("%s\n", tag_write_result_name(r));
        return r == TAG_WRITE_OK ? 0 : 1;
    }
#ifdef WITH_METADATA_READER
    if (argc == 3 && strcmp(argv[1], "mdread") == 0) {
        track_metadata_t m;
        metadata_read(argv[2], &m);
        print_md("full", &m);
        free(m.picture_data);
        free(m.lyrics);
        metadata_read_without_artwork(argv[2], &m);
        print_md("text", &m);
        free(m.picture_data);
        free(m.lyrics);
        return 0;
    }
#endif
    fprintf(stderr, "usage: see source\n");
    return 2;
}
