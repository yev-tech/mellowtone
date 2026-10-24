#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H
#include "board_config.h"
#include "sd_card.h"

#include <stdbool.h>
#include <stddef.h>

/* Two roots:
 *
 *   install root  BOARD_DATA_ROOT (flash): binary, fonts, theme, ALSA config,
 *                 boot files, the read-only base Radio.txt. Written only by
 *                 install and update; the running player writes just
 *                 MT_CHARGE_BASELINE_FILE there, once per device. On a
 *                 firmware-image install the read-only part (theme, fonts,
 *                 volume tables, factory player) is in the rootfs instead:
 *                 board_install_root() / board_asset_path() (board.h).
 *   state root    <SD card>/.mellowtone: everything the player writes.
 *
 *   .mellowtone/config/        settings, peq, bt/wifi power, stream and
 *                              server lists, radio_hidden.txt
 *   .mellowtone/plugins/       installed .lua plugins
 *   .mellowtone/plugin-data/<plugin id>/   plugin.storage and plugin.secrets
 *   .mellowtone/library/       metadata DB, path cache, queue.bin, remote_state
 *   .mellowtone/cache/         album art, stream covers, mp3 seek tables
 *   .mellowtone/logs/          player and launcher logs
 *   .mellowtone/backup/        retag backups, settings resets
 *   .mellowtone/update/        player updates waiting to be installed
 *   .mellowtone/card_id        this card's random identity (settings binding)
 *
 * Every state path is built here at compile time (MT_STATE_PATH("...")).
 * Writers call mt_state_prepare_write() first: it refuses when there is no
 * writable card, so nothing ever lands in flash or under an empty mount
 * point. The host simulator keeps its card in ./music. */

#define MT_STATE_DIR ".mellowtone"

#ifdef HOST_BUILD
#define MT_SD_ROOT "./music"
#define MT_INSTALL_ROOT "."
#else
#define MT_SD_ROOT BOARD_SD_ROOT
#define MT_INSTALL_ROOT BOARD_DATA_ROOT
#endif

#define MT_STATE_ROOT MT_SD_ROOT "/" MT_STATE_DIR
#define MT_STATE_PATH(rel) MT_STATE_ROOT "/" rel

#define MT_CONFIG_DIR MT_STATE_PATH("config")
#define MT_PLUGINS_DIR MT_STATE_PATH("plugins")
#define MT_PLUGIN_DATA_DIR MT_STATE_PATH("plugin-data")
#define MT_LIBRARY_DIR MT_STATE_PATH("library")
#define MT_CACHE_DIR MT_STATE_PATH("cache")
#define MT_LOGS_DIR MT_STATE_PATH("logs")
#define MT_BACKUP_DIR MT_STATE_PATH("backup")
#define MT_UPDATE_DIR MT_STATE_PATH("update")

/* The library directory relative to the card root, for openat() on a pinned
 * card descriptor (hot-swap safety), and the way back up to the root. */
#define MT_LIBRARY_REL MT_STATE_DIR "/library"
#define MT_LIBRARY_TO_SD_ROOT "../.."

#define MT_CARD_ID_FILE MT_STATE_PATH("card_id")

/* config/ */
#define SETTINGS_FILE_PATH MT_CONFIG_DIR "/settings.txt"
#define PEQ_FILE_PATH MT_CONFIG_DIR "/peq.txt"
#define MT_BT_POWER_FILE MT_CONFIG_DIR "/bt_power"
#define MT_BT_CODECS_FILE MT_CONFIG_DIR "/bt_codecs.tsv" /* per-device Bluetooth codec (R3 Pro) */
#define MT_WIFI_POWER_FILE MT_CONFIG_DIR "/wifi_power"
#define MT_WIFI_DNS_FILE MT_CONFIG_DIR "/wifi_dns" /* custom DNS servers, one per line */
#define MT_DEBUG_FLAG_FILE MT_CONFIG_DIR "/debug_logging"
#define MT_SUBSONIC_SERVERS_DIR MT_CONFIG_DIR
#define MT_STREAM_NAMES_FILE MT_CONFIG_DIR "/stream_server_names.tsv"
#define MT_DLNA_SERVERS_FILE MT_CONFIG_DIR "/dlna_servers.tsv"
#define MT_STREAM_ACTIVE_FILE MT_CONFIG_DIR "/stream_active.txt"
#define MT_DLNA_CONTINUE_FILE MT_CONFIG_DIR "/dlna_continue.txt"
#define MT_STREAM_CONTINUE_FILE MT_CONFIG_DIR "/stream_continue.txt"
#define MT_RADIO_HIDDEN_FILE MT_CONFIG_DIR "/radio_hidden.txt"

/* library/ */
#define MT_QUEUE_FILE MT_LIBRARY_DIR "/queue.bin"
/* A queue of local tracks that are not on the card (it used to live in flash). */
#define MT_QUEUE_INTERNAL_FILE MT_LIBRARY_DIR "/queue_internal.bin"

/* cache/ */
#define MT_ALBUMART_DIR MT_CACHE_DIR "/albumart"
#define MT_STREAM_COVERS_DIR MT_CACHE_DIR "/stream_covers"
#define MT_MP3SEEK_DIR MT_CACHE_DIR "/mp3seek"

/* backup/ */
#define MT_TAG_BACKUP_DIR MT_BACKUP_DIR "/tag_backup"

/* Radio: your stations on the card, the read-only base list in
 * the install root. */
#define MT_RADIO_SD_FILE MT_SD_ROOT "/Radio.txt"
#define MT_RADIO_BASE_FILE MT_INSTALL_ROOT "/Radio.txt"

/* The charger's own current setting from before the player's 500 mA cap
 * (charger_axp2101_i2c.c). It belongs to this device and must outlive a card
 * swap, a card-less boot or a lend, so it is the one file the player keeps
 * in the install root. */
#define MT_CHARGE_BASELINE_FILE MT_INSTALL_ROOT "/charge_baseline.txt"

/* ---- run time --------------------------------------------------------- */

/* Creates the state root and its subdirectories on a writable card
 * (state_reset.h). False without one. */
bool mt_state_create_layout(void);

/* Boot: reads the card state once. Call after the card is mounted and the
 * state root is created, before anything loads state. */
void mt_state_init(void);

/* A writable, supported card is mounted (cached for about a second). */
bool mt_state_available(void);
/* config/ may be written: the card is writable and this session's settings
 * belong to it (see mt_state_poll()). False while no card is bound: writes
 * are then kept as unsaved until mt_state_poll() binds one. */
bool mt_config_writable(void);

/* Creates the parent directories of path (which must be under
 * MT_STATE_ROOT). Returns false, without touching anything, when the card
 * is missing or read-only, or (for config/) while config is not writable. */
bool mt_state_prepare_write(const char * path);
/* mkdir -p for a directory under MT_STATE_ROOT, with the same checks. */
bool mt_state_mkdirs(const char * dir);
/* tmp + fsync + rename + directory fsync, after mt_state_prepare_write(). */
bool mt_state_write_file(const char * path, const void * data, size_t len);

/* Opens logs/<name> with open(2) flags (O_CREAT gets mode 0644), creating
 * logs/ first. -1 without a writable card: logs never go to flash. */
int mt_log_open(const char * name, int flags);

/* Opens (and with create, creates) <card root>/.mellowtone/library relative
 * to a pinned card root descriptor. -1 on failure. */
int mt_state_open_library_at(int sd_rootfd, bool create);

/* Pending saves: a writer that was refused calls mt_state_mark_unsaved(); the
 * registered hook runs once when a writable card appears. */
typedef enum {
    MT_PENDING_SETTINGS = 1u << 0,
    MT_PENDING_PEQ = 1u << 1,
} mt_pending_t;
void mt_state_mark_unsaved(mt_pending_t what);
void mt_state_set_flush_hook(mt_pending_t what, void (*hook)(void));

/* UI-thread poll (from the SD hotplug poll): refreshes the card state; when a
 * writable card appears it creates the state root, then writes pending settings
 * once. Removing the card unbinds config; the same card coming back (by
 * MT_CARD_ID_FILE, also when swapped between two polls) binds it again. Any
 * other card that already holds settings keeps them: config writes then
 * pause until a restart.
 * Returns true and the new state in *notice when the user should be told. */
typedef enum {
    MT_NOTICE_NONE = 0,
    MT_NOTICE_NO_CARD,
    MT_NOTICE_NTFS_READONLY,
    MT_NOTICE_READONLY_ERRORS,
    MT_NOTICE_UNSUPPORTED,
    MT_NOTICE_RESTART_FOR_SETTINGS,
} mt_notice_t;
mt_notice_t mt_state_poll(void);

/* Current card state (cached). SD_CARD_ABSENT while the card is lent. */
sd_card_state_t mt_state_card(void);

/* The card lent to a computer (USB storage): it is unmounted
 * and the computer writes it as a block device, so nothing here may touch
 * it. While lent, every writer sees no card (mt_state_available() false,
 * saves are marked unsaved and kept in memory), mt_state_poll() stays quiet,
 * and stdout/stderr -- the launcher points them at a log on the card -- go
 * to MT_LENT_LOG_FILE in RAM. Ending the lend moves the RAM log back onto
 * the card log (appended), refreshes the card state and writes what was
 * kept in memory. The caller (usb_storage_lend.c) closes the files it knows
 * are open before lending. UI thread. */
#define MT_LENT_LOG_FILE "/tmp/mellowtone-usb-storage.log"
void mt_state_set_card_lent(bool lent);
bool mt_state_card_lent(void);

/* Mellowtone settings reset: moves .mellowtone/config to
 * .mellowtone/backup/reset-<date>/. Library, cache and music stay. Returns
 * false if there was a config directory that could not be moved. */
bool mt_state_reset_config(void);

#endif
