/* Offline tests: MusicBrainz JSON parsing (fixtures) and retag_match.
 * Usage: test_retag <fixtures dir> */
#include "musicbrainz.h"
#include "retag_match.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } } while (0)

static char * slurp(const char * dir, const char * name, size_t * len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE * fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(2); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char * buf = malloc((size_t) n + 1);
    if (fread(buf, 1, (size_t) n, fp) != (size_t) n) exit(2);
    buf[n] = '\0';
    fclose(fp);
    *len = (size_t) n;
    return buf;
}

static void load_release(const char * dir, const char * name, mb_release_detail_t * d) {
    size_t len;
    char * json = slurp(dir, name, &len);
    mb_result_t r = mb_parse_release_json(json, len, d);
    free(json);
    if (r != MB_OK) { printf("cannot parse %s\n", name); exit(2); }
}

/* Local files from a release: titles/artists as given, durations in whole
 * seconds plus jitter[i], numbering as requested. */
typedef struct {
    retag_local_t f[64];
    char titles[64][256];
    int n;
} album_t;

static void make_album(album_t * a, const mb_release_detail_t * d, int disc_only, bool numbered,
                       const double * jitter) {
    memset(a, 0, sizeof(*a));
    for (int i = 0; i < d->track_count && a->n < 64; i++) {
        const mb_track_t * t = &d->tracks[i];
        if (disc_only && t->disc != disc_only) continue;
        snprintf(a->titles[a->n], sizeof(a->titles[0]), "%s", t->title);
        a->f[a->n] = (retag_local_t) { "", a->titles[a->n], t->artist, d->release.title, d->release.artist,
                                       numbered ? t->number : -1, -1, 2017, "",
                                       (double) ((t->length_ms + 500) / 1000) + (jitter ? jitter[i] : 0) };
        a->n++;
    }
}

static bool identity_map(const retag_match_t * m, int n) {
    for (int i = 0; i < n; i++)
        if (m->track_for_file[i] != i) return false;
    return true;
}

static void test_normalize(void) {
    printf("normalisation\n");
    struct { const char * a, * b; double min; } cases[] = {
        { "Don\xE2\x80\x99t Stop Me Now (Remastered 2011)", "don't stop me now", 0.99 },
        { "Caf\xC3\xA9 del Mar", "CAFE DEL MAR", 0.99 },
        { "Major Tom (V\xC3\xB6llig losgel\xC3\xB6st)", "major tom (vollig losgelost)", 0.99 },
        { "Rule the World - 2017 Remaster", "Rule The World", 0.99 },
        { "Cat People (Putting Out Fire)", "Cat People", 0.45 }, /* not a version suffix: kept */
        { "Stigmata [Bonus Track]", "stigmata", 0.99 },
        { "Re\xE2\x80\x90" "Flex & Friends", "re-flex and friends", 0.99 },
        { "Season's End", "Seasons End", 0.99 },
        { "Bl\xC3\xBC" "cher", "Blucher", 0.99 },
        { "Ghost Opera (feat. Simone Simons)", "Ghost Opera", 0.99 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double s = retag_title_similarity(cases[i].a, cases[i].b);
        char n[256];
        retag_normalize_title(cases[i].a, n, sizeof(n));
        printf("  %.2f  \"%s\" -> \"%s\"\n", s, cases[i].a, n);
        CHECK(s >= cases[i].min, "sim(%s, %s) = %.2f", cases[i].a, cases[i].b, s);
    }
    CHECK(retag_title_similarity("Blue Monday", "London Calling") < 0.3, "unrelated titles too similar");
    CHECK(retag_title_similarity("Cat People (Putting Out Fire)", "Cat People (Putting Out Fire)") == 1.0, "exact");
}

static void test_parse(const char * dir, const mb_release_detail_t * ab, const mb_release_detail_t * kl) {
    printf("parsing\n");
    size_t len;
    char * json = slurp(dir, "atomic_blonde_search.json", &len);
    mb_release_t rel[5];
    int n = 0;
    CHECK(mb_parse_search_json(json, len, rel, 5, &n) == MB_OK && n >= 3, "search parse n=%d", n);
    for (int i = 0; i < n; i++)
        printf("  search #%d mb=%d %s \"%s\" %s %s %s %d trk\n", i + 1, rel[i].mb_score, rel[i].id, rel[i].title,
               rel[i].date, rel[i].country, rel[i].format, rel[i].track_count);
    char q[512];
    mb_build_search_query("Live AND \"Loud\": 1+1", "AC/DC", 0, q, sizeof(q));
    printf("  query: %s\n", q);
    CHECK(strcmp(q, "+release:(Live and \\\"Loud\\\"\\: 1\\+1) artist:(AC\\/DC)") == 0, "lucene escaping");
    mb_build_search_query("x", "", 7, q, sizeof(q));
    CHECK(strcmp(q, "+release:(x) tracks:7") == 0, "no artist: %s", q);
    for (size_t cut = 0; cut < len; cut += 97) { /* truncated input must fail cleanly */
        mb_release_detail_t d;
        if (mb_parse_release_json(json, cut, &d) == MB_OK) mb_release_detail_free(&d);
        mb_parse_search_json(json, cut, rel, 5, &n);
    }
    free(json);
    CHECK(mb_parse_search_json("{\"releases\":[]}", 15, rel, 5, &n) == MB_NOT_FOUND && n == 0, "empty search");
    CHECK(mb_parse_search_json("not json", 8, rel, 5, &n) == MB_PARSE_ERROR, "bad json");

    CHECK(ab->track_count == 16 && ab->disc_count == 1, "atomic blonde %d tracks %d discs", ab->track_count,
          ab->disc_count);
    CHECK(strcmp(ab->release.artist, "Various Artists") == 0, "VA phrase '%s'", ab->release.artist);
    CHECK(strcmp(ab->release.artist_id, "89ad4ac3-39f7-470e-963a-56509c546377") == 0, "VA MBID");
    CHECK(ab->genre[0] != '\0', "release-group genre fallback");
    CHECK(strcmp(ab->tracks[0].artist, "David Bowie") == 0 && ab->tracks[0].length_ms > 400000, "track 1");
    CHECK(strcmp(ab->tracks[9].artist, "Marilyn Manson & Tyler Bates") == 0, "joinphrase '%s'", ab->tracks[9].artist);
    for (int i = 0; i < ab->track_count; i++)
        CHECK(ab->tracks[i].number == i + 1 && ab->tracks[i].recording_id[0] && ab->tracks[i].track_id[0],
              "track %d ids", i);
    printf("  atomic blonde: %s \"%s\" %s %s genre \"%s\"\n", ab->release.id, ab->release.title,
           ab->release.date, ab->release.format, ab->genre);
    printf("  kamelot: %s \"%s\" %s %s %d discs %d tracks genre \"%s\"\n", kl->release.id, kl->release.title,
           kl->release.date, kl->release.format, kl->disc_count, kl->track_count, kl->genre);
}

static void test_match(const mb_release_detail_t * ab, const mb_release_detail_t * kl,
                       const mb_release_detail_t * kl2) {
    printf("matching\n");
    album_t a;
    retag_match_t m;

    make_album(&a, ab, 0, true, NULL);
    retag_match_score(a.f, a.n, ab, &m);
    printf("  exact durations, numbered:        score %3d matched %d/%d\n", m.score, m.matched, a.n);
    CHECK(m.score >= 95 && m.matched == 16 && identity_map(&m, a.n), "exact");
    retag_match_free(&m);

    double jitter[16] = { 2, -2, 1.5, -1, 2, -2, 0.5, 2, -2, 1, -1.5, 2, -2, 2, -2, 1.9 };
    make_album(&a, ab, 0, false, jitter);
    for (int i = 0; i < a.n; i++) {
        /* Different case/punctuation and a version suffix on some titles. */
        for (char * p = a.titles[i]; *p; p++)
            if (*p >= 'a' && *p <= 'z' && i % 2) *p = (char) (*p - 'a' + 'A');
        if (i % 3 == 0) strncat(a.titles[i], " (Remastered 2017)", sizeof(a.titles[i]) - strlen(a.titles[i]) - 1);
        if (i % 4 == 1) strncat(a.titles[i], "!", sizeof(a.titles[i]) - strlen(a.titles[i]) - 1);
    }
    retag_match_score(a.f, a.n, ab, &m);
    printf("  +-2 s jitter, no numbers, titles: score %3d matched %d/%d (e.g. \"%s\")\n", m.score, m.matched, a.n,
           a.titles[3]);
    CHECK(m.score >= 90 && identity_map(&m, a.n), "jitter");
    retag_match_free(&m);

    make_album(&a, ab, 0, true, NULL);
    memmove(&a.f[6], &a.f[7], sizeof(a.f[0]) * (size_t) (a.n - 7)); /* drop track 7 */
    a.n--;
    retag_match_score(a.f, a.n, ab, &m);
    printf("  one missing track:                score %3d matched %d/%d\n", m.score, m.matched, a.n);
    bool ok = m.matched == 15;
    for (int i = 0; i < a.n; i++) ok = ok && m.track_for_file[i] == (i < 6 ? i : i + 1);
    CHECK(m.score >= 85 && ok, "missing track");
    retag_match_free(&m);

    make_album(&a, ab, 0, false, jitter);
    int perm[16] = { 5, 12, 0, 15, 3, 9, 1, 14, 7, 11, 2, 8, 13, 4, 10, 6 };
    retag_local_t shuffled[16];
    for (int i = 0; i < 16; i++) shuffled[i] = a.f[perm[i]];
    retag_match_score(shuffled, 16, ab, &m);
    ok = true;
    for (int i = 0; i < 16; i++) ok = ok && m.track_for_file[i] == perm[i];
    printf("  shuffled, no numbers:             score %3d matched %d/16 %s\n", m.score, m.matched,
           ok ? "mapping correct" : "MAPPING WRONG");
    CHECK(ok && m.score >= 90, "shuffled");
    retag_match_free(&m);

    /* Shuffled, no titles or artists, exact whole-second durations: durations
     * alone. Father Figure and Stigmata are both 5:37 (337.09 / 336.81 s),
     * so that pair may swap; everything else must be right. */
    make_album(&a, ab, 0, false, NULL);
    for (int i = 0; i < 16; i++) {
        shuffled[i] = a.f[perm[i]];
        shuffled[i].title = "";
        shuffled[i].artist = "";
    }
    retag_match_score(shuffled, 16, ab, &m);
    int right = 0;
    for (int i = 0; i < 16; i++) right += m.track_for_file[i] == perm[i];
    printf("  shuffled, no titles/numbers:      score %3d matched %d/16, %d correct\n", m.score, m.matched, right);
    CHECK(right >= 14, "durations only");
    retag_match_free(&m);

    /* Same titles, a different cut: every track 15..45 s longer. */
    make_album(&a, ab, 0, true, NULL);
    for (int i = 0; i < a.n; i++) a.f[i].duration_s += 15 + (i * 7) % 31;
    retag_match_score(a.f, a.n, ab, &m);
    printf("  different cut (+15..45 s):        score %3d matched %d/%d\n", m.score, m.matched, a.n);
    CHECK(m.score < 40, "different cut scored %d", m.score);
    retag_match_free(&m);

    /* A wholly different album. */
    make_album(&a, kl, 0, true, NULL);
    retag_match_score(a.f, a.n, ab, &m);
    printf("  Kamelot files vs Atomic Blonde:   score %3d matched %d/%d\n", m.score, m.matched, a.n);
    CHECK(m.score < 30, "wrong album scored %d", m.score);
    retag_match_free(&m);

    /* 12 local files vs the CD+DVD limited edition (12 + 2 tracks). */
    make_album(&a, kl, 0, true, NULL);
    retag_match_score(a.f, a.n, kl2, &m);
    printf("  Kamelot JP files vs CD+DVD ed.:   score %3d matched %d/%d\n", m.score, m.matched, a.n);
    for (int i = 0; i < a.n; i++) {
        int t = m.track_for_file[i];
        if (t < 0) printf("    unmatched: %s\n", a.f[i].title);
        else if (strcmp(kl2->tracks[t].title, a.f[i].title) != 0)
            printf("    %s -> %d-%d %s\n", a.f[i].title, kl2->tracks[t].disc, kl2->tracks[t].number, kl2->tracks[t].title);
    }
    CHECK(m.score >= 70 && m.matched == 11 && m.track_for_file[11] == -1, "limited edition");
    retag_match_free(&m);

    /* Unknown durations everywhere: titles + numbers carry it. */
    make_album(&a, ab, 0, true, NULL);
    for (int i = 0; i < a.n; i++) a.f[i].duration_s = 0;
    retag_match_score(a.f, a.n, ab, &m);
    printf("  no durations:                     score %3d matched %d/%d\n", m.score, m.matched, a.n);
    CHECK(identity_map(&m, a.n) && m.score >= 70 && m.score < 95, "no durations");
    retag_match_free(&m);

    /* No two files on one track, even with duplicates locally. */
    make_album(&a, ab, 0, true, NULL);
    a.f[1] = a.f[0];
    retag_match_score(a.f, a.n, ab, &m);
    bool unique = true;
    for (int i = 0; i < a.n; i++)
        for (int j = i + 1; j < a.n; j++)
            if (m.track_for_file[i] >= 0 && m.track_for_file[i] == m.track_for_file[j]) unique = false;
    printf("  duplicate file:                   score %3d matched %d/%d %s\n", m.score, m.matched, a.n,
           unique ? "unique" : "DUPLICATE");
    CHECK(unique, "unique mapping");
    retag_match_free(&m);

    retag_match_score(a.f, 0, ab, &m);
    CHECK(m.score == 0 && m.matched == 0, "empty");
    retag_match_free(&m);
}

static const char * field_name(retag_field_t f) {
    static const char * const n[] = { "ALBUM", "ALBUMARTIST", "ARTIST", "TITLE", "TRACK", "DISC", "DATE", "GENRE",
                                      "MBIDS" };
    return n[f];
}

static void test_summary(const mb_release_detail_t * ab, const mb_release_detail_t * kl2) {
    printf("summary / fields\n");
    album_t a;
    make_album(&a, ab, 0, false, NULL);
    for (int i = 0; i < a.n; i++) {
        a.f[i].album = "Atomic Blonde (Original Motion Picture Soundtrack)";
        a.f[i].album_artist = "";
        a.f[i].year = -1;
        a.f[i].genre = "Unknown Genre";
        a.f[i].path = "/music/x.flac";
    }
    a.f[3].artist = "Various Artists"; /* one file credited to the album artist */
    retag_match_t m;
    retag_match_score(a.f, a.n, ab, &m);
    retag_change_t rows[RETAG_FIELD_COUNT];
    int n = retag_summarize(a.f, a.n, ab, &m, rows, RETAG_FIELD_COUNT);
    bool seen[RETAG_FIELD_COUNT] = { 0 }, on[RETAG_FIELD_COUNT] = { 0 };
    for (int i = 0; i < n; i++) {
        printf("  %-11s %2d files  \"%s\" -> \"%s\"  %s\n", field_name(rows[i].field), rows[i].files_changed,
               rows[i].old_example, rows[i].new_example, rows[i].default_on ? "[x]" : "[ ]");
        seen[rows[i].field] = true;
        on[rows[i].field] = rows[i].default_on;
    }
    CHECK(seen[RETAG_FIELD_ALBUMARTIST] && on[RETAG_FIELD_ALBUMARTIST], "album artist row on");
    CHECK(seen[RETAG_FIELD_TRACK] && on[RETAG_FIELD_TRACK], "track row on");
    CHECK(seen[RETAG_FIELD_DATE] && on[RETAG_FIELD_DATE], "date row on");
    CHECK(seen[RETAG_FIELD_GENRE] && on[RETAG_FIELD_GENRE], "genre row on");
    CHECK(seen[RETAG_FIELD_MBIDS] && on[RETAG_FIELD_MBIDS], "mbid row on");
    CHECK(seen[RETAG_FIELD_ALBUM] && !on[RETAG_FIELD_ALBUM], "album spelled differently: row off");
    CHECK(seen[RETAG_FIELD_ARTIST] && on[RETAG_FIELD_ARTIST], "artist 'Various Artists' on a track: on");
    CHECK(!seen[RETAG_FIELD_TITLE] && !seen[RETAG_FIELD_DISC], "no title/disc rows");

    bool enabled[RETAG_FIELD_COUNT];
    for (int i = 0; i < RETAG_FIELD_COUNT; i++) enabled[i] = on[i];
    char buf[2048];
    retag_kv_t kv[24];
    int ti = m.track_for_file[1];
    int k = retag_fields_for_file(&a.f[1], ab, &ab->tracks[ti], enabled, buf, sizeof(buf), kv, 24);
    printf("  fields for file 2 (%d):\n", k);
    for (int i = 0; i < k; i++) printf("    %s=%s\n", kv[i].key, kv[i].value);
    CHECK(k == 10, "file 2 field count %d", k); /* ALBUMARTIST TRACKNUMBER TRACKTOTAL DATE GENRE + 5 MBIDs */
    bool has_total = false, has_album = false, has_disc = false;
    for (int i = 0; i < k; i++) {
        if (strcmp(kv[i].key, "TRACKTOTAL") == 0) has_total = strcmp(kv[i].value, "16") == 0;
        if (strcmp(kv[i].key, "ALBUM") == 0) has_album = true;
        if (strncmp(kv[i].key, "DISC", 4) == 0) has_disc = true;
    }
    CHECK(has_total && !has_album && !has_disc, "TRACKTOTAL=16, no ALBUM (disabled), no DISC (1 disc)");
    CHECK(retag_fields_for_file(&a.f[1], ab, &ab->tracks[ti], enabled, buf, 40, kv, 24) == -1, "small buf -> -1");
    CHECK(retag_fields_for_file(&a.f[1], ab, &ab->tracks[ti], enabled, buf, sizeof(buf), kv, 3) == -1,
          "few slots -> -1");
    k = retag_fields_for_file(&a.f[1], ab, NULL, enabled, buf, sizeof(buf), kv, 24);
    printf("  album-level only (track NULL): %d fields\n", k);
    CHECK(k == 5, "album-level %d", k); /* ALBUMARTIST DATE GENRE ALBUMID ALBUMARTISTID */
    retag_match_free(&m);

    /* Fully tagged album already agreeing: only MBIDs change. */
    make_album(&a, ab, 0, true, NULL);
    for (int i = 0; i < a.n; i++) a.f[i].genre = ab->genre;
    retag_match_score(a.f, a.n, ab, &m);
    n = retag_summarize(a.f, a.n, ab, &m, rows, RETAG_FIELD_COUNT);
    printf("  already-correct album: %d row(s), first %s\n", n, n ? field_name(rows[0].field) : "-");
    CHECK(n == 1 && rows[0].field == RETAG_FIELD_MBIDS, "only MBIDs");
    retag_match_free(&m);

    /* Two-medium release (Atomic Blonde split 8 + 8, as on the 2x12" vinyl):
     * DISC rows and DISCNUMBER/DISCTOTAL. */
    mb_release_detail_t two = *ab;
    mb_track_t split[16];
    memcpy(split, ab->tracks, sizeof(split));
    for (int i = 8; i < 16; i++) { split[i].disc = 2; split[i].number = i - 7; }
    two.tracks = split;
    two.disc_count = 2;
    kl2 = &two; /* the rest of this test uses the split copy */
    album_t b;
    make_album(&b, kl2, 0, true, NULL);
    for (int i = 0; i < b.n; i++) b.f[i].disc_number = -1;
    retag_match_score(b.f, b.n, kl2, &m);
    n = retag_summarize(b.f, b.n, kl2, &m, rows, RETAG_FIELD_COUNT);
    bool disc_row = false;
    for (int i = 0; i < n; i++) {
        printf("  [2 discs] %-11s %2d files  \"%s\" -> \"%s\"  %s\n", field_name(rows[i].field), rows[i].files_changed,
               rows[i].old_example, rows[i].new_example, rows[i].default_on ? "[x]" : "[ ]");
        if (rows[i].field == RETAG_FIELD_DISC) disc_row = rows[i].default_on && rows[i].files_changed == b.n;
    }
    CHECK(disc_row, "disc row on for all files");
    for (int i = 0; i < RETAG_FIELD_COUNT; i++) enabled[i] = true;
    int last = b.n - 1;
    k = retag_fields_for_file(&b.f[last], kl2, &kl2->tracks[m.track_for_file[last]], enabled, buf, sizeof(buf), kv, 24);
    for (int i = 0; i < k; i++)
        if (strncmp(kv[i].key, "DISC", 4) == 0) printf("    last file %s=%s\n", kv[i].key, kv[i].value);
    retag_match_free(&m);
}

int main(int argc, char ** argv) {
    const char * dir = argc > 1 ? argv[1] : "fixtures";
    mb_release_detail_t ab, kl, kl2;
    load_release(dir, "atomic_blonde_release.json", &ab);
    load_release(dir, "kamelot_ghost_opera_release.json", &kl);
    load_release(dir, "kamelot_ghost_opera_limited_release.json", &kl2);
    test_normalize();
    test_parse(dir, &ab, &kl);
    test_match(&ab, &kl, &kl2);
    test_summary(&ab, &kl2);
    mb_release_detail_free(&ab);
    mb_release_detail_free(&kl);
    mb_release_detail_free(&kl2);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
