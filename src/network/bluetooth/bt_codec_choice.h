#ifndef MT_BT_CODEC_CHOICE_H
#define MT_BT_CODEC_CHOICE_H

/* Per-device codec choice for a back end with one system-wide codec setting
 * (the R3 Pro: audio.conf DefaultCodec, which HiBy's ALSA plugin reads on
 * every stream open). Pure logic, no I/O: the caller reads and writes the
 * files.
 *
 * Auto takes the best codec the device has, LDAC > aptX > AAC > SBC (UAT is
 * never chosen), LDAC at the global LDAC quality. A device can instead be
 * given one of its codecs; that choice is kept per address in
 * config/bt_codecs.tsv. */

#include <stdbool.h>
#include <stddef.h>

enum {
    BT_CODEC_SBC = 1u << 0,
    BT_CODEC_AAC = 1u << 1,
    BT_CODEC_APTX = 1u << 2,
    BT_CODEC_LDAC = 1u << 3,
};

#define BT_CODEC_LDAC_QUALITY_DEFAULT "ldac_hq"
#define BT_CODEC_DEVICES_MAX 32

/* "LDAC,APTX,AAC,SBC" (bluetoothd's /tmp/bt_a2dp_remote names, any case,
 * "aptX" too) -> BT_CODEC_* bits; unknown names (UAT, MPEG12) are skipped. */
unsigned int bt_codec_parse_list(const char * list);
/* The bits as a list in preference order ("LDAC,APTX,AAC,SBC"); "" for 0. */
void bt_codec_format_list(unsigned int codecs, char * out, size_t out_size);

/* One /tmp/bt_a2dp_remote line: "addr=<mac> codecs=<list> complete=<0|1>".
 * complete=0 (or missing): only the codec the device configured itself is
 * known. False when the line has no valid address. */
bool bt_codec_parse_remote(const char * line, char mac[18], unsigned int * codecs, bool * complete);

/* "ldac_hq"/"ldac_sq"/"ldac_mq"/"ldac_abr". */
bool bt_codec_is_ldac_quality(const char * name);
/* A per-device choice: "auto", "ldac", "aptx", "aac", "sbc". */
bool bt_codec_is_choice(const char * choice);
/* The BT_CODEC_* bit of a choice or of a codec setting name ("ldac_sq",
 * "aptx", ...); 0 for "auto" and unknown names. */
unsigned int bt_codec_bit(const char * name);

/* The codec to configure (a bt_control_set_codec() name: "ldac_hq",
 * "aptx", "aac", "sbc"), or NULL when nothing is known about the device.
 * A choice the device does not list falls back to Auto; with nothing known
 * the choice is used as it is (it was picked from the device's own list). */
const char * bt_codec_resolve(unsigned int codecs, const char * choice, const char * ldac_quality);

/* The old global preference (settings bt_codec): an LDAC mode becomes the
 * LDAC quality, anything else is dropped; *bt_codec becomes "auto". True
 * when something changed. */
bool bt_codec_migrate_global(char * bt_codec, size_t bt_codec_size, char * ldac_quality, size_t ldac_quality_size);

/* ---- per-device table (config/bt_codecs.tsv) ---------------------------- */

typedef struct {
    char mac[18];
    char choice[8];      /* "auto" or a codec family */
    unsigned int codecs; /* what the device has, as far as known */
} bt_codec_device_t;

typedef struct {
    bt_codec_device_t dev[BT_CODEC_DEVICES_MAX];
    int count;
} bt_codec_store_t;

/* Lines "<mac>\t<choice>\t<codecs>"; '#' lines and bad lines are skipped. */
void bt_codec_store_parse(bt_codec_store_t * store, const char * text);
/* The file contents; the length written (0 if it did not fit). */
size_t bt_codec_store_format(const bt_codec_store_t * store, char * out, size_t out_size);
bt_codec_device_t * bt_codec_store_find(bt_codec_store_t * store, const char * mac);
/* Sets the device's choice ("auto" when choice is NULL); true when changed.
 * A new device past the limit replaces the oldest entry. */
bool bt_codec_store_set_choice(bt_codec_store_t * store, const char * mac, const char * choice);
/* What bluetoothd reported: a complete list replaces the known one, a partial
 * one is added to it. True when the table changed. */
bool bt_codec_store_note_remote(bt_codec_store_t * store, const char * mac, unsigned int codecs, bool complete);

#endif /* MT_BT_CODEC_CHOICE_H */
