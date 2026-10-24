/* The R3 Pro's per-device codec choice (bt_codec_choice.c): parsing
 * bluetoothd's /tmp/bt_a2dp_remote, Auto's order, a device's own choice and
 * when it is overridden, the config/bt_codecs.tsv table, and the migration
 * of the old global setting. */
#include "bt_codec_choice.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            fprintf(stderr, __VA_ARGS__);                                                                          \
            fputc('\n', stderr);                                                                                   \
        }                                                                                                          \
    } while (0)

static bool same(const char * a, const char * b) {
    return a == b || (a && b && strcmp(a, b) == 0);
}

static const char * shown(const char * s) {
    return s ? s : "NULL";
}

#define CHECK_STR(got, want)                                                                                       \
    do {                                                                                                           \
        const char * got_ = (got), * want_ = (want);                                                               \
        CHECK(same(got_, want_), "got %s, want %s", shown(got_), shown(want_));                                    \
    } while (0)

#define XM4 "94:DB:56:01:02:03"
#define CAR "00:1E:43:AA:BB:CC"

static void test_lists(void) {
    CHECK(bt_codec_parse_list("LDAC,APTX,AAC,SBC") == (BT_CODEC_LDAC | BT_CODEC_APTX | BT_CODEC_AAC | BT_CODEC_SBC),
          "full list");
    CHECK(bt_codec_parse_list("SBC") == BT_CODEC_SBC, "SBC only");
    CHECK(bt_codec_parse_list("aptX,sbc") == (BT_CODEC_APTX | BT_CODEC_SBC), "any case");
    CHECK(bt_codec_parse_list("UAT,UAT_TWS,MPEG12,SBC") == BT_CODEC_SBC, "UAT and MPEG-1/2 are skipped");
    CHECK(bt_codec_parse_list("") == 0 && bt_codec_parse_list(NULL) == 0, "empty");
    CHECK(bt_codec_parse_list("SBCX,LDA,,AAC") == BT_CODEC_AAC, "whole names only");

    char out[64];
    bt_codec_format_list(BT_CODEC_SBC | BT_CODEC_LDAC | BT_CODEC_AAC, out, sizeof(out));
    CHECK_STR(out, "LDAC,AAC,SBC");
    bt_codec_format_list(0, out, sizeof(out));
    CHECK_STR(out, "");
    bt_codec_format_list(BT_CODEC_LDAC | BT_CODEC_APTX, out, 8);
    CHECK_STR(out, "LDAC");

    char mac[18];
    unsigned int codecs = 0;
    bool complete = false;
    CHECK(bt_codec_parse_remote("addr=" XM4 " codecs=SBC,AAC,LDAC complete=1\n", mac, &codecs, &complete),
          "remote line");
    CHECK_STR(mac, XM4);
    CHECK(codecs == (BT_CODEC_SBC | BT_CODEC_AAC | BT_CODEC_LDAC) && complete, "codecs 0x%x complete %d", codecs,
          complete);
    CHECK(bt_codec_parse_remote("addr=" CAR " codecs=SBC complete=0\n", mac, &codecs, &complete), "partial line");
    CHECK(codecs == BT_CODEC_SBC && !complete, "partial");
    CHECK(bt_codec_parse_remote("addr=" CAR " codecs=SBC\n", mac, &codecs, &complete) && !complete,
          "no complete= is partial");
    CHECK(!bt_codec_parse_remote("addr=nonsense codecs=SBC complete=1", mac, &codecs, &complete), "bad address");
    CHECK(!bt_codec_parse_remote("", mac, &codecs, &complete), "empty line");
}

static void test_auto_order(void) {
    unsigned int all = BT_CODEC_LDAC | BT_CODEC_APTX | BT_CODEC_AAC | BT_CODEC_SBC;
    CHECK_STR(bt_codec_resolve(all, "auto", "ldac_hq"), "ldac_hq");
    CHECK_STR(bt_codec_resolve(all, "auto", "ldac_abr"), "ldac_abr");
    CHECK_STR(bt_codec_resolve(BT_CODEC_LDAC | BT_CODEC_AAC | BT_CODEC_SBC, NULL, "ldac_sq"), "ldac_sq");
    CHECK_STR(bt_codec_resolve(BT_CODEC_APTX | BT_CODEC_AAC | BT_CODEC_SBC, "auto", "ldac_hq"), "aptx");
    CHECK_STR(bt_codec_resolve(BT_CODEC_AAC | BT_CODEC_SBC, "auto", "ldac_hq"), "aac");
    CHECK_STR(bt_codec_resolve(BT_CODEC_SBC, "auto", "ldac_hq"), "sbc");
    CHECK_STR(bt_codec_resolve(0, "auto", "ldac_hq"), NULL);
    /* A quality that is not an LDAC mode falls back to the default. */
    CHECK_STR(bt_codec_resolve(BT_CODEC_LDAC, "auto", "aac"), BT_CODEC_LDAC_QUALITY_DEFAULT);
    CHECK_STR(bt_codec_resolve(BT_CODEC_LDAC, "", NULL), BT_CODEC_LDAC_QUALITY_DEFAULT);
}

static void test_override(void) {
    unsigned int xm4 = BT_CODEC_LDAC | BT_CODEC_AAC | BT_CODEC_SBC;
    CHECK_STR(bt_codec_resolve(xm4, "aac", "ldac_hq"), "aac");
    CHECK_STR(bt_codec_resolve(xm4, "sbc", "ldac_hq"), "sbc");
    CHECK_STR(bt_codec_resolve(xm4, "ldac", "ldac_mq"), "ldac_mq");
    /* A choice the device does not have: Auto. */
    CHECK_STR(bt_codec_resolve(xm4, "aptx", "ldac_hq"), "ldac_hq");
    CHECK_STR(bt_codec_resolve(BT_CODEC_SBC, "ldac", "ldac_hq"), "sbc");
    /* Nothing known: the choice as it is (it came from the device's list). */
    CHECK_STR(bt_codec_resolve(0, "aac", "ldac_hq"), "aac");
    CHECK_STR(bt_codec_resolve(0, "ldac", "ldac_sq"), "ldac_sq");
    /* Unknown choices are Auto. */
    CHECK_STR(bt_codec_resolve(xm4, "sbc_xq", "ldac_hq"), "ldac_hq");

    CHECK(bt_codec_is_choice("auto") && bt_codec_is_choice("ldac") && bt_codec_is_choice("sbc"), "choices");
    CHECK(!bt_codec_is_choice("ldac_hq") && !bt_codec_is_choice("sbc_xq") && !bt_codec_is_choice(NULL), "not choices");
    CHECK(bt_codec_bit("ldac_abr") == BT_CODEC_LDAC && bt_codec_bit("aptx") == BT_CODEC_APTX &&
              bt_codec_bit("auto") == 0,
          "bits");
}

static void test_store(void) {
    bt_codec_store_t store;
    bt_codec_store_parse(&store,
                         "# address\tcodec\tsupported\n" XM4 "\taac\tLDAC,AAC,SBC\n"
                         "garbage line\n"
                         "11:22:33:44:55:66\tsbc_xq\tSBC\n" /* not a choice: dropped */
                         CAR "\tauto\tSBC\r\n" XM4 "\tsbc\tSBC\n" /* duplicate: first wins */
                         "AA:BB:CC:DD:EE:FF\tauto");          /* no list, no newline */
    CHECK(store.count == 3, "count %d", store.count);
    bt_codec_device_t * d = bt_codec_store_find(&store, "94:db:56:01:02:03");
    CHECK(d && same(d->choice, "aac") && d->codecs == (BT_CODEC_LDAC | BT_CODEC_AAC | BT_CODEC_SBC), "xm4 entry");
    d = bt_codec_store_find(&store, CAR);
    CHECK(d && same(d->choice, "auto") && d->codecs == BT_CODEC_SBC, "car entry (CRLF)");
    d = bt_codec_store_find(&store, "AA:BB:CC:DD:EE:FF");
    CHECK(d && d->codecs == 0, "entry without a list");

    /* Choice changes report whether anything changed (no needless writes). */
    CHECK(!bt_codec_store_set_choice(&store, XM4, "aac"), "same choice");
    CHECK(bt_codec_store_set_choice(&store, XM4, NULL), "back to auto");
    CHECK(!bt_codec_store_set_choice(&store, XM4, "ldac_hq"), "not a choice");
    CHECK(!bt_codec_store_set_choice(&store, "bad", "aac"), "bad address");

    /* Reports: complete replaces, partial adds, nothing new is no change. */
    CHECK(!bt_codec_store_note_remote(&store, XM4, BT_CODEC_LDAC, false), "partial already known");
    CHECK(bt_codec_store_note_remote(&store, XM4, BT_CODEC_LDAC | BT_CODEC_SBC, true), "complete replaces");
    CHECK(bt_codec_store_find(&store, XM4)->codecs == (BT_CODEC_LDAC | BT_CODEC_SBC), "replaced");
    CHECK(bt_codec_store_note_remote(&store, XM4, BT_CODEC_AAC, false), "partial adds");
    CHECK(bt_codec_store_find(&store, XM4)->codecs == (BT_CODEC_LDAC | BT_CODEC_AAC | BT_CODEC_SBC), "added");
    CHECK(!bt_codec_store_note_remote(&store, XM4, 0, true), "empty report ignored");
    CHECK(bt_codec_store_note_remote(&store, "01:02:03:04:05:06", BT_CODEC_SBC, false), "new device");
    CHECK(store.count == 4, "count %d", store.count);

    /* Round trip. */
    char text[4096];
    size_t n = bt_codec_store_format(&store, text, sizeof(text));
    CHECK(n > 0 && n == strlen(text), "format");
    CHECK(strstr(text, XM4 "\tauto\tLDAC,AAC,SBC\n") != NULL, "xm4 line: %s", text);
    bt_codec_store_t again;
    bt_codec_store_parse(&again, text);
    CHECK(again.count == store.count && memcmp(again.dev, store.dev, sizeof(store.dev[0]) * store.count) == 0,
          "round trip");
    CHECK(bt_codec_store_format(&store, text, 20) == 0, "too small");

    /* Full: the oldest entry goes. */
    bt_codec_store_parse(&store, "");
    for (int i = 0; i < BT_CODEC_DEVICES_MAX + 1; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "00:00:00:00:00:%02X", i);
        CHECK(bt_codec_store_set_choice(&store, mac, "sbc"), "add %d", i);
    }
    CHECK(store.count == BT_CODEC_DEVICES_MAX, "count %d", store.count);
    CHECK(!bt_codec_store_find(&store, "00:00:00:00:00:00") && bt_codec_store_find(&store, "00:00:00:00:00:20"),
          "oldest dropped");
}

static void test_migration(void) {
    char codec[16], quality[16];
    struct {
        const char * old;
        const char * quality; /* expected; NULL = unchanged */
        bool changed;
    } cases[] = {
        { "auto", NULL, false }, { "", NULL, false },         { "ldac_hq", "ldac_hq", true },
        { "ldac_sq", "ldac_sq", true }, { "ldac_mq", "ldac_mq", true }, { "ldac_abr", "ldac_abr", true },
        { "ldac", "ldac_sq", true },    { "aptx", NULL, true },         { "aac", NULL, true },
        { "sbc", NULL, true },          { "sbc_xq", NULL, true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(codec, sizeof(codec), "%s", cases[i].old);
        snprintf(quality, sizeof(quality), "ldac_mq");
        bool changed = bt_codec_migrate_global(codec, sizeof(codec), quality, sizeof(quality));
        CHECK(changed == cases[i].changed, "%s: changed %d", cases[i].old, changed);
        CHECK_STR(quality, cases[i].quality ? cases[i].quality : "ldac_mq");
        CHECK_STR(codec, cases[i].changed ? "auto" : cases[i].old);
        /* Once migrated, nothing more happens. */
        CHECK(!bt_codec_migrate_global(codec, sizeof(codec), quality, sizeof(quality)), "%s: second run",
              cases[i].old);
    }
}

int main(void) {
    test_lists();
    test_auto_order();
    test_override();
    test_store();
    test_migration();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
