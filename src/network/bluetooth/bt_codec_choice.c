/* Per-device codec choice: see bt_codec_choice.h. */
#include "bt_codec_choice.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Preference order, best first. */
static const struct {
    unsigned int bit;
    const char * remote; /* bluetoothd's name */
    const char * choice; /* per-device choice / setting name */
} codecs[] = {
    { BT_CODEC_LDAC, "LDAC", "ldac" },
    { BT_CODEC_APTX, "APTX", "aptx" },
    { BT_CODEC_AAC, "AAC", "aac" },
    { BT_CODEC_SBC, "SBC", "sbc" },
};
#define CODEC_COUNT (sizeof(codecs) / sizeof(codecs[0]))

static bool valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!isxdigit((unsigned char) mac[i])) {
            return false;
        }
    }
    return true;
}

unsigned int bt_codec_parse_list(const char * list) {
    unsigned int bits = 0;
    while (list && *list) {
        size_t len = strcspn(list, ",");
        for (size_t i = 0; i < CODEC_COUNT; i++)
            if (len == strlen(codecs[i].remote) && strncasecmp(list, codecs[i].remote, len) == 0)
                bits |= codecs[i].bit;
        list += len;
        if (*list) list++;
    }
    return bits;
}

void bt_codec_format_list(unsigned int bits, char * out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        if (!(bits & codecs[i].bit)) continue;
        int n = snprintf(out + used, out_size - used, "%s%s", used ? "," : "", codecs[i].remote);
        if (n < 0 || (size_t) n >= out_size - used) {
            out[used] = '\0';
            return;
        }
        used += (size_t) n;
    }
}

bool bt_codec_parse_remote(const char * line, char mac[18], unsigned int * out_codecs, bool * complete) {
    char addr[18] = "", list[96] = "";
    int full = 0;
    if (!line || sscanf(line, "addr=%17s codecs=%95s complete=%d", addr, list, &full) < 2 || !valid_mac(addr))
        return false;
    snprintf(mac, 18, "%s", addr);
    if (out_codecs) *out_codecs = bt_codec_parse_list(list);
    if (complete) *complete = full == 1;
    return true;
}

bool bt_codec_is_ldac_quality(const char * name) {
    return name && (strcmp(name, "ldac_hq") == 0 || strcmp(name, "ldac_sq") == 0 ||
                    strcmp(name, "ldac_mq") == 0 || strcmp(name, "ldac_abr") == 0);
}

bool bt_codec_is_choice(const char * choice) {
    return choice && (strcmp(choice, "auto") == 0 || bt_codec_bit(choice) != 0) &&
           !bt_codec_is_ldac_quality(choice);
}

unsigned int bt_codec_bit(const char * name) {
    if (!name) return 0;
    if (bt_codec_is_ldac_quality(name)) return BT_CODEC_LDAC;
    for (size_t i = 0; i < CODEC_COUNT; i++)
        if (strcmp(name, codecs[i].choice) == 0) return codecs[i].bit;
    return 0;
}

static const char * setting_name(unsigned int bit, const char * ldac_quality) {
    if (bit == BT_CODEC_LDAC)
        return bt_codec_is_ldac_quality(ldac_quality) ? ldac_quality : BT_CODEC_LDAC_QUALITY_DEFAULT;
    for (size_t i = 0; i < CODEC_COUNT; i++)
        if (codecs[i].bit == bit) return codecs[i].choice;
    return NULL;
}

const char * bt_codec_resolve(unsigned int known, const char * choice, const char * ldac_quality) {
    unsigned int chosen = bt_codec_bit(choice);
    if (chosen && (known == 0 || (known & chosen))) return setting_name(chosen, ldac_quality);
    for (size_t i = 0; i < CODEC_COUNT; i++)
        if (known & codecs[i].bit) return setting_name(codecs[i].bit, ldac_quality);
    return NULL;
}

bool bt_codec_migrate_global(char * bt_codec, size_t bt_codec_size, char * ldac_quality,
                             size_t ldac_quality_size) {
    if (!bt_codec || !bt_codec[0] || strcmp(bt_codec, "auto") == 0) return false;
    if (bt_codec_is_ldac_quality(bt_codec)) snprintf(ldac_quality, ldac_quality_size, "%s", bt_codec);
    else if (strcmp(bt_codec, "ldac") == 0) snprintf(ldac_quality, ldac_quality_size, "ldac_sq");
    snprintf(bt_codec, bt_codec_size, "auto");
    return true;
}

/* ---- per-device table ------------------------------------------------------ */

bt_codec_device_t * bt_codec_store_find(bt_codec_store_t * store, const char * mac) {
    if (!store || !mac) return NULL;
    for (int i = 0; i < store->count; i++)
        if (strcasecmp(store->dev[i].mac, mac) == 0) return &store->dev[i];
    return NULL;
}

static bt_codec_device_t * store_add(bt_codec_store_t * store, const char * mac) {
    if (store->count >= BT_CODEC_DEVICES_MAX) {
        memmove(&store->dev[0], &store->dev[1], sizeof(store->dev[0]) * (BT_CODEC_DEVICES_MAX - 1));
        store->count = BT_CODEC_DEVICES_MAX - 1;
    }
    bt_codec_device_t * d = &store->dev[store->count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->mac, sizeof(d->mac), "%s", mac);
    snprintf(d->choice, sizeof(d->choice), "auto");
    return d;
}

void bt_codec_store_parse(bt_codec_store_t * store, const char * text) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
    while (text && *text) {
        size_t len = strcspn(text, "\n");
        char line[160];
        if (len < sizeof(line) && text[0] != '#') {
            memcpy(line, text, len);
            line[len] = '\0';
            char * mac = strtok(line, "\t\r");
            char * choice = mac ? strtok(NULL, "\t\r") : NULL;
            char * list = choice ? strtok(NULL, "\t\r") : NULL;
            if (valid_mac(mac) && bt_codec_is_choice(choice) && !bt_codec_store_find(store, mac)) {
                bt_codec_device_t * d = store_add(store, mac);
                snprintf(d->choice, sizeof(d->choice), "%s", choice);
                d->codecs = bt_codec_parse_list(list);
            }
        }
        text += len;
        if (*text) text++;
    }
}

size_t bt_codec_store_format(const bt_codec_store_t * store, char * out, size_t out_size) {
    if (!store || !out || !out_size) return 0;
    size_t used = 0;
    int n = snprintf(out, out_size, "# address\tcodec\tsupported\n");
    if (n < 0 || (size_t) n >= out_size) return 0;
    used = (size_t) n;
    for (int i = 0; i < store->count; i++) {
        char list[64];
        bt_codec_format_list(store->dev[i].codecs, list, sizeof(list));
        n = snprintf(out + used, out_size - used, "%s\t%s\t%s\n", store->dev[i].mac, store->dev[i].choice, list);
        if (n < 0 || (size_t) n >= out_size - used) return 0;
        used += (size_t) n;
    }
    return used;
}

bool bt_codec_store_set_choice(bt_codec_store_t * store, const char * mac, const char * choice) {
    if (!store || !valid_mac(mac)) return false;
    if (!choice) choice = "auto";
    if (!bt_codec_is_choice(choice)) return false;
    bt_codec_device_t * d = bt_codec_store_find(store, mac);
    if (!d) d = store_add(store, mac);
    else if (strcmp(d->choice, choice) == 0) return false;
    snprintf(d->choice, sizeof(d->choice), "%s", choice);
    return true;
}

bool bt_codec_store_note_remote(bt_codec_store_t * store, const char * mac, unsigned int known, bool complete) {
    if (!store || !valid_mac(mac) || known == 0) return false;
    bt_codec_device_t * d = bt_codec_store_find(store, mac);
    if (!d) d = store_add(store, mac);
    else {
        unsigned int merged = complete ? known : (d->codecs | known);
        if (merged == d->codecs) return false;
        known = merged;
    }
    d->codecs = known;
    return true;
}
