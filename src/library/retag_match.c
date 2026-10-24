/* Local album <-> MusicBrainz release matching and the resulting tag
 * changes. See retag_match.h for the scoring rules. */
#include "retag_match.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DUR_FULL_S 3.0   /* within this many seconds: a full duration match */
#define DUR_ZERO_S 10.0  /* at or beyond: no duration credit */
#define W_DUR 0.50
#define W_TITLE 0.35
#define W_NUMBER 0.15
#define W_ARTIST 0.15   /* only when the file names a track artist */
#define DUR_TIEBREAK 0.004 /* per second inside DUR_FULL_S: closer wins a tie */
#define PAIR_MIN 0.35    /* greedy: weaker pairs leave the file unmapped */
#define TITLE_MAX 256

/* ---- Title normalisation ---- */

/* U+00C0..U+00FF and U+0100..U+017F folded to one ASCII letter; '*' marks
 * a two-letter fold, ' ' a symbol (multiplication/division sign). */
static const char fold_latin1[] = "aaaaaa*ceeeeiiiidnooooo ouuuuy**aaaaaa*ceeeeiiiidnooooo ouuuuy*y";
static const char fold_ext_a[] =
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooo**rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

static unsigned utf8_decode(const unsigned char ** pp) {
    const unsigned char * p = *pp;
    unsigned c = p[0];
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && c < 0xC0) extra = 0;
    unsigned cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
    int i = 1;
    for (; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) break;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (i <= extra) { *pp = p + 1; return 0xFFFD; }
    *pp = p + i;
    return cp;
}

static void put(char * out, size_t size, size_t * pos, const char * s, size_t n) {
    for (size_t i = 0; i < n && *pos + 1 < size; i++) out[(*pos)++] = s[i];
}

/* Lower-case, diacritics folded, apostrophes dropped, typographic dashes
 * and quotes made ASCII; other non-ASCII bytes are kept as they are. */
static void fold(const char * in, char * out, size_t size) {
    size_t pos = 0;
    const unsigned char * p = (const unsigned char *) (in ? in : "");
    while (*p && pos + 1 < size) {
        const unsigned char * start = p;
        unsigned cp = utf8_decode(&p);
        if (cp < 0x80) {
            char c = (char) (cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
            if (c == '\'' || c == '`') continue;
            put(out, size, &pos, &c, 1);
        } else if (cp >= 0xC0 && cp <= 0x17F) {
            char c = cp <= 0xFF ? fold_latin1[cp - 0xC0] : fold_ext_a[cp - 0x100];
            if (c != '*') put(out, size, &pos, &c, 1);
            else if (cp == 0xC6 || cp == 0xE6) put(out, size, &pos, "ae", 2);
            else if (cp == 0xDE || cp == 0xFE) put(out, size, &pos, "th", 2);
            else if (cp == 0xDF) put(out, size, &pos, "ss", 2);
            else put(out, size, &pos, "oe", 2); /* U+0152/U+0153 */
        } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201B || cp == 0x2BC || cp == 0xB4) {
            continue;
        } else if (cp >= 0x2010 && cp <= 0x2015) {
            put(out, size, &pos, "-", 1);
        } else if (cp == 0x201C || cp == 0x201D || cp == 0xA0 || cp == 0x2026 || cp == 0xD7) {
            put(out, size, &pos, " ", 1);
        } else {
            put(out, size, &pos, (const char *) start, (size_t) (p - start));
        }
    }
    out[pos] = '\0';
}

static const char * const version_words[] = {
    "remaster", "bonus", "explicit", "clean", "version", "mono", "stereo", "deluxe", "feat.", "feat ", "ft.",
    "featuring", "from the", "motion picture", "soundtrack", "edit", "live", "demo", "instrumental", NULL
};

static bool has_version_word(const char * s, size_t n) {
    char tmp[TITLE_MAX];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    for (int i = 0; version_words[i]; i++)
        if (strstr(tmp, version_words[i])) return true;
    return false;
}

/* Drops "(...)"/"[...]" groups and a " - ..." tail that name a version
 * rather than the song; works on folded (lower-case) text in place. */
static void strip_version_suffixes(char * s) {
    for (char * open = s; (open = strpbrk(open, "([")) != NULL;) {
        char * close = strchr(open, *open == '(' ? ')' : ']');
        if (!close) close = open + strlen(open) - 1;
        if (open != s && has_version_word(open + 1, (size_t) (close - open - 1))) {
            memmove(open, close + 1, strlen(close + 1) + 1);
        } else {
            open = close + 1;
            if (!*close) break;
        }
    }
    char * dash = s;
    while ((dash = strstr(dash, " - ")) != NULL) {
        if (dash != s && has_version_word(dash + 3, strlen(dash + 3))) { *dash = '\0'; break; }
        dash += 3;
    }
}

/* Punctuation to single spaces, "&" to "and", trimmed. */
static void squash(const char * in, char * out, size_t size) {
    size_t pos = 0;
    bool space = true;
    for (const unsigned char * p = (const unsigned char *) in; *p && pos + 1 < size; p++) {
        unsigned char c = *p;
        if (c == '&') {
            if (!space) put(out, size, &pos, " ", 1);
            put(out, size, &pos, "and ", 4);
            space = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
            out[pos++] = (char) c;
            space = false;
        } else if (!space) {
            out[pos++] = ' ';
            space = true;
        }
    }
    while (pos > 0 && out[pos - 1] == ' ') pos--;
    out[pos] = '\0';
}

void retag_normalize_title(const char * in, char * out, size_t out_size) {
    char tmp[TITLE_MAX * 2];
    fold(in, tmp, sizeof(tmp));
    strip_version_suffixes(tmp);
    squash(tmp, out, out_size);
}

/* Album titles keep their "(Original Motion Picture Soundtrack)" part:
 * only folding and punctuation apply. */
static void normalize_plain(const char * in, char * out, size_t out_size) {
    char tmp[TITLE_MAX * 2];
    fold(in, tmp, sizeof(tmp));
    squash(tmp, out, out_size);
}

/* A file title such as "01 - Song" or "3. Song": drop the number. */
static const char * skip_track_prefix(const char * s) {
    const char * p = s;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) { p++; digits++; }
    if (digits == 0 || digits > 3) return s;
    const char * q = p;
    while (*q == ' ') q++;
    if (*q != '-' && *q != '.' && *q != '_' && *q != ')') return s;
    q++;
    while (*q == ' ') q++;
    return *q ? q : s;
}

static int u16_cmp(const void * a, const void * b) {
    uint16_t x = *(const uint16_t *) a, y = *(const uint16_t *) b;
    return x < y ? -1 : x > y;
}

/* Bigram Dice coefficient of two normalised strings; -1 when either is empty. */
static double dice(const char * a, const char * b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0 || lb == 0) return -1.0;
    if (strcmp(a, b) == 0) return 1.0;
    if (la < 2 || lb < 2) return 0.0;
    uint16_t ba[TITLE_MAX], bb[TITLE_MAX];
    size_t na = 0, nb = 0;
    for (size_t i = 0; i + 1 < la && na < TITLE_MAX; i++) ba[na++] = (uint16_t) ((unsigned char) a[i] << 8 | (unsigned char) a[i + 1]);
    for (size_t i = 0; i + 1 < lb && nb < TITLE_MAX; i++) bb[nb++] = (uint16_t) ((unsigned char) b[i] << 8 | (unsigned char) b[i + 1]);
    qsort(ba, na, sizeof(uint16_t), u16_cmp);
    qsort(bb, nb, sizeof(uint16_t), u16_cmp);
    size_t i = 0, j = 0, common = 0;
    while (i < na && j < nb) {
        if (ba[i] == bb[j]) { common++; i++; j++; }
        else if (ba[i] < bb[j]) i++;
        else j++;
    }
    return 2.0 * (double) common / (double) (na + nb);
}

double retag_title_similarity(const char * a, const char * b) {
    char na[TITLE_MAX], nb[TITLE_MAX];
    retag_normalize_title(a, na, sizeof(na));
    retag_normalize_title(b, nb, sizeof(nb));
    double d = dice(na, nb);
    return d < 0 ? 0.0 : d;
}

/* ---- Matching ---- */

static bool is_blank(const char * s) {
    if (!s) return true;
    while (*s == ' ') s++;
    return !*s || strncasecmp(s, "unknown", 7) == 0 || strcasecmp(s, "<unknown>") == 0;
}


typedef struct {
    float score;
    int file;
    int track;
} pair_t;

typedef struct {
    const retag_local_t * files;
    const mb_release_detail_t * detail;
    char (*file_titles)[TITLE_MAX];
    char (*track_titles)[TITLE_MAX];
    char (*file_artists)[TITLE_MAX];  /* "" when it says nothing (blank, or the album's artist) */
    char (*track_artists)[TITLE_MAX];
} match_ctx_t;

static double dur_credit(double file_s, int length_ms) {
    if (file_s <= 0 || length_ms <= 0) return -1.0;
    double d = file_s - length_ms / 1000.0;
    if (d < 0) d = -d;
    if (d <= DUR_FULL_S) return 1.0;
    if (d >= DUR_ZERO_S) return 0.0;
    return (DUR_ZERO_S - d) / (DUR_ZERO_S - DUR_FULL_S);
}

static bool number_matches(const retag_local_t * f, const mb_track_t * t, int global_index, int disc_count) {
    if (f->track_number <= 0) return false;
    if (f->track_number == t->number && (f->disc_number <= 0 || f->disc_number == t->disc)) return true;
    /* Numbered straight through a multi-disc set (disc 2 starting at 13). */
    return disc_count > 1 && f->disc_number <= 0 && f->track_number == global_index + 1;
}

static double pair_score(const match_ctx_t * ctx, int fi, int ti, double * out_dur, double * out_title) {
    const retag_local_t * f = &ctx->files[fi];
    const mb_track_t * t = &ctx->detail->tracks[ti];
    double s = 0, w = 0;
    double dur = dur_credit(f->duration_s, t->length_ms);
    if (dur >= 0) {
        double d = f->duration_s - t->length_ms / 1000.0;
        if (d < 0) d = -d;
        s += W_DUR * dur - (d <= DUR_FULL_S ? DUR_TIEBREAK * d : 0);
        w += W_DUR;
    }
    double title = dice(ctx->file_titles[fi], ctx->track_titles[ti]);
    if (title >= 0) { s += W_TITLE * title; w += W_TITLE; }
    double artist = dice(ctx->file_artists[fi], ctx->track_artists[ti]);
    if (artist >= 0) { s += W_ARTIST * artist; w += W_ARTIST; }
    if (f->track_number > 0) {
        s += W_NUMBER * (number_matches(f, t, ti, ctx->detail->disc_count) ? 1.0 : 0.0);
        w += W_NUMBER;
    }
    if (out_dur) *out_dur = dur;
    if (out_title) *out_title = title;
    return w > 0 ? s / w : 0.0;
}

static int pair_cmp(const void * a, const void * b) {
    const pair_t * x = a, * y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    if (x->file != y->file) return x->file < y->file ? -1 : 1; /* stable for equal scores */
    return x->track < y->track ? -1 : x->track > y->track;
}

/* Greedy best-pair assignment; returns the mapped count or -1 on OOM. */
static int assign_greedy(const match_ctx_t * ctx, int n, int m, int * map) {
    for (int i = 0; i < n; i++) map[i] = -1;
    pair_t * pairs = malloc((size_t) n * (size_t) m * sizeof(pair_t));
    bool * used = calloc((size_t) m, sizeof(bool));
    if (!pairs || !used) {
        free(pairs);
        free(used);
        return -1;
    }
    size_t np = 0;
    for (int f = 0; f < n; f++)
        for (int t = 0; t < m; t++) {
            double s = pair_score(ctx, f, t, NULL, NULL);
            if (s >= PAIR_MIN) pairs[np++] = (pair_t) { (float) s, f, t };
        }
    qsort(pairs, np, sizeof(pair_t), pair_cmp);
    int mapped = 0;
    for (size_t i = 0; i < np && mapped < n && mapped < m; i++) {
        if (map[pairs[i].file] >= 0 || used[pairs[i].track]) continue;
        map[pairs[i].file] = pairs[i].track;
        used[pairs[i].track] = true;
        mapped++;
    }
    free(pairs);
    free(used);
    return mapped;
}

/* Files in playing order: by (disc, track) when every file is numbered,
 * else as given. Insertion sort (stable, no qsort context needed). */
static void file_order(const retag_local_t * files, int n, int * order) {
    bool numbered = true;
    for (int i = 0; i < n; i++) {
        order[i] = i;
        if (files[i].track_number <= 0) numbered = false;
    }
    if (!numbered) return;
    for (int i = 1; i < n; i++) {
        int v = order[i], j = i - 1;
        const retag_local_t * fv = &files[v];
        int dv = fv->disc_number > 0 ? fv->disc_number : 0;
        while (j >= 0) {
            const retag_local_t * fj = &files[order[j]];
            int dj = fj->disc_number > 0 ? fj->disc_number : 0;
            if (dj < dv || (dj == dv && fj->track_number <= fv->track_number)) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static double mean_pair(const match_ctx_t * ctx, int n, const int * map) {
    double sum = 0;
    for (int i = 0; i < n; i++)
        if (map[i] >= 0) sum += pair_score(ctx, i, map[i], NULL, NULL);
    return n > 0 ? sum / n : 0;
}

void retag_match_score(const retag_local_t * files, int file_count, const mb_release_detail_t * detail,
                       retag_match_t * out) {
    memset(out, 0, sizeof(*out));
    if (file_count <= 0) return;
    out->track_for_file = malloc((size_t) file_count * sizeof(int));
    if (!out->track_for_file) return;
    for (int i = 0; i < file_count; i++) out->track_for_file[i] = -1;
    int n = file_count, m = detail ? detail->track_count : 0;
    if (m <= 0) return;

    match_ctx_t ctx = { files, detail, malloc((size_t) n * TITLE_MAX), malloc((size_t) m * TITLE_MAX),
                        malloc((size_t) n * TITLE_MAX), malloc((size_t) m * TITLE_MAX) };
    int * greedy = malloc((size_t) n * sizeof(int));
    int * order = malloc((size_t) n * sizeof(int));
    if (!ctx.file_titles || !ctx.track_titles || !ctx.file_artists || !ctx.track_artists || !greedy || !order)
        goto done;
    for (int i = 0; i < n; i++) {
        const char * t = files[i].title ? files[i].title : "";
        retag_normalize_title(skip_track_prefix(t), ctx.file_titles[i], TITLE_MAX);
        const char * a = files[i].artist;
        bool says_nothing = is_blank(a) || strcasecmp(a, detail->release.artist) == 0 ||
                            (files[i].album_artist && strcasecmp(a, files[i].album_artist) == 0);
        normalize_plain(says_nothing ? "" : a, ctx.file_artists[i], TITLE_MAX);
    }
    for (int i = 0; i < m; i++) {
        retag_normalize_title(detail->tracks[i].title, ctx.track_titles[i], TITLE_MAX);
        normalize_plain(detail->tracks[i].artist, ctx.track_artists[i], TITLE_MAX);
    }

    int * map = out->track_for_file;
    if (assign_greedy(&ctx, n, m, greedy) < 0) goto done;
    memcpy(map, greedy, (size_t) n * sizeof(int));

    if (n == m) {
        file_order(files, n, order);
        int * pos_map = malloc((size_t) n * sizeof(int));
        if (pos_map) {
            int known = 0, agree = 0;
            for (int i = 0; i < n; i++) {
                pos_map[order[i]] = i;
                double d = dur_credit(files[order[i]].duration_s, detail->tracks[i].length_ms);
                if (d >= 0) { known++; if (d >= 1.0) agree++; }
            }
            bool take = known > 0 ? agree * 10 >= known * 9
                                  : mean_pair(&ctx, n, pos_map) >= mean_pair(&ctx, n, greedy) - 0.02;
            if (take) {
                /* Position wins overall, but a pair that plainly disagrees
                 * (a different song in that slot) stays unmapped. */
                for (int i = 0; i < n; i++)
                    map[i] = pair_score(&ctx, i, pos_map[i], NULL, NULL) >= PAIR_MIN ? pos_map[i] : -1;
            }
            free(pos_map);
        }
    }

    /* Overall confidence. */
    int matched = 0, dur_known = 0, title_known = 0;
    double dur_sum = 0, title_sum = 0;
    for (int i = 0; i < n; i++) {
        if (map[i] < 0) continue;
        matched++;
        double d, t;
        pair_score(&ctx, i, map[i], &d, &t);
        if (d >= 0) { dur_known++; dur_sum += d; }
        if (t >= 0) { title_known++; title_sum += t; }
    }
    out->matched = matched;

    double s = 0, w = 0;
    double count_ratio = (double) (n < m ? n : m) / (double) (n > m ? n : m);
    s += 0.25 * count_ratio; w += 0.25;
    s += 0.25 * (double) matched / n; w += 0.25;
    if (title_known > 0) { s += 0.35 * title_sum / title_known; w += 0.35; }
    const char * album = NULL;
    for (int i = 0; i < n && !album; i++)
        if (files[i].album && files[i].album[0]) album = files[i].album;
    if (album) {
        char la[TITLE_MAX], ra[TITLE_MAX];
        normalize_plain(album, la, sizeof(la));
        normalize_plain(detail->release.title, ra, sizeof(ra));
        double a = dice(la, ra);
        if (a >= 0) { s += 0.15 * a; w += 0.15; }
    }
    double base = w > 0 ? s / w : 0;
    /* Unmatched files count as disagreeing durations. */
    double dur_factor = dur_known > 0 ? dur_sum / (dur_known + (n - matched)) : 0.85;
    double score = 100.0 * base * (0.2 + 0.8 * dur_factor);
    out->score = (int) (score + 0.5);
    if (out->score > 100) out->score = 100;
    if (out->score < 0) out->score = 0;

done:
    free(ctx.file_titles);
    free(ctx.track_titles);
    free(ctx.file_artists);
    free(ctx.track_artists);
    free(greedy);
    free(order);
}

void retag_match_free(retag_match_t * m) {
    if (!m) return;
    free(m->track_for_file);
    m->track_for_file = NULL;
    m->matched = 0;
}

/* ---- Proposed changes ---- */

static int year_of(const char * date) {
    return date && date[0] >= '0' && date[0] <= '9' ? atoi(date) : -1;
}

static int medium_track_total(const mb_release_detail_t * d, int disc) {
    int n = 0;
    for (int i = 0; i < d->track_count; i++)
        if (d->tracks[i].disc == disc) n++;
    return n;
}

/* Old and new value of one field for one file; returns whether it changes.
 * track may be NULL (album-level fields only). */
static bool field_values(const retag_local_t * f, const mb_release_detail_t * d, const mb_track_t * t,
                         retag_field_t field, char * old_v, char * new_v, size_t size) {
    old_v[0] = new_v[0] = '\0';
    const char * o = NULL, * nv = NULL;
    switch (field) {
        case RETAG_FIELD_ALBUM: o = f->album; nv = d->release.title; break;
        case RETAG_FIELD_ALBUMARTIST: o = f->album_artist; nv = d->release.artist; break;
        case RETAG_FIELD_ARTIST: if (!t) return false; o = f->artist; nv = t->artist; break;
        case RETAG_FIELD_TITLE: if (!t) return false; o = f->title; nv = t->title; break;
        case RETAG_FIELD_GENRE: o = f->genre; nv = d->genre; break;
        case RETAG_FIELD_TRACK:
            if (!t) return false;
            if (f->track_number > 0) snprintf(old_v, size, "%d", f->track_number);
            snprintf(new_v, size, "%d", t->number);
            return f->track_number != t->number;
        case RETAG_FIELD_DISC:
            if (!t || d->disc_count <= 1) return false;
            if (f->disc_number > 0) snprintf(old_v, size, "%d", f->disc_number);
            snprintf(new_v, size, "%d", t->disc);
            return f->disc_number != t->disc;
        case RETAG_FIELD_DATE:
            if (f->year > 0) snprintf(old_v, size, "%d", f->year);
            snprintf(new_v, size, "%s", d->release.date);
            return year_of(d->release.date) > 0 && year_of(d->release.date) != f->year;
        case RETAG_FIELD_MBIDS:
            snprintf(new_v, size, "%s", d->release.id);
            return d->release.id[0] != '\0';
        default: return false;
    }
    snprintf(old_v, size, "%s", o ? o : "");
    snprintf(new_v, size, "%s", nv ? nv : "");
    return nv && nv[0] && strcmp(old_v, new_v) != 0;
}

static void example_copy(char * dst, size_t size, const char * src) {
    /* Truncate on a UTF-8 boundary. */
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char) src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int retag_summarize(const retag_local_t * files, int file_count, const mb_release_detail_t * detail,
                    const retag_match_t * m, retag_change_t * out, int max) {
    if (!files || !detail || !m || !m->track_for_file || !out || max <= 0) return 0;
    int rows = 0;
    char old_v[512], new_v[512], first_old[512], first_new[512];
    for (int field = 0; field < RETAG_FIELD_COUNT && rows < max; field++) {
        int changed = 0, blank_old = 0, min_n = INT_MAX, max_n = 0, total = 0;
        bool mixed_old = false, mixed_new = false, old_is_release_artist = false, dup_numbers = false;
        for (int i = 0; i < file_count; i++) {
            int ti = m->track_for_file[i];
            if (ti < 0 || ti >= detail->track_count) continue;
            const mb_track_t * t = &detail->tracks[ti];
            if (!field_values(&files[i], detail, t, (retag_field_t) field, old_v, new_v, sizeof(old_v))) continue;
            if (changed == 0) {
                memcpy(first_old, old_v, sizeof(first_old));
                memcpy(first_new, new_v, sizeof(first_new));
            } else {
                if (strcmp(first_old, old_v) != 0) mixed_old = true;
                if (strcmp(first_new, new_v) != 0) mixed_new = true;
            }
            changed++;
            if (is_blank(old_v)) blank_old++;
            if (field == RETAG_FIELD_ARTIST && strcasecmp(old_v, detail->release.artist) == 0) old_is_release_artist = true;
            int num = field == RETAG_FIELD_DISC ? t->disc : t->number;
            if (num < min_n) min_n = num;
            if (num > max_n) max_n = num;
        }
        if (changed == 0) continue;
        if (field == RETAG_FIELD_TRACK) {
            for (int i = 0; i < file_count && !dup_numbers; i++)
                for (int j = i + 1; j < file_count && !dup_numbers; j++)
                    if (files[i].track_number > 0 && files[i].track_number == files[j].track_number &&
                        files[i].disc_number == files[j].disc_number)
                        dup_numbers = true;
        }

        retag_change_t * r = &out[rows++];
        memset(r, 0, sizeof(*r));
        r->field = (retag_field_t) field;
        r->files_changed = changed;
        example_copy(r->old_example, sizeof(r->old_example), mixed_old ? "mixed" : first_old);
        switch (field) {
            case RETAG_FIELD_TRACK:
                total = detail->disc_count > 1 ? 0 : detail->track_count;
                if (total > 0)
                    snprintf(r->new_example, sizeof(r->new_example), "%d\xE2\x80\x93%d of %d", min_n, max_n, total);
                else
                    snprintf(r->new_example, sizeof(r->new_example), "%d\xE2\x80\x93%d", min_n, max_n);
                break;
            case RETAG_FIELD_DISC:
                snprintf(r->new_example, sizeof(r->new_example), "%d\xE2\x80\x93%d of %d", min_n, max_n,
                         detail->disc_count);
                break;
            default:
                if (mixed_new) {
                    example_copy(r->new_example, sizeof(r->new_example) - 6, first_new);
                    strcat(r->new_example, ", \xE2\x80\xA6");
                } else {
                    example_copy(r->new_example, sizeof(r->new_example), first_new);
                }
                break;
        }
        switch (field) {
            case RETAG_FIELD_ALBUM: r->default_on = blank_old > 0 || mixed_old; break;
            case RETAG_FIELD_ARTIST: r->default_on = blank_old == changed || old_is_release_artist; break;
            case RETAG_FIELD_TITLE: r->default_on = blank_old == changed; break;
            case RETAG_FIELD_TRACK: r->default_on = blank_old > 0 || dup_numbers; break;
            case RETAG_FIELD_MBIDS: r->default_on = true; break;
            default: r->default_on = blank_old > 0; break; /* ALBUMARTIST, DISC, DATE, GENRE */
        }
    }
    return rows;
}

typedef struct {
    char * buf;
    size_t size, used;
    retag_kv_t * fields;
    int max, count;
    bool overflow;
} kv_out_t;

static void kv_push(kv_out_t * o, const char * key, const char * value) {
    if (!value || !value[0] || o->overflow) return;
    size_t len = strlen(value) + 1;
    if (o->count >= o->max || o->used + len > o->size) { o->overflow = true; return; }
    memcpy(o->buf + o->used, value, len);
    o->fields[o->count].key = key;
    o->fields[o->count].value = o->buf + o->used;
    o->count++;
    o->used += len;
}

int retag_fields_for_file(const retag_local_t * file, const mb_release_detail_t * detail, const mb_track_t * track,
                          const bool enabled[RETAG_FIELD_COUNT], char * buf, size_t buf_size,
                          retag_kv_t * out_fields, int max_fields) {
    if (!file || !detail || !enabled || !buf || !out_fields) return -1;
    kv_out_t o = { buf, buf_size, 0, out_fields, max_fields, 0, false };
    char old_v[512], new_v[512], num[16];
    static const char * const keys[RETAG_FIELD_COUNT] = { "ALBUM", "ALBUMARTIST", "ARTIST", "TITLE", NULL, NULL,
                                                          "DATE", "GENRE", NULL };
    for (int field = 0; field < RETAG_FIELD_COUNT; field++) {
        if (!enabled[field]) continue;
        if (!field_values(file, detail, track, (retag_field_t) field, old_v, new_v, sizeof(old_v))) continue;
        switch (field) {
            case RETAG_FIELD_TRACK:
                kv_push(&o, "TRACKNUMBER", new_v);
                snprintf(num, sizeof(num), "%d", medium_track_total(detail, track->disc));
                kv_push(&o, "TRACKTOTAL", num);
                break;
            case RETAG_FIELD_DISC:
                kv_push(&o, "DISCNUMBER", new_v);
                snprintf(num, sizeof(num), "%d", detail->disc_count);
                kv_push(&o, "DISCTOTAL", num);
                break;
            case RETAG_FIELD_MBIDS:
                kv_push(&o, "MUSICBRAINZ_ALBUMID", detail->release.id);
                kv_push(&o, "MUSICBRAINZ_ALBUMARTISTID", detail->release.artist_id);
                if (track) {
                    kv_push(&o, "MUSICBRAINZ_ARTISTID", track->artist_id);
                    kv_push(&o, "MUSICBRAINZ_TRACKID", track->recording_id);
                    kv_push(&o, "MUSICBRAINZ_RELEASETRACKID", track->track_id);
                }
                break;
            default:
                kv_push(&o, keys[field], new_v);
                break;
        }
    }
    return o.overflow ? -1 : o.count;
}
