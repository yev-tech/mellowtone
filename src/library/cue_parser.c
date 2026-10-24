#include "cue_parser.h"
#include "library_endian.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Extracts a "quoted string" argument (the TITLE/PERFORMER/FILE convention)
 * from `line`, starting the scan at `line`'s own first non-space character
 * after the command keyword has already been skipped by the caller. Some
 * real-world tools write an unquoted argument for a name with no spaces --
 * tolerated here too: falls back to reading up to the next whitespace/EOL
 * if no opening quote is found. Bounded-copies into out (out_size includes
 * the NUL). */
static void extract_arg(const char * line, char * out, size_t out_size) {
    out[0] = '\0';
    while (*line == ' ' || *line == '\t') line++;

    if (*line == '"') {
        line++;
        const char * end = strchr(line, '"');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, line, len);
        out[len] = '\0';
        return;
    }

    size_t len = 0;
    while (line[len] && line[len] != '\r' && line[len] != '\n' && !isspace((unsigned char) line[len])) len++;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, line, len);
    out[len] = '\0';
}

/* mm:ss:ff (CD frame time -- 75 frames/sec, the Red Book standard every
 * real .cue sheet's INDEX lines use) -> seconds. Returns false (out
 * untouched) if the three fields don't parse as plain integers, so a
 * malformed INDEX line is skipped rather than silently producing 0:00:00 as
 * if it were a genuine track start. */
static bool parse_cue_time(const char * s, double * out_seconds) {
    if (!s || !out_seconds) return false;
    unsigned long fields[3] = {0, 0, 0};
    for (int field = 0; field < 3; field++) {
        if (!isdigit((unsigned char) *s)) return false;
        int digits = 0;
        while (isdigit((unsigned char) *s)) {
            if (++digits > 7 || fields[field] > 10000000UL) return false;
            fields[field] = fields[field] * 10UL + (unsigned long) (*s - '0');
            s++;
        }
        if (field < 2) {
            if (*s++ != ':') return false;
        }
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s != '\0' || fields[1] > 59 || fields[2] > 74) return false;
    *out_seconds = (double) fields[0] * 60.0 + (double) fields[1] + (double) fields[2] / 75.0;
    return true;
}

/* Case-insensitive "does line start with keyword, followed by a word
 * boundary (space/tab/EOL)" check -- real .cue sheets are written in
 * ALL CAPS by convention but not every tool honors that strictly. Returns
 * a pointer just past the keyword (and any following whitespace) on match,
 * NULL otherwise. */
static const char * match_keyword(const char * line, const char * keyword) {
    size_t len = strlen(keyword);
    if (strncasecmp(line, keyword, len) != 0) return NULL;
    char next = line[len];
    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n') return NULL;
    const char * rest = line + len;
    while (*rest == ' ' || *rest == '\t') rest++;
    return rest;
}

static bool append_track(cue_track_t ** tracks, int * count, int * capacity, int number) {
    if (*count == *capacity) {
        int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
        cue_track_t * grown = realloc(*tracks, sizeof(cue_track_t) * (size_t) new_capacity);
        if (!grown) return false;
        *tracks = grown;
        *capacity = new_capacity;
    }
    cue_track_t * t = &(*tracks)[*count];
    memset(t, 0, sizeof(*t));
    t->number = number;
    (*count)++;
    return true;
}

/* Windows-1252 0x80..0x9F; 0 = undefined there. */
static const unsigned short cp1252_high[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static bool valid_utf8(const unsigned char * s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t n;
        if (c < 0x80) n = 0;
        else if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if (c >= 0xF0 && c <= 0xF4) n = 3;
        else return false;
        if (n && i + n >= len) return false;
        for (size_t k = 1; k <= n; k++)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

/* Windows-1251 0x80..0xBF (0xC0..0xFF are U+0410..U+044F in order). */
static const unsigned short cp1251_high[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0,      0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

/* Cyrillic (Windows-1251) or Western (Windows-1252)? Cyrillic words are runs
 * of high bytes; Western text has accented letters between ASCII ones. */
static bool looks_cp1251(const unsigned char * s, size_t len) {
    size_t high = 0, paired = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 0xC0) continue;
        high++;
        if ((i > 0 && s[i - 1] >= 0xC0) || (i + 1 < len && s[i + 1] >= 0xC0)) paired++;
    }
    return high >= 4 && paired * 2 > high;
}

static unsigned decode_legacy(unsigned c, bool cyrillic) {
    if (c < 0x80) return c;
    if (cyrillic) {
        if (c >= 0xC0) return 0x0410 + (c - 0xC0);
        return cp1251_high[c - 0x80] ? cp1251_high[c - 0x80] : '?';
    }
    if (c < 0xA0) return cp1252_high[c - 0x80] ? cp1252_high[c - 0x80] : '?';
    return c; /* Latin-1 range */
}

char * cue_text_to_utf8(const char * text, size_t len, bool * converted) {
    if (converted) *converted = false;
    if (valid_utf8((const unsigned char *) text, len)) {
        char * out = malloc(len + 1);
        if (!out) return NULL;
        memcpy(out, text, len);
        out[len] = '\0';
        return out;
    }
    bool cyrillic = looks_cp1251((const unsigned char *) text, len);
    char * out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned c = decode_legacy((unsigned char) text[i], cyrillic);
        if (c < 0x80) out[n++] = (char) c;
        else if (c < 0x800) { out[n++] = (char) (0xC0 | (c >> 6)); out[n++] = (char) (0x80 | (c & 0x3F)); }
        else { out[n++] = (char) (0xE0 | (c >> 12)); out[n++] = (char) (0x80 | ((c >> 6) & 0x3F)); out[n++] = (char) (0x80 | (c & 0x3F)); }
    }
    out[n] = '\0';
    if (converted) *converted = true;
    return out;
}

bool cue_parse_text(const char * text_in, size_t text_len, const char * cue_path, const char * audio_override,
                    cue_sheet_t * out) {
    memset(out, 0, sizeof(*out));
    char * text = cue_text_to_utf8(text_in, text_len, NULL);
    if (!text) return false;

    char file_name[512] = "";
    int file_command_count = 0;

    cue_track_t * tracks = NULL;
    int track_count = 0;
    int track_capacity = 0;
    bool have_current_track = false;
    bool current_track_is_audio = false;
    bool any_track = false;
    bool parse_failed = false;

    char line[1024];
    const char * cursor = text;
    bool first_line = true;
    while (*cursor) {
        size_t ll = strcspn(cursor, "\n");
        size_t copy = ll < sizeof(line) - 1 ? ll : sizeof(line) - 1;
        memcpy(line, cursor, copy);
        line[copy] = '\0';
        cursor += ll;
        if (*cursor == '\n') cursor++;
        char * p = line;
        /* A leading UTF-8 BOM (common from Windows ripping tools) would
         * otherwise hide the first command's keyword. */
        if (first_line && library_utf8_bom_skip(p) == 3) {
            p += 3;
        }
        first_line = false;
        while (*p == ' ' || *p == '\t') p++;

        const char * arg;
        if ((arg = match_keyword(p, "FILE")) != NULL) {
            file_command_count++;
            extract_arg(arg, file_name, sizeof(file_name));
        } else if ((arg = match_keyword(p, "TRACK")) != NULL) {
            any_track = true;
            int number = atoi(arg);
            const char * type = arg;
            while (*type && !isspace((unsigned char) *type)) type++;
            while (*type == ' ' || *type == '\t') type++;
            /* Only AUDIO tracks -- a mixed-mode disc's own DATA track(s)
             * (a hidden data session some CD releases have) aren't
             * something this app can play, and would just be silently
             * unseekable/silent audio if treated as one. */
            current_track_is_audio = strncasecmp(type, "AUDIO", 5) == 0;
            if (current_track_is_audio) {
                have_current_track = append_track(&tracks, &track_count, &track_capacity, number);
                if (!have_current_track) {
                    parse_failed = true;
                    break;
                }
            } else {
                have_current_track = false;
            }
        } else if (!any_track && (arg = match_keyword(p, "TITLE")) != NULL) {
            extract_arg(arg, out->title, sizeof(out->title));
        } else if (!any_track && (arg = match_keyword(p, "PERFORMER")) != NULL) {
            extract_arg(arg, out->performer, sizeof(out->performer));
        } else if (!any_track && (arg = match_keyword(p, "REM")) != NULL) {
            const char * v;
            if ((v = match_keyword(arg, "DATE")) != NULL) extract_arg(v, out->date, sizeof(out->date));
            else if ((v = match_keyword(arg, "GENRE")) != NULL) extract_arg(v, out->genre, sizeof(out->genre));
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "TITLE")) != NULL) {
            extract_arg(arg, tracks[track_count - 1].title, sizeof(tracks[track_count - 1].title));
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "PERFORMER")) != NULL) {
            extract_arg(arg, tracks[track_count - 1].performer, sizeof(tracks[track_count - 1].performer));
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "INDEX")) != NULL) {
            /* INDEX 01 is the real track start every player seeks to;
             * INDEX 00 (if present) is the pre-gap, which precedes it and
             * isn't where a listener tapping this track expects playback
             * to begin -- skipped in favor of 01. Some sheets have only
             * 01 with no 00 at all, which this handles the same way
             * (the only INDEX line present is 01). */
            int index_number = atoi(arg);
            if (index_number == 1) {
                const char * time_str = arg;
                while (*time_str && !isspace((unsigned char) *time_str)) time_str++;
                while (*time_str == ' ' || *time_str == '\t') time_str++;
                char t[32];
                snprintf(t, sizeof(t), "%s", time_str);
                t[strcspn(t, "\r")] = '\0';
                double start_seconds;
                if (!parse_cue_time(t, &start_seconds)) {
                    parse_failed = true;
                    break;
                }
                tracks[track_count - 1].start_seconds = start_seconds;
                tracks[track_count - 1].has_index_01 = true;
            }
        }
    }
    free(text);

    /* Multiple FILE commands (a sheet spanning several physical audio
     * files) is real but comparatively rare -- correctly attributing each
     * track to the right one of several files needs tracking which FILE
     * section each track fell under, not just the single audio_path this
     * struct currently has room for. Rejected outright rather than
     * silently mis-attributing every track after the first FILE to the
     * wrong file, which would seek into the wrong song entirely. */
    if (!parse_failed) {
        for (int i = 0; i < track_count; i++) {
            if (!tracks[i].has_index_01 || (i > 0 && tracks[i].start_seconds <= tracks[i - 1].start_seconds)) {
                parse_failed = true;
                break;
            }
        }
    }
    if (parse_failed || file_command_count > 1 || track_count == 0 ||
        (!audio_override && (file_command_count != 1 || file_name[0] == '\0'))) {
        free(tracks);
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (audio_override) {
        snprintf(out->audio_path, sizeof(out->audio_path), "%s", audio_override);
    } else {
        /* Resolve relative to the .cue file's OWN directory, not the
         * process's current working directory (the .cue and its audio file
         * sit side by side on the SD card, wherever it is mounted). */
        const char * last_slash = cue_path ? strrchr(cue_path, '/') : NULL;
        if (last_slash) {
            size_t dir_len = (size_t) (last_slash - cue_path) + 1;
            if (dir_len >= sizeof(out->audio_path)) dir_len = sizeof(out->audio_path) - 1;
            memcpy(out->audio_path, cue_path, dir_len);
            snprintf(out->audio_path + dir_len, sizeof(out->audio_path) - dir_len, "%s", file_name);
        } else {
            snprintf(out->audio_path, sizeof(out->audio_path), "%s", file_name);
        }
    }

    out->tracks = tracks;
    out->track_count = track_count;
    return true;
}

#define CUE_MAX_BYTES (256 * 1024)

static char * read_small_file(const char * path, size_t * len) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * buf = malloc(CUE_MAX_BYTES + 1);
    size_t n = buf ? fread(buf, 1, CUE_MAX_BYTES + 1, f) : 0;
    fclose(f);
    if (!buf || n > CUE_MAX_BYTES) { free(buf); return NULL; }
    buf[n] = '\0';
    *len = n;
    return buf;
}

bool cue_parse_file(const char * cue_path, cue_sheet_t * out) {
    memset(out, 0, sizeof(*out));
    size_t len = 0;
    char * text = read_small_file(cue_path, &len);
    if (!text) return false;
    bool ok = cue_parse_text(text, len, cue_path, NULL, out);
    free(text);
    return ok;
}

/* ---- sheets inside FLAC files ---------------------------------------------------- */

static uint32_t be24(const unsigned char * p) { return (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2]; }
static uint32_t le32(const unsigned char * p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
static uint64_t be64(const unsigned char * p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

/* Reads the FLAC's CUESHEET Vorbis comment (*text) and/or CUESHEET block
 * (*block, *block_len) and the sample rate. Skips a leading ID3v2 tag. */
static bool flac_cue_blocks(const char * path, char ** text, size_t * text_len, unsigned char ** block, size_t * block_len,
                            unsigned * sample_rate) {
    *text = NULL;
    *block = NULL;
    *sample_rate = 0;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    unsigned char h[10];
    long start = 0;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3))
        start = 10 + (long) ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
    fseek(f, start, SEEK_SET);
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4) != 0) { fclose(f); return false; }
    for (int guard = 0; guard < 128; guard++) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        bool last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        uint32_t len = be24(bh + 1);
        long next = ftell(f) + (long) len;
        if (type == 0 && len >= 18) {
            unsigned char si[18];
            if (fread(si, 1, 18, f) == 18) *sample_rate = (unsigned) (si[10] << 12 | si[11] << 4 | si[12] >> 4);
        } else if ((type == 4 || type == 5) && len <= 4u * 1024u * 1024u) {
            unsigned char * b = malloc(len ? len : 1);
            if (b && fread(b, 1, len, f) == len) {
                if (type == 5) {
                    *block = b;
                    *block_len = len;
                    b = NULL;
                } else if (len >= 8) {
                    uint32_t vlen = le32(b), off = 4 + vlen;
                    uint32_t count = off + 4 <= len ? le32(b + off) : 0;
                    off += 4;
                    for (uint32_t i = 0; i < count && off + 4 <= len; i++) {
                        uint32_t cl = le32(b + off);
                        off += 4;
                        if (cl > len - off) break;
                        if (cl > 9 && !strncasecmp((const char *) b + off, "CUESHEET=", 9) && !*text) {
                            *text = malloc(cl - 9 + 1);
                            if (*text) {
                                memcpy(*text, b + off + 9, cl - 9);
                                (*text)[cl - 9] = '\0';
                                *text_len = cl - 9;
                            }
                        }
                        off += cl;
                    }
                }
            }
            free(b);
        }
        if (last) break;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    fclose(f);
    return *text || *block;
}

static void cue_time(double seconds, char * out, size_t size) {
    long frames = (long) (seconds * 75.0 + 0.5);
    snprintf(out, size, "%02ld:%02ld:%02ld", frames / 75 / 60, frames / 75 % 60, frames % 75);
}

/* A sheet text from a FLAC CUESHEET block (times only). */
static char * text_from_block(const unsigned char * b, size_t len, unsigned rate, const char * audio_path) {
    if (len < 396 || !rate) return NULL;
    int n = b[395];
    size_t off = 396;
    const char * name = strrchr(audio_path, '/');
    name = name ? name + 1 : audio_path;
    size_t cap = 256 + strlen(name) + (size_t) n * 96;
    char * t = malloc(cap);
    if (!t) return NULL;
    size_t k = (size_t) snprintf(t, cap, "FILE \"%s\" WAVE\r\n", name);
    for (int i = 0; i < n && off + 36 <= len; i++) {
        uint64_t toff = be64(b + off);
        int number = b[off + 8];
        bool audio = !(b[off + 21] & 0x80); /* after offset, number and ISRC: the non-audio flag */
        int points = b[off + 35];
        off += 36;
        double start = -1;
        for (int p = 0; p < points && off + 12 <= len; p++, off += 12)
            if (b[off + 8] == 1) start = (double) (toff + be64(b + off)) / rate;
        if (number == 170 || number == 255 || start < 0 || !audio) continue; /* lead-out, data */
        char tm[16];
        cue_time(start, tm, sizeof(tm));
        k += (size_t) snprintf(t + k, cap - k, "  TRACK %02d AUDIO\r\n    INDEX 01 %s\r\n", number, tm);
        if (k >= cap - 96) break;
    }
    return t;
}

static bool sheet_for_file_in_dir(const char * audio_path, cue_sheet_t * out, char * cue_path, size_t cue_path_size,
                                  char ** text_out) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", audio_path);
    char * slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';
    DIR * dp = opendir(dir);
    if (!dp) return false;
    struct dirent * de;
    bool found = false;
    while (!found && (de = readdir(dp)) != NULL) {
        const char * dot = strrchr(de->d_name, '.');
        if (de->d_name[0] == '.' || !dot || strcasecmp(dot, ".cue") != 0) continue;
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        size_t len = 0;
        char * text = read_small_file(path, &len);
        if (!text) continue;
        cue_sheet_t sheet;
        if (cue_parse_text(text, len, path, NULL, &sheet) && sheet.track_count > 1 &&
            strcmp(sheet.audio_path, audio_path) == 0) {
            *out = sheet;
            if (cue_path) snprintf(cue_path, cue_path_size, "%s", path);
            if (text_out) *text_out = cue_text_to_utf8(text, len, NULL);
            found = true;
        } else {
            cue_sheet_free(&sheet);
        }
        free(text);
    }
    closedir(dp);
    return found;
}

int cue_sheets_for_audio(const char * audio_path, char paths[][1024], int max) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", audio_path);
    char * slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';
    DIR * dp = opendir(dir);
    if (!dp) return 0;
    struct dirent * de;
    int n = 0;
    while (n < max && (de = readdir(dp)) != NULL) {
        const char * dot = strrchr(de->d_name, '.');
        if (de->d_name[0] == '.' || !dot || strcasecmp(dot, ".cue") != 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        cue_sheet_t sheet;
        if (cue_parse_file(path, &sheet)) {
            if (sheet.track_count > 1 && strcmp(sheet.audio_path, audio_path) == 0) snprintf(paths[n++], 1024, "%s", path);
            cue_sheet_free(&sheet);
        }
    }
    closedir(dp);
    return n;
}

cue_source_t cue_find_for_audio(const char * audio_path, cue_sheet_t * out, char * cue_path, size_t cue_path_size,
                                char ** text_out) {
    memset(out, 0, sizeof(*out));
    if (cue_path && cue_path_size) cue_path[0] = '\0';
    if (text_out) *text_out = NULL;
    if (!audio_path || !audio_path[0]) return CUE_SOURCE_NONE;
    if (sheet_for_file_in_dir(audio_path, out, cue_path, cue_path_size, text_out)) return CUE_SOURCE_FILE;
    const char * dot = strrchr(audio_path, '.');
    if (!dot || strcasecmp(dot, ".flac") != 0) return CUE_SOURCE_NONE;
    char * text = NULL;
    size_t text_len = 0;
    unsigned char * block = NULL;
    size_t block_len = 0;
    unsigned rate = 0;
    if (!flac_cue_blocks(audio_path, &text, &text_len, &block, &block_len, &rate)) return CUE_SOURCE_NONE;
    cue_source_t src = CUE_SOURCE_NONE;
    if (text && cue_parse_text(text, text_len, NULL, audio_path, out) && out->track_count > 1) {
        src = CUE_SOURCE_EMBEDDED_TEXT;
        if (text_out) *text_out = cue_text_to_utf8(text, text_len, NULL);
    } else {
        cue_sheet_free(out);
        char * gen = block ? text_from_block(block, block_len, rate, audio_path) : NULL;
        if (gen && cue_parse_text(gen, strlen(gen), NULL, audio_path, out) && out->track_count > 1) {
            src = CUE_SOURCE_EMBEDDED_BLOCK;
            if (text_out) { *text_out = gen; gen = NULL; }
        } else {
            cue_sheet_free(out);
        }
        free(gen);
    }
    free(text);
    free(block);
    return src;
}

/* ---- rewriting ------------------------------------------------------------------------ */

typedef struct {
    char * s;
    size_t n, cap;
    bool ok;
} sbuf_t;

static void sb_add(sbuf_t * b, const char * s, size_t n) {
    if (!b->ok) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->cap + n + 1) * 2;
        char * g = realloc(b->s, cap);
        if (!g) { b->ok = false; return; }
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

/* `indent`KEYWORD "value"`eol` (double quotes in the value become single ones). */
static void sb_command(sbuf_t * b, const char * indent, const char * keyword, const char * value, bool quoted,
                       const char * eol) {
    sb_add(b, indent, strlen(indent));
    sb_add(b, keyword, strlen(keyword));
    sb_add(b, " ", 1);
    if (quoted) sb_add(b, "\"", 1);
    for (const char * p = value; *p; p++) sb_add(b, *p == '"' ? "'" : p, 1);
    if (quoted) sb_add(b, "\"", 1);
    sb_add(b, eol, strlen(eol));
}

char * cue_rewrite(const char * text, const cue_edit_t * e) {
    sbuf_t b = { NULL, 0, 0, true };
    const char * eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    bool album_title = false, album_perf = false, album_date = false, album_genre = false;
    bool album_done = false; /* missing album lines inserted (before FILE / first TRACK) */
    int track = -1;          /* audio track index of the current TRACK block */
    bool in_audio = false, t_title = false, t_perf = false, t_flushed = false;
    const char * cur = text;
    if (library_utf8_bom_skip(cur) == 3) { sb_add(&b, cur, 3); cur += 3; }

    /* Adds what the current track block still lacks (called when it ends). */
#define FLUSH_TRACK()                                                                                              \
    do {                                                                                                         \
        if (in_audio && track >= 0 && track < e->track_count) {                                                  \
            if (!t_title && e->track_titles && e->track_titles[track])                                           \
                sb_command(&b, "    ", "TITLE", e->track_titles[track], true, eol);                              \
            if (!t_perf && e->track_performers && e->track_performers[track])                                    \
                sb_command(&b, "    ", "PERFORMER", e->track_performers[track], true, eol);                      \
        }                                                                                                        \
        t_title = t_perf = t_flushed = true;                                                                     \
    } while (0)
#define FLUSH_ALBUM()                                                                                              \
    do {                                                                                                         \
        if (!album_done) {                                                                                       \
            if (!album_date && e->date) sb_command(&b, "", "REM DATE", e->date, false, eol);                     \
            if (!album_genre && e->genre) sb_command(&b, "", "REM GENRE", e->genre, true, eol);                  \
            if (!album_perf && e->performer) sb_command(&b, "", "PERFORMER", e->performer, true, eol);           \
            if (!album_title && e->title) sb_command(&b, "", "TITLE", e->title, true, eol);                      \
            album_done = true;                                                                                   \
        }                                                                                                        \
    } while (0)

    while (*cur) {
        size_t ll = strcspn(cur, "\n");
        const char * next = cur + ll + (cur[ll] == '\n');
        size_t content = ll;
        if (content && cur[content - 1] == '\r') content--;
        char line[1024];
        size_t copy = content < sizeof(line) - 1 ? content : sizeof(line) - 1;
        memcpy(line, cur, copy);
        line[copy] = '\0';
        const char * p = line;
        while (*p == ' ' || *p == '\t') p++;
        char indent[32];
        size_t il = (size_t) (p - line) < sizeof(indent) - 1 ? (size_t) (p - line) : sizeof(indent) - 1;
        memcpy(indent, line, il);
        indent[il] = '\0';
        const char * arg;
        bool replaced = false;
        if (match_keyword(p, "FILE")) {
            FLUSH_ALBUM();
        } else if ((arg = match_keyword(p, "TRACK")) != NULL) {
            FLUSH_ALBUM();
            FLUSH_TRACK();
            const char * type = arg;
            while (*type && !isspace((unsigned char) *type)) type++;
            while (*type == ' ' || *type == '\t') type++;
            in_audio = strncasecmp(type, "AUDIO", 5) == 0;
            if (in_audio) track++;
            t_title = t_perf = t_flushed = false;
            sb_add(&b, cur, (size_t) (next - cur));
            if (cur[ll] != '\n') sb_add(&b, eol, strlen(eol));
            cur = next;
            continue;
        } else if (track < 0 && !album_done) {
            if (match_keyword(p, "TITLE")) {
                album_title = true;
                if (e->title) { sb_command(&b, indent, "TITLE", e->title, true, eol); replaced = true; }
            } else if (match_keyword(p, "PERFORMER")) {
                album_perf = true;
                if (e->performer) { sb_command(&b, indent, "PERFORMER", e->performer, true, eol); replaced = true; }
            } else if ((arg = match_keyword(p, "REM")) != NULL) {
                if (match_keyword(arg, "DATE")) {
                    album_date = true;
                    if (e->date) { sb_command(&b, indent, "REM DATE", e->date, false, eol); replaced = true; }
                } else if (match_keyword(arg, "GENRE")) {
                    album_genre = true;
                    if (e->genre) { sb_command(&b, indent, "REM GENRE", e->genre, true, eol); replaced = true; }
                }
            }
        } else if (in_audio && track >= 0 && track < e->track_count) {
            if (match_keyword(p, "TITLE")) {
                t_title = true;
                if (t_flushed && e->track_titles && e->track_titles[track]) {
                    replaced = true; /* already written before INDEX */
                } else if (e->track_titles && e->track_titles[track]) {
                    sb_command(&b, indent, "TITLE", e->track_titles[track], true, eol);
                    replaced = true;
                }
            } else if (match_keyword(p, "PERFORMER")) {
                t_perf = true;
                if (t_flushed && e->track_performers && e->track_performers[track]) {
                    replaced = true;
                } else if (e->track_performers && e->track_performers[track]) {
                    sb_command(&b, indent, "PERFORMER", e->track_performers[track], true, eol);
                    replaced = true;
                }
            } else if (match_keyword(p, "INDEX")) {
                FLUSH_TRACK(); /* names go before the first INDEX */
            }
        }
        if (!replaced) {
            sb_add(&b, cur, (size_t) (next - cur));
        }
        cur = next;
    }
    FLUSH_TRACK();
#undef FLUSH_TRACK
#undef FLUSH_ALBUM
    if (!b.ok) { free(b.s); return NULL; }
    if (!b.s) { b.s = calloc(1, 1); }
    return b.s;
}

void cue_sheet_free(cue_sheet_t * out) {
    if (!out) return;
    free(out->tracks);
    memset(out, 0, sizeof(*out));
}
