/* Live MusicBrainz check: searches three albums, fetches the top
 * candidates, scores them against synthetic local files built from the
 * best-ranked one, and prints everything. With --save-fixtures DIR the
 * Atomic Blonde and Kamelot release JSON (and the Atomic Blonde search)
 * are written there for test_retag; --fetch MBID OUT saves one release. All requests go through the client's
 * 1 req/s gate; the elapsed times printed show it working. */
#include "musicbrainz.h"
#include "retag_match.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char * result_name(mb_result_t r) {
    static const char * const names[] = { "OK", "NOT_FOUND", "NET_ERROR", "PARSE_ERROR", "RATE_LIMITED" };
    return (unsigned) r < 5 ? names[r] : "?";
}

static double t0;
static double last_end;

static void stamp(const char * what, mb_result_t r, double start) {
    double end = now_s();
    printf("  [t=%6.2fs] %-28s %-10s took %.2fs (gap since previous call end %.2fs)\n", end - t0, what,
           result_name(r), end - start, start - last_end);
    last_end = end;
}

static void print_detail(const mb_release_detail_t * d) {
    printf("  release %s \"%s\" by %s (artist %s)\n  date %s country %s format %s discs %d tracks %d genre \"%s\"\n",
           d->release.id, d->release.title, d->release.artist, d->release.artist_id, d->release.date,
           d->release.country, d->release.format, d->disc_count, d->track_count, d->genre);
    for (int i = 0; i < d->track_count; i++) {
        const mb_track_t * t = &d->tracks[i];
        printf("    %d-%02d  %d:%02d.%03d  %-45s  %s\n", t->disc, t->number, t->length_ms / 60000,
               t->length_ms / 1000 % 60, t->length_ms % 1000, t->title, t->artist);
    }
}

/* Local files as a ripper would have tagged them from `d`, durations rounded
 * to whole seconds like most tag readers report. */
static retag_local_t * synth_files(const mb_release_detail_t * d, int * n) {
    retag_local_t * f = calloc(d->track_count > 0 ? (size_t) d->track_count : 1, sizeof(*f));
    for (int i = 0; i < d->track_count; i++) {
        const mb_track_t * t = &d->tracks[i];
        f[i] = (retag_local_t) { "", t->title, t->artist, d->release.title, "", t->number, -1, -1, "",
                                 (double) ((t->length_ms + 500) / 1000) };
    }
    *n = d->track_count;
    return f;
}

static int save(const char * dir, const char * name, const char * url) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    char * body = NULL;
    size_t len = 0;
    double start = now_s();
    mb_result_t r = mb_fetch_raw(url, &body, &len);
    stamp(name, r, start);
    if (r != MB_OK) return -1;
    FILE * fp = fopen(path, "wb");
    if (!fp) { free(body); return -1; }
    fwrite(body, 1, len, fp);
    fclose(fp);
    printf("  saved %s (%zu bytes)\n", path, len);
    free(body);
    return 0;
}

typedef struct {
    const char * album, * artist;
    int tracks;
    const char * fixture;    /* save best release under this name */
} album_case_t;

typedef struct {
    const char * id;
    mb_release_detail_t d;
    mb_result_t r;
    double start, end;
} thread_job_t;

static void * fetch_thread(void * arg) {
    thread_job_t * j = arg;
    j->start = now_s();
    j->r = mb_get_release(j->id, &j->d, NULL);
    j->end = now_s();
    return NULL;
}

int main(int argc, char ** argv) {
    const char * fixture_dir = argc > 2 && strcmp(argv[1], "--save-fixtures") == 0 ? argv[2] : NULL;
    t0 = last_end = now_s();
    if (argc == 4 && strcmp(argv[1], "--fetch") == 0) { /* --fetch <release MBID> <out.json> */
        char url[256];
        snprintf(url, sizeof(url),
                 "https://musicbrainz.org/ws/2/release/%s?inc=recordings+artist-credits+media+genres+release-groups&fmt=json",
                 argv[2]);
        return save(".", argv[3], url) == 0 ? 0 : 1;
    }
    char query[1024];
    album_case_t cases[] = {
        { "Atomic Blonde (Original Motion Picture Soundtrack)", "Various Artists", 16, "atomic_blonde_release.json" },
        { "Born To Die", "Lana Del Rey", 12, NULL },
        { "Ghost Opera", "Kamelot", 12, "kamelot_ghost_opera_release.json" },
    };
    int failures = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        album_case_t * ac = &cases[c];
        mb_build_search_query(ac->album, ac->artist, ac->tracks, query, sizeof(query));
        printf("\n=== \"%s\" by \"%s\", %d tracks\n  query: %s\n", ac->album, ac->artist, ac->tracks, query);
        mb_release_t found[10];
        int n = 0;
        double start = now_s();
        mb_result_t r = mb_search_releases(ac->album, ac->artist, ac->tracks, found, 10, &n, NULL);
        stamp("search", r, start);
        if (r != MB_OK) { failures++; continue; }
        for (int i = 0; i < n; i++)
            printf("  #%d mb=%3d %s  %-50.50s | %-16.16s | %-10s %-3s %-16s %2d trk %d disc\n", i + 1,
                   found[i].mb_score, found[i].id, found[i].title, found[i].artist, found[i].date, found[i].country,
                   found[i].format, found[i].track_count, found[i].disc_count);

        /* The UI's pre-pick: best search score, exact track count first. */
        int k = n < 3 ? n : 3;
        mb_release_detail_t details[3];
        memset(details, 0, sizeof(details));
        for (int i = 0; i < k; i++) {
            start = now_s();
            r = mb_get_release(found[i].id, &details[i], NULL);
            stamp("lookup", r, start);
            if (r != MB_OK) failures++;
        }
        if (k == 0 || !details[0].tracks) continue;
        printf("  -- best-ranked candidate detail:\n");
        print_detail(&details[0]);

        int nf = 0;
        retag_local_t * files = synth_files(&details[0], &nf);
        printf("  -- matcher, local files synthesised from #1 (whole-second durations):\n");
        for (int i = 0; i < k; i++) {
            if (!details[i].tracks) continue;
            retag_match_t m;
            retag_match_score(files, nf, &details[i], &m);
            printf("    #%d %s %-16s %2d trk: score %3d matched %d/%d\n", i + 1, details[i].release.id,
                   details[i].release.format, details[i].track_count, m.score, m.matched, nf);
            retag_match_free(&m);
        }
        free(files);

        if (fixture_dir && ac->fixture) {
            char url[2048];
            snprintf(url, sizeof(url),
                     "https://musicbrainz.org/ws/2/release/%s?inc=recordings+artist-credits+media+genres+release-groups&fmt=json",
                     found[0].id);
            if (save(fixture_dir, ac->fixture, url) != 0) failures++;
            if (c == 0) {
                char enc[1024];
                mb_build_search_query(ac->album, ac->artist, ac->tracks, query, sizeof(query));
                size_t p = 0;
                for (const unsigned char * s = (const unsigned char *) query; *s && p + 4 < sizeof(enc); s++) {
                    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))
                        enc[p++] = (char) *s;
                    else p += (size_t) sprintf(enc + p, "%%%02X", *s);
                }
                enc[p] = '\0';
                snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/release/?query=%s&fmt=json&limit=5", enc);
                if (save(fixture_dir, "atomic_blonde_search.json", url) != 0) failures++;
            }
        }
        for (int i = 0; i < k; i++) mb_release_detail_free(&details[i]);
    }

    /* Gate under contention: three threads at once must still be spaced. */
    printf("\n=== concurrent lookups (3 threads)\n");
    thread_job_t jobs[3] = { { .id = "93fead8e-5c00-4db9-8a74-5c596ca00191" },
                             { .id = "8feb0444-7494-4184-b0a4-3fc49850ee28" },
                             { .id = "93fead8e-5c00-4db9-8a74-5c596ca00191" } };
    pthread_t th[3];
    double start = now_s();
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, fetch_thread, &jobs[i]);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    double ends[3];
    for (int i = 0; i < 3; i++) {
        ends[i] = jobs[i].end - start;
        printf("  thread %d %s %s tracks %d finished at +%.2fs\n", i, jobs[i].id, result_name(jobs[i].r),
               jobs[i].d.track_count, ends[i]);
        if (jobs[i].r != MB_OK) failures++;
        mb_release_detail_free(&jobs[i].d);
    }
    double total = now_s() - start;
    printf("  all three in %.2fs (>= 2 x %.1fs gate expected): %s\n", total, 1.1, total >= 2.2 ? "OK" : "FAIL");
    if (total < 2.2) failures++;

    /* Bad MBID: no request, NOT_FOUND. Unknown valid MBID: 404 -> NOT_FOUND. */
    mb_release_detail_t d;
    mb_result_t r = mb_get_release("not-an-mbid", &d, NULL);
    printf("\n  invalid id -> %s\n", result_name(r));
    if (r != MB_NOT_FOUND) failures++;
    start = now_s();
    r = mb_get_release("00000000-0000-0000-0000-000000000000", &d, NULL);
    stamp("lookup unknown mbid", r, start);
    if (r != MB_NOT_FOUND) failures++;
    printf("\n%s (%d failures)\n", failures ? "LIVE TEST FAILED" : "LIVE TEST OK", failures);
    return failures ? 1 : 0;
}
