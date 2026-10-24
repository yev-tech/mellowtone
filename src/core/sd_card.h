#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

/* SD card check: is a card mounted, which file system, can we
 * write to it. The parser below does no I/O (the caller passes /proc/mounts
 * text, so it is unit-testable); sd_card_refresh() does the reading. */

#define SD_CARD_DEVICE_BYTES 96
#define SD_CARD_MOUNT_BYTES 160
#define SD_CARD_FSTYPE_BYTES 32

typedef enum {
    SD_FS_KIND_UNKNOWN = 0,
    SD_FS_KIND_VFAT,
    SD_FS_KIND_EXFAT,
    SD_FS_KIND_NTFS,
} sd_fs_kind_t;

typedef struct {
    bool found;
    bool readonly;
    sd_fs_kind_t kind;
    char device[SD_CARD_DEVICE_BYTES];
    char mount_point[SD_CARD_MOUNT_BYTES];
    char fstype[SD_CARD_FSTYPE_BYTES];
} sd_mount_info_t;

/* What the player may do with the card. */
typedef enum {
    SD_CARD_ABSENT = 0,      /* no card mounted: state stays in memory */
    SD_CARD_OK,              /* FAT32 / exFAT (or a writable NTFS-3G mount): full support */
    SD_CARD_NTFS_READONLY,   /* NTFS through a read-only driver: music plays, nothing is saved */
    SD_CARD_READONLY_ERRORS, /* FAT/exFAT mounted read-only (the kernel does that after errors) */
    SD_CARD_UNSUPPORTED,     /* any other file system: nothing is saved */
} sd_card_state_t;

/* Last matching SD mount line wins. Matches the board's SD mount point
 * (BOARD_SD_ROOT) or alt_mount_point (the resolved real path of it, when the
 * mount point sits behind a symlink; NULL or "" to skip). A line is
 * read-only only when its option list has an exact "ro" token and no "rw"
 * token. */
bool sd_card_parse_mounts_at(const char * mounts_text, const char * alt_mount_point, sd_mount_info_t * out);
bool sd_card_parse_mounts(const char * mounts_text, sd_mount_info_t * out);

sd_fs_kind_t sd_card_kind_from_type(const char * fstype);

/* Pure classification of a parsed mount (no I/O). */
sd_card_state_t sd_card_classify(const sd_mount_info_t * info);

/* Re-reads /proc/mounts (on the host: checks that ./music exists) and
 * returns the current state. Thread-safe. */
sd_card_state_t sd_card_refresh(void);
/* Cached state; refreshes when the cached value is older than max_age_ms
 * (0 = always refresh). Thread-safe, cheap enough for every state write. */
sd_card_state_t sd_card_state_fresh(unsigned max_age_ms);
/* Copy of the last mount info sd_card_refresh() saw. */
void sd_card_last_info(sd_mount_info_t * out);

static inline bool sd_card_state_writable(sd_card_state_t state) {
    return state == SD_CARD_OK;
}

static inline bool sd_card_state_readable(sd_card_state_t state) {
    return state != SD_CARD_ABSENT;
}

#endif
