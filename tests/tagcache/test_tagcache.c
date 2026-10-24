/* tagcache on-disk formats: a library this engine wrote opens and answers
 * queries; databases in an older or foreign format (Rockbox master without
 * the indexed magic, index meta v3, query postings v1, a generation without
 * its refs manifest, no persisted indexes at all) are rejected by
 * tagcache_open() and rebuilt cleanly through tagcache_open_for_rebuild(). */
#include "board.h"
#include "db_log.h"
#include "tagcache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* No spool directory: staging files go next to the database. No logging. */
const board_desc_t BOARD = {0};
bool db_log_enabled(void) { return false; }
void db_log(const char *area, const char *fmt, ...) {
    (void)area;
    (void)fmt;
}

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define SONG_COUNT 3
static const char *const titles[SONG_COUNT] = {"Alpha", "Bravo", "Charlie"};
static const char *const artists[SONG_COUNT] = {"First Artist", "Second Artist", "First Artist"};
static const char *const albums[SONG_COUNT] = {"One", "Two", "One"};

static char root[256];

static void make_dir(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", root, name);
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        perror(out);
        exit(1);
    }
}

static void song_path(char *out, size_t size, int i) {
    snprintf(out, size, "%s/music/song%d.flac", root, i);
}

static void write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || write(fd, data, size) != (ssize_t)size || close(fd) != 0) {
        perror(path);
        exit(1);
    }
}

static void patch_u32(const char *dir, const char *name, off_t offset, uint32_t value) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(pwrite(fd, &value, sizeof(value), offset) == (ssize_t)sizeof(value));
    close(fd);
}

static int32_t read_pointer(const char *dir) {
    char path[600], buf[32] = "";
    snprintf(path, sizeof(path), "%s/tagcache.gen", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
    fclose(f);
    return (int32_t)atoi(buf);
}

/* One full scan of the test songs as the library performs it. */
static bool scan(void) {
    tagcache_begin_update_with_lock(NULL, NULL);
    for (int i = 0; i < SONG_COUNT; i++) {
        char path[300];
        bool changed = false;
        song_path(path, sizeof(path), i);
        tagcache_upsert_year_changed(path, 1000 + i, 4096, titles[i], artists[i], albums[i], artists[i],
                                     "Genre", i + 1, 1, 2001, &changed);
    }
    return tagcache_end_update_with_lock(NULL, NULL);
}

/* The open database holds exactly the test songs and answers queries. */
static void check_library(void) {
    CHECK(tagcache_live_count() == SONG_COUNT);
    for (int i = 0; i < SONG_COUNT; i++) {
        char path[300];
        tagcache_song_t song;
        song_path(path, sizeof(path), i);
        CHECK(tagcache_song_by_path(path, &song));
        CHECK(strcmp(song.title, titles[i]) == 0);
        CHECK(strcmp(song.album, albums[i]) == 0);
        CHECK(song.release_year == 2001);
    }
    tagcache_song_t first;
    CHECK(tagcache_song_at_title_rank(0, &first) && strcmp(first.title, "Alpha") == 0);
    CHECK(tagcache_group_count(TAGCACHE_GROUP_ARTIST) == 2);
    CHECK(tagcache_group_count(TAGCACHE_GROUP_ALBUM) == 2);
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_TITLES, "rav", NULL, NULL, NULL);
    CHECK(tagcache_query_count(&query) == 1);
}

/* A library written by this engine: fresh open, scan, reopen. */
static int32_t build_library(const char *dir) {
    CHECK(tagcache_open(dir));
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_FRESH);
    CHECK(scan());
    check_library();
    tagcache_close();
    CHECK(tagcache_open(dir));
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_NORMAL);
    check_library();
    int32_t gen = tagcache_generation();
    CHECK(gen > 0 && read_pointer(dir) == gen);
    tagcache_close();
    return gen;
}

/* An unloadable database is refused without a partial load, then rebuilt
 * into a library that opens normally once an ordinary update follows. */
static void check_rejected_then_rebuilt(const char *dir, const char *what) {
    fprintf(stderr, "case: %s\n", what);
    CHECK(!tagcache_open(dir));
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_FAILED);
    CHECK(tagcache_live_count() == 0);
    CHECK(tagcache_open_for_rebuild(dir));
    CHECK(tagcache_live_count() == 0);
    CHECK(scan());
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_RECOVERED);
    check_library();
    tagcache_close();

    CHECK(tagcache_open(dir));
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_RECOVERED);
    check_library();
    CHECK(scan());
    int32_t gen = tagcache_generation();
    tagcache_close();
    CHECK(read_pointer(dir) == gen);
    CHECK(tagcache_open(dir));
    CHECK(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_NORMAL);
    check_library();
    tagcache_close();
}

int main(void) {
    snprintf(root, sizeof(root), "/tmp/mt-tagcache-XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    char music[300];
    make_dir(music, sizeof(music), "music");
    for (int i = 0; i < SONG_COUNT; i++) {
        char path[300];
        song_path(path, sizeof(path), i);
        write_file(path, "x", 1);
    }

    char dir[300], name[80];
    int32_t gen;

    make_dir(dir, sizeof(dir), "current");
    build_library(dir);

    /* A generation whose master carries the plain Rockbox magic. */
    make_dir(dir, sizeof(dir), "plain_master");
    gen = build_library(dir);
    snprintf(name, sizeof(name), "database_idx.tcd.g%d", gen);
    patch_u32(dir, name, 0, 0x54434810u);
    check_rejected_then_rebuilt(dir, "plain master magic");

    /* Index meta version 3 (no query postings file in that format). */
    make_dir(dir, sizeof(dir), "index_v3");
    gen = build_library(dir);
    snprintf(name, sizeof(name), "database_idx.tcd.g%d.imeta", gen);
    patch_u32(dir, name, 4, 3u);
    check_rejected_then_rebuilt(dir, "index meta v3");

    /* Query postings version 1. */
    make_dir(dir, sizeof(dir), "query_v1");
    gen = build_library(dir);
    snprintf(name, sizeof(name), "database_idx.tcd.g%d.iquery", gen);
    patch_u32(dir, name, 4, 1u);
    check_rejected_then_rebuilt(dir, "query postings v1");

    /* A generation without its refs manifest. */
    make_dir(dir, sizeof(dir), "no_refs");
    gen = build_library(dir);
    {
        char path[600];
        snprintf(path, sizeof(path), "%s/tagcache.refs.g%d", dir, gen);
        CHECK(unlink(path) == 0);
    }
    check_rejected_then_rebuilt(dir, "missing refs manifest");

    /* A Rockbox-style unsuffixed database without persisted indexes. */
    make_dir(dir, sizeof(dir), "rockbox");
    {
        int32_t master[6] = {0x54434810, 0, 0, 1, 0, 0};
        int32_t tag[3] = {0x54434810, 0, 0};
        char path[600];
        snprintf(path, sizeof(path), "%s/database_idx.tcd", dir);
        write_file(path, master, sizeof(master));
        static const int tags[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12};
        for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
            snprintf(path, sizeof(path), "%s/database_%d.tcd", dir, tags[i]);
            write_file(path, tag, sizeof(tag));
        }
    }
    check_rejected_then_rebuilt(dir, "unsuffixed database without indexes");

    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", root);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("tagcache: all checks passed\n");
    return 0;
}
