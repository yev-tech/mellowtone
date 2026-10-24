#define _GNU_SOURCE
#include "board_config.h"
#include "sd_card.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool unescape_field(const char * in, size_t length, char * out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value;
        if (in[i] == '\\' && i + 3 < length &&
            in[i + 1] >= '0' && in[i + 1] <= '7' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            value = (unsigned char) ((in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 + (in[i + 3] - '0'));
            i += 3;
        } else {
            value = (unsigned char) in[i];
        }
        if (written + 1 >= out_size) return false;
        out[written++] = (char) value;
    }
    out[written] = '\0';
    return true;
}

static bool option_token(const char * options, const char * token) {
    size_t token_len = strlen(token);
    const char * cursor = options;
    while (*cursor) {
        const char * comma = strchr(cursor, ',');
        size_t length = comma ? (size_t) (comma - cursor) : strlen(cursor);
        if (length == token_len && memcmp(cursor, token, token_len) == 0) return true;
        if (!comma) break;
        cursor = comma + 1;
    }
    return false;
}

static bool sd_mount_point(const char * path, const char * alt) {
    return strcmp(path, BOARD_SD_ROOT "") == 0 || (alt && alt[0] && strcmp(path, alt) == 0);
}

sd_fs_kind_t sd_card_kind_from_type(const char * fstype) {
    if (!fstype || !fstype[0]) return SD_FS_KIND_UNKNOWN;
    if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "msdos") == 0 || strcmp(fstype, "fat") == 0)
        return SD_FS_KIND_VFAT;
    if (strcmp(fstype, "exfat") == 0 || strcmp(fstype, "fuse.exfat") == 0) return SD_FS_KIND_EXFAT;
    if (strcmp(fstype, "ntfs") == 0 || strcmp(fstype, "ntfs3") == 0 || strcmp(fstype, "fuseblk") == 0 ||
        strcmp(fstype, "fuse.ntfs-3g") == 0 || strcmp(fstype, "ntfs-3g") == 0)
        return SD_FS_KIND_NTFS;
    /* Vendor drivers ("sdfat", "texfat", ...) for the same formats. */
    if (strstr(fstype, "exfat")) return SD_FS_KIND_EXFAT;
    if (strstr(fstype, "fat")) return SD_FS_KIND_VFAT;
    return SD_FS_KIND_UNKNOWN;
}

bool sd_card_parse_mounts(const char * mounts_text, sd_mount_info_t * out) {
    return sd_card_parse_mounts_at(mounts_text, NULL, out);
}

bool sd_card_parse_mounts_at(const char * mounts_text, const char * alt_mount_point, sd_mount_info_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!mounts_text) return false;

    const char * line = mounts_text;
    while (*line) {
        const char * next = strchr(line, '\n');
        size_t line_len = next ? (size_t) (next - line) : strlen(line);
        const char * fields[4];
        size_t field_len[4];
        size_t count = 0;
        size_t start = 0;
        for (size_t i = 0; i <= line_len && count < 4; i++) {
            bool boundary = i == line_len || line[i] == ' ' || line[i] == '\t';
            if (!boundary) continue;
            if (i > start) {
                fields[count] = line + start;
                field_len[count] = i - start;
                count++;
            }
            start = i + 1;
        }
        char device[SD_CARD_DEVICE_BYTES];
        char mount_point[SD_CARD_MOUNT_BYTES];
        char fstype[SD_CARD_FSTYPE_BYTES];
        char options[512];
        if (count == 4 &&
            unescape_field(fields[0], field_len[0], device, sizeof(device)) &&
            unescape_field(fields[1], field_len[1], mount_point, sizeof(mount_point)) &&
            unescape_field(fields[2], field_len[2], fstype, sizeof(fstype)) &&
            unescape_field(fields[3], field_len[3], options, sizeof(options)) &&
            sd_mount_point(mount_point, alt_mount_point)) {
            out->found = true;
            out->readonly = option_token(options, "ro") && !option_token(options, "rw");
            out->kind = sd_card_kind_from_type(fstype);
            snprintf(out->device, sizeof(out->device), "%s", device);
            snprintf(out->mount_point, sizeof(out->mount_point), "%s", mount_point);
            snprintf(out->fstype, sizeof(out->fstype), "%s", fstype);
        }
        if (!next) break;
        line = next + 1;
    }
    return out->found;
}

sd_card_state_t sd_card_classify(const sd_mount_info_t * info) {
    if (!info || !info->found) return SD_CARD_ABSENT;
    switch (info->kind) {
    case SD_FS_KIND_VFAT:
    case SD_FS_KIND_EXFAT:
        return info->readonly ? SD_CARD_READONLY_ERRORS : SD_CARD_OK;
    case SD_FS_KIND_NTFS:
        /* The R3 Pro kernel's ntfs driver is read-only and mounts "ro"; the
         * X1600 boards mount NTFS read-write through ntfs-3g. */
        return info->readonly ? SD_CARD_NTFS_READONLY : SD_CARD_OK;
    default:
        return SD_CARD_UNSUPPORTED;
    }
}

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static sd_mount_info_t last_info;
static sd_card_state_t last_state = SD_CARD_ABSENT;
static bool have_state;
static struct timespec last_refresh;

#ifndef HOST_BUILD
/* The mount point differs from its parent's device: something is mounted. */
static bool mount_point_attached(void) {
    struct stat parent_st, root_st;
    if (stat(BOARD_SD_PARENT, &parent_st) != 0) return false;
    if (stat(BOARD_SD_ROOT "", &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}
#endif

static sd_card_state_t probe(sd_mount_info_t * info) {
    memset(info, 0, sizeof(*info));
#ifdef HOST_BUILD
    /* The simulator's "card" is ./music. */
    struct stat st;
    if (stat("./music", &st) != 0 || !S_ISDIR(st.st_mode)) return SD_CARD_ABSENT;
    info->found = true;
    info->kind = SD_FS_KIND_VFAT;
    info->readonly = access("./music", W_OK) != 0;
    snprintf(info->mount_point, sizeof(info->mount_point), "./music");
    snprintf(info->fstype, sizeof(info->fstype), "host");
    return sd_card_classify(info);
#else
    char * text = NULL;
    FILE * f = fopen("/proc/mounts", "r");
    if (f) {
        size_t cap = 16384, len = 0;
        text = malloc(cap + 1);
        if (text) {
            size_t n;
            while ((n = fread(text + len, 1, cap - len, f)) > 0) {
                len += n;
                if (len == cap) {
                    char * grown = realloc(text, cap * 2 + 1);
                    if (!grown) break;
                    text = grown;
                    cap *= 2;
                }
            }
            text[len] = '\0';
        }
        fclose(f);
    }
    char real[PATH_MAX];
    const char * alt = realpath(BOARD_SD_ROOT "", real) ? real : NULL;
    bool parsed = text && sd_card_parse_mounts_at(text, alt, info);
    free(text);
    if (!parsed) {
        /* Not listed under the expected name, yet something is mounted
         * there: trust the mount, and let write access decide. */
        if (!mount_point_attached()) return SD_CARD_ABSENT;
        info->found = true;
        info->readonly = access(BOARD_SD_ROOT "", W_OK) != 0;
        info->kind = SD_FS_KIND_UNKNOWN;
        snprintf(info->mount_point, sizeof(info->mount_point), "%s", BOARD_SD_ROOT);
        return info->readonly ? SD_CARD_READONLY_ERRORS : SD_CARD_OK;
    }
    /* A stale line for a pulled card can outlive the card itself. */
    if (!mount_point_attached()) {
        memset(info, 0, sizeof(*info));
        return SD_CARD_ABSENT;
    }
    return sd_card_classify(info);
#endif
}

sd_card_state_t sd_card_refresh(void) {
    sd_mount_info_t info;
    sd_card_state_t state = probe(&info);
    pthread_mutex_lock(&state_mutex);
    last_info = info;
    last_state = state;
    have_state = true;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);
    pthread_mutex_unlock(&state_mutex);
    return state;
}

sd_card_state_t sd_card_state_fresh(unsigned max_age_ms) {
    pthread_mutex_lock(&state_mutex);
    bool fresh = false;
    sd_card_state_t state = last_state;
    if (have_state && max_age_ms > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long age_ms = (long long) (now.tv_sec - last_refresh.tv_sec) * 1000 +
                           (now.tv_nsec - last_refresh.tv_nsec) / 1000000;
        fresh = age_ms >= 0 && age_ms < (long long) max_age_ms;
    }
    pthread_mutex_unlock(&state_mutex);
    return fresh ? state : sd_card_refresh();
}

void sd_card_last_info(sd_mount_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&state_mutex);
    *out = last_info;
    pthread_mutex_unlock(&state_mutex);
}
