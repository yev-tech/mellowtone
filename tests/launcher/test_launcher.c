/* mellowtone-launcher core on the host: fake clock,
 * processes, keys and battery; a memory framebuffer; temporary directories
 * for the slots, the boot state, the card and the factory copy.
 *
 *   fallback chain (crash x3 -> previous -> factory -> menu), boot-loop
 *   counter, one-shot menu flag, menu by keys and touch, update packages
 *   (install, wrong board, corrupt, power loss at every step), rollback,
 *   firmware images (R3 Pro UPDATE.TXT and X1600 OTA; low battery, md5
 *   mismatch, wrong model, confirm twice), settings reset, ADB. */
#define _GNU_SOURCE
#include "lx.h"
#include "lx_fb.h"
#include "lx_update.h"
#include "board_launcher.h"
#include "state_reset.h"

#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
            fprintf(stderr, __VA_ARGS__);                                                                         \
            fputc('\n', stderr);                                                                                  \
        }                                                                                                         \
    } while (0)

static char root[PATH_MAX];
static char p_slots[PATH_MAX], p_boot[PATH_MAX], p_sd[PATH_MAX], p_install[PATH_MAX], p_logs[PATH_MAX],
    p_factory[PATH_MAX], p_redeploy[PATH_MAX], p_adb[PATH_MAX];

/* ---- files ------------------------------------------------------------------ */

static void write_bytes(const char * path, const void * data, size_t n, mode_t mode) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        lx_mkdir_p(dir);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, data, n) != (ssize_t) n) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    close(fd);
    chmod(path, mode);
}

static void write_text(const char * path, const char * text) {
    write_bytes(path, text, strlen(text), 0644);
}

static char * pathf(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static char * pathf(const char * fmt, ...) {
    static char bufs[8][PATH_MAX];
    static int i;
    char * b = bufs[i++ % 8];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, PATH_MAX, fmt, ap);
    va_end(ap);
    return b;
}

/* The card's backup of the previous version. */
static char * backup_dir(void) {
    return pathf("%s/.mellowtone/backup/player", p_sd);
}

static void make_tree(const char * dir, const char * version) {
    write_bytes(pathf("%s/%s", dir, LX_PLAYER_EXE), "#!/bin/sh\n", 10, 0755);
    write_text(pathf("%s/%s", dir, LX_MANIFEST), pathf("mellowtone-package 1\nboard r3pro\nversion %s\n", version));
}

/* "current" in flash; "previous" is the backup on the card. */
static void make_slot(const char * slot, const char * version) {
    make_tree(strcmp(slot, "previous") == 0 ? backup_dir() : pathf("%s/%s", p_slots, slot), version);
}

static void reset_tree(void) {
    lx_rm_rf(root);
    lx_mkdir_p(root);
    lx_mkdir_p(p_slots);
    lx_mkdir_p(p_sd);
    lx_mkdir_p(p_install);
    lx_mkdir_p(p_logs);
}

static bool file_contains(const char * path, const char * needle) {
    static char buf[65536];
    return lx_read_file(path, buf, sizeof(buf)) && strstr(buf, needle);
}

/* ---- fake platform -------------------------------------------------------------- */

typedef struct {
    int64_t run_ms; /* how long it runs */
    int status;     /* wait status when it exits */
    bool set_menu_flag; /* the player asks for the menu before exiting */
    bool redeploy;
} outcome_t;

#define EXIT(code) ((code) << 8)
#define SIG(n) (n)

static struct {
    int64_t now;
    outcome_t script[64];
    int nscript, next;
    char spawned[64][PATH_MAX];
    char slots[64][16];
    char notices[64][32];
    int counter_at_spawn[64];
    int nspawned;
    outcome_t cur;
    lx_key_t keys[64];
    int tap_y[64];
    int nkeys, nextkey;
    int battery;
    int recoveries, adbs;
    bool key_starved;
    lx_fb_t * fb;
} fake;

static lx_config_t cfg;

static int64_t f_now(void * c) { (void) c; return fake.now; }
static void f_sleep(void * c, int ms) { (void) c; fake.now += ms; }

static long f_spawn(void * c, const char * path, const char * cwd, const char * log, const char * slot, const char * notice) {
    (void) c; (void) cwd; (void) log;
    int i = fake.nspawned++;
    snprintf(fake.spawned[i], sizeof(fake.spawned[i]), "%s", path);
    snprintf(fake.slots[i], sizeof(fake.slots[i]), "%s", slot ? slot : "");
    snprintf(fake.notices[i], sizeof(fake.notices[i]), "%s", notice ? notice : "");
    fake.counter_at_spawn[i] = lx_boot_counter_read(&cfg);
    fake.cur = fake.next < fake.nscript ? fake.script[fake.next++] : (outcome_t) { 1000, EXIT(0), false, false };
    return i + 1;
}

static bool f_wait(void * c, long h, int timeout_ms, int * status) {
    (void) c; (void) h;
    if (timeout_ms >= 0 && fake.cur.run_ms > timeout_ms) {
        fake.now += timeout_ms;
        fake.cur.run_ms -= timeout_ms;
        return false;
    }
    fake.now += fake.cur.run_ms;
    if (fake.cur.set_menu_flag) {
        lx_mkdir_p(p_boot);
        write_text(pathf("%s/%s", p_boot, BOARD_LAUNCHER_MENU_FLAG), "");
    }
    if (fake.cur.redeploy) write_text(p_redeploy, "");
    *status = fake.cur.status;
    return true;
}

static bool f_recovery(void * c) { (void) c; fake.recoveries++; return false; }
static bool f_adb(void * c) { (void) c; fake.adbs++; return true; }
static int f_battery(void * c) { (void) c; return fake.battery; }

static lx_key_t f_key(void * c, int timeout_ms, int * tap_y) {
    (void) c;
    if (fake.nextkey < fake.nkeys) {
        int i = fake.nextkey++;
        if (tap_y) *tap_y = fake.tap_y[i];
        return fake.keys[i];
    }
    if (timeout_ms < 0) timeout_ms = 1000;
    fake.now += timeout_ms;
    /* No more scripted keys: a test that forgot to leave the menu would
     * wait for ever; the idle limit (menu_idle_ms) ends it as POWEROFF. */
    fake.key_starved = true;
    return LX_KEY_NONE;
}

static lx_fb_t * f_display_open(void * c) { (void) c; return fake.fb; }
static void f_display_close(void * c, lx_fb_t * fb) { (void) c; (void) fb; }

static const lx_platform_t pf = {
    .now_ms = f_now, .sleep_ms = f_sleep, .spawn = f_spawn, .wait = f_wait, .enter_recovery = f_recovery,
    .enable_adb = f_adb, .battery_percent = f_battery, .next_key = f_key, .display_open = f_display_open,
    .display_close = f_display_close,
};

static void script(int n, const outcome_t * o) {
    memcpy(fake.script, o, sizeof(outcome_t) * (size_t) n);
    fake.nscript = n;
    fake.next = 0;
}

static void keys(const char * spec) {
    /* u d s b = up down select back, t<y>, separated by spaces */
    fake.nkeys = fake.nextkey = 0;
    for (const char * p = spec; *p; p++) {
        if (*p == ' ') continue;
        int i = fake.nkeys++;
        fake.tap_y[i] = -1;
        switch (*p) {
        case 'u': fake.keys[i] = LX_KEY_UP; break;
        case 'd': fake.keys[i] = LX_KEY_DOWN; break;
        case 's': fake.keys[i] = LX_KEY_SELECT; break;
        case 'b': fake.keys[i] = LX_KEY_BACK; break;
        case 't':
            fake.keys[i] = LX_KEY_TAP;
            fake.tap_y[i] = atoi(p + 1);
            while (p[1] >= '0' && p[1] <= '9') p++;
            break;
        default: fprintf(stderr, "bad key spec\n"); exit(2);
        }
    }
}

static const char * const r3pro_names[] = { "r3pro.upt", "update.upt", NULL };

static lx_t new_lx(void) {
    memset(&fake.script, 0, sizeof(fake.script));
    fake.nscript = fake.next = fake.nspawned = fake.nkeys = fake.nextkey = 0;
    fake.recoveries = fake.adbs = 0;
    fake.key_starved = false;
    fake.battery = 80;
    lx_t lx = { .cfg = &cfg, .pf = &pf, .menu_idle_ms = 10 * 60 * 1000 };
    return lx;
}

/* The level a start ran at: "previous" is the card backup, rolled back and
 * started from flash. */
static const char * level(int i) {
    const char * p = fake.spawned[i];
    if (strcmp(p, p_factory) == 0) return "factory";
    if (strstr(p, "/current/")) return fake.slots[i];
    return p;
}

/* ---- tar and ISO writers ------------------------------------------------------------ */

typedef struct {
    const char * name;
    const void * data;
    size_t size;
    char type;
} tar_item_t;

static void tar_header(FILE * f, const char * name, size_t size, char type, unsigned mode) {
    unsigned char h[512];
    memset(h, 0, sizeof(h));
    snprintf((char *) h, 100, "%s", name);
    snprintf((char *) h + 100, 8, "%07o", mode);
    snprintf((char *) h + 108, 8, "%07o", 0);
    snprintf((char *) h + 116, 8, "%07o", 0);
    snprintf((char *) h + 124, 12, "%011zo", size);
    snprintf((char *) h + 136, 12, "%011o", 0);
    h[156] = (unsigned char) type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *) h + 148, 8, "%06o", sum);
    h[155] = ' ';
    fwrite(h, 1, 512, f);
}

static void write_tar(const char * path, const tar_item_t * items, int n) {
    lx_mkdir_p(pathf("%s/.mellowtone/update", p_sd));
    FILE * f = fopen(path, "wb");
    for (int i = 0; i < n; i++) {
        tar_header(f, items[i].name, items[i].size, items[i].type, items[i].type == '5' ? 0755 : 0755);
        if (items[i].size) {
            fwrite(items[i].data, 1, items[i].size, f);
            static const unsigned char zero[512];
            size_t pad = (512 - items[i].size % 512) % 512;
            fwrite(zero, 1, pad, f);
        }
    }
    static const unsigned char end[1024];
    fwrite(end, 1, sizeof(end), f);
    fclose(f);
}

static void sha_hex(const void * data, size_t n, char out[65]) {
    unsigned char d[32];
    mbedtls_sha256(data, n, d, 0);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void md5_hex(const void * data, size_t n, char out[33]) {
    unsigned char d[16];
    mbedtls_md5(data, n, d);
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static const char player_v2[] = "#!/bin/sh\necho player v2\n";
static const char font_v2[] = "font bytes v2";

/* A package for `board` version `ver`; corrupt: wrong checksum for the font;
 * extra: a file not in the manifest; traversal: a ../ path. */
static char * make_package(const char * board, const char * ver, const char * file_board, bool corrupt, bool extra,
                           bool traversal) {
    static char manifest[1024];
    char sp[65], sf[65];
    sha_hex(player_v2, strlen(player_v2), sp);
    sha_hex(corrupt ? "other" : font_v2, corrupt ? 5 : strlen(font_v2), sf);
    snprintf(manifest, sizeof(manifest),
             "mellowtone-package 1\nboard %s\nversion %s\nfile %s mellowtone\nfile %s fonts/a.ttf\n", board, ver, sp, sf);
    tar_item_t items[6];
    int n = 0;
    items[n++] = (tar_item_t) { "MANIFEST", manifest, strlen(manifest), '0' };
    items[n++] = (tar_item_t) { "fonts", NULL, 0, '5' };
    items[n++] = (tar_item_t) { "mellowtone", player_v2, strlen(player_v2), '0' };
    items[n++] = (tar_item_t) { "fonts/a.ttf", font_v2, strlen(font_v2), '0' };
    if (extra) items[n++] = (tar_item_t) { "sneaky", "x", 1, '0' };
    if (traversal) items[n++] = (tar_item_t) { "../escape", "x", 1, '0' };
    char * path = pathf("%s/.mellowtone/update/mellowtone-%s-%s.tar", p_sd, file_board, ver);
    write_tar(path, items, n);
    return path;
}

/* Minimal ISO 9660 writer: a root directory and one optional subdirectory,
 * plain ISO names plus an optional Rock Ridge NM name per entry. */
typedef struct {
    const char * iso_name; /* "UPDATE.TXT;1" or "OTA_V0" */
    const char * rr_name;  /* or NULL */
    const void * data;
    size_t size;
    bool in_sub;
} iso_item_t;

static void le_be32(unsigned char * p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char) (v >> (8 * i));
        p[7 - i] = (unsigned char) (v >> (8 * i));
    }
}

static size_t dir_record(unsigned char * r, const char * name, size_t nlen, const char * rr, uint32_t lba, uint32_t size,
                         bool dir) {
    size_t len = 33 + nlen;
    if (len & 1) len++;
    size_t su = len;
    if (rr) len += 5 + strlen(rr);
    if (len & 1) len++;
    memset(r, 0, len);
    r[0] = (unsigned char) len;
    le_be32(r + 2, lba);
    le_be32(r + 10, size);
    r[25] = dir ? 2 : 0;
    r[28] = 1;
    r[32] = (unsigned char) nlen;
    memcpy(r + 33, name, nlen);
    if (rr) {
        r[su] = 'N'; r[su + 1] = 'M'; r[su + 2] = (unsigned char) (5 + strlen(rr)); r[su + 3] = 1; r[su + 4] = 0;
        memcpy(r + su + 5, rr, strlen(rr));
    }
    return len;
}

static void write_iso(const char * path, const iso_item_t * items, int n, const char * sub_name, const char * sub_rr) {
    enum { S = 2048 };
    uint32_t root_lba = 18, sub_lba = 19, next = 20;
    uint32_t lbas[16];
    for (int i = 0; i < n; i++) {
        lbas[i] = next;
        next += (uint32_t) ((items[i].size + S - 1) / S);
        if (items[i].size == 0) next++;
    }
    size_t total = (size_t) next * S;
    unsigned char * img = calloc(1, total);
    unsigned char * pvd = img + 16 * S;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "\0", 1, NULL, root_lba, S, true);
    unsigned char * term = img + 17 * S;
    term[0] = 255; memcpy(term + 1, "CD001", 5); term[6] = 1;
    for (int d = 0; d < 2; d++) {
        if (d == 1 && !sub_name) break;
        unsigned char * dir = img + (d ? sub_lba : root_lba) * S;
        size_t off = 0;
        off += dir_record(dir + off, "\0", 1, NULL, d ? sub_lba : root_lba, S, true);
        off += dir_record(dir + off, "\1", 1, NULL, root_lba, S, true);
        if (d == 0 && sub_name) off += dir_record(dir + off, sub_name, strlen(sub_name), sub_rr, sub_lba, S, true);
        for (int i = 0; i < n; i++) {
            if (items[i].in_sub != (d == 1)) continue;
            off += dir_record(dir + off, items[i].iso_name, strlen(items[i].iso_name), items[i].rr_name, lbas[i],
                              (uint32_t) items[i].size, false);
        }
    }
    for (int i = 0; i < n; i++) memcpy(img + (size_t) lbas[i] * S, items[i].data, items[i].size);
    write_bytes(path, img, total, 0644);
    free(img);
}

static char kernel_data[5000], rootfs_data[9000];

static void make_r3pro_image(const char * name, const char * product, bool bad_md5, bool bootloader) {
    char mk[33], mr[33], text[1024], ver[256];
    md5_hex(kernel_data, sizeof(kernel_data), mk);
    md5_hex(rootfs_data, sizeof(rootfs_data), mr);
    if (bad_md5) mr[0] = mr[0] == '0' ? '1' : '0';
    snprintf(text, sizeof(text),
             "\nkernel={\n\tname=kernel\n\tfile_path=autoupdate/uimage.bin\n\tmd5=%s\n}\n\n"
             "rootfs={\n\tname=rootfs\n\tfull_upgrade=yes\n\tfile_path=autoupdate/system.ubi\n\tmd5=%s\n}\n%s",
             mk, mr, bootloader ? "bootloader={\n\tname=bootloader\n\tfile_path=autoupdate/uimage.bin\n\tmd5=0\n}\n" : "");
    snprintf(ver, sizeof(ver), "\nversion={\n\tname=%s\n\tver=2023-02-07T14:18:03+08:00\n}\n", product);
    iso_item_t items[] = {
        { "UPDATE.TXT;1", NULL, text, strlen(text), false },
        { "VERSION.TXT;1", NULL, ver, strlen(ver), false },
        { "UIMAGE.BIN;1", NULL, kernel_data, sizeof(kernel_data), false },
        { "SYSTEM.UBI;1", NULL, rootfs_data, sizeof(rootfs_data), false },
    };
    write_iso(pathf("%s/%s", p_sd, name), items, 4, NULL, NULL);
}

static void make_ota_image(const char * name, bool bad, const char * board) {
    /* rootfs.squashfs split in two chunks named <name>.NNNN.<md5 chain>. */
    char m_all[33], m0[33], text[512];
    md5_hex(rootfs_data, sizeof(rootfs_data), m_all);
    md5_hex(rootfs_data, 4096, m0);
    static char c1[sizeof(rootfs_data) - 4096];
    memcpy(c1, rootfs_data + 4096, sizeof(c1));
    if (bad) c1[10] ^= 0x55;
    snprintf(text, sizeof(text), "ota_version=0\nimg_type=rootfs\nimg_name=rootfs.squashfs\nimg_size=%zu\nimg_md5=%s\n",
             sizeof(rootfs_data), m_all);
    char n0[96], n1[96];
    snprintf(n0, sizeof(n0), "rootfs.squashfs.0000.%s", m_all);
    snprintf(n1, sizeof(n1), "rootfs.squashfs.0001.%s", m0);
    /* board: the repack script's marker at the root (NULL: an older image without one). */
    char marker[64];
    snprintf(marker, sizeof(marker), "board=%s\n", board ? board : "");
    iso_item_t items[] = {
        { "OTA_UPDA.IN;1", "ota_update.in", text, strlen(text), true },
        { "ROOTFS_S.000;1", n0, rootfs_data, 4096, true },
        { "ROOTFS_S.001;1", n1, c1, sizeof(c1), true },
        { "MELLOWTO.IN;1", LX_FW_BOARD_MARKER, marker, strlen(marker), false },
    };
    write_iso(pathf("%s/%s", p_sd, name), items, board ? 4 : 3, "OTA_V0", "ota_v0");
}

/* ---- tests ------------------------------------------------------------------------- */

static void test_fallback_chain(void) {
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.0");
    write_bytes(p_factory, "#!/bin/sh\n", 10, 0755);
    lx_t lx = new_lx();
    outcome_t o[9];
    for (int i = 0; i < 9; i++) o[i] = (outcome_t) { 2000, i % 2 ? SIG(11) : EXIT(1), false, false };
    script(9, o);
    keys("u s"); /* the menu: up wraps to Power off */
    lx_end_t end = lx_run(&lx, false);
    CHECK(end == LX_END_POWEROFF, "chain ends in the menu, then power off (got %d)", end);
    CHECK(fake.nspawned == 9, "9 starts, got %d", fake.nspawned);
    for (int i = 0; i < 9 && i < fake.nspawned; i++) {
        const char * want = i < 3 ? "current" : i < 6 ? "previous" : "factory";
        CHECK(strcmp(level(i), want) == 0, "start %d is %s, got %s", i, want, level(i));
    }
    CHECK(strcmp(fake.notices[3], "rolled_back") == 0, "previous starts with the rolled-back notice (%s)", fake.notices[3]);
    CHECK(fake.notices[4][0] == '\0', "notice only once");
    CHECK(strcmp(fake.notices[6], "factory") == 0, "factory notice (%s)", fake.notices[6]);
    CHECK(strstr(lx.reason, "did not start") != NULL, "menu shows the reason: %s", lx.reason);
    char v1[64], v2[64];
    CHECK(lx_slot_version(&cfg, "current", v1, sizeof(v1)) && strcmp(v1, "1.0") == 0 &&
              lx_slot_version(&cfg, "previous", v2, sizeof(v2)) && strcmp(v2, "2.0") == 0,
          "automatic roll back: 1.0 current, 2.0 on the card (%s, %s)", v1, v2);
    CHECK(lx_fb_flushes(fake.fb) > 0, "menu drawn");
    CHECK(lx_fb_pixel(fake.fb, 2, 2) == 0x1E4D6B, "title bar colour %06x", lx_fb_pixel(fake.fb, 2, 2));

    /* A slow crash (after 60 s) does not count towards the three. */
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.0");
    lx = new_lx();
    outcome_t o2[] = { { 2000, EXIT(1), 0, 0 }, { 2000, EXIT(1), 0, 0 }, { 70000, EXIT(1), 0, 0 },
                       { 2000, EXIT(1), 0, 0 }, { 100000, EXIT(0), 0, 0 } };
    script(5, o2);
    end = lx_run(&lx, false);
    CHECK(end == LX_END_HALT && fake.nspawned == 5, "slow crash resets the count (%d starts)", fake.nspawned);
    for (int i = 0; i < fake.nspawned; i++) CHECK(strcmp(level(i), "current") == 0, "start %d stays on current", i);

    /* No factory copy: current -> previous -> stock player. */
    reset_tree();
    make_slot("current", "2.0");
    write_bytes(p_factory, "#!/bin/sh\n", 10, 0755);
    lx = new_lx();
    for (int i = 0; i < 3; i++) o[i] = (outcome_t) { 1000, EXIT(2), false, false };
    o[3] = (outcome_t) { 200000, EXIT(0), false, false };
    script(4, o);
    end = lx_run(&lx, false);
    CHECK(end == LX_END_HALT && fake.nspawned == 4 && strcmp(level(3), "factory") == 0,
          "missing previous is skipped (%d starts, last %s)", fake.nspawned, fake.nspawned ? level(fake.nspawned - 1) : "-");
    CHECK(strcmp(fake.notices[3], "factory") == 0, "factory notice past a missing previous (%s)", fake.notices[3]);
    unlink(p_factory);

    /* Player exit 75: reboot; exit 0: the system is shutting down. */
    reset_tree();
    make_slot("current", "2.0");
    lx = new_lx();
    o[0] = (outcome_t) { 5000, EXIT(BOARD_LAUNCHER_EXIT_REBOOT), false, false };
    script(1, o);
    CHECK(lx_run(&lx, false) == LX_END_REBOOT, "exit 75 reboots");

    /* Developer redeploy marker: restart without counting. */
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.0");
    lx = new_lx();
    for (int i = 0; i < 4; i++) o[i] = (outcome_t) { 1000, SIG(15), false, true };
    o[4] = (outcome_t) { 1000, EXIT(0), false, false };
    script(5, o);
    end = lx_run(&lx, false);
    CHECK(end == LX_END_HALT && fake.nspawned == 5 && strcmp(level(4), "current") == 0, "redeploys are not crashes");
}

static void test_boot_counter(void) {
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.0");
    lx_t lx = new_lx();
    outcome_t o[] = { { 200000, EXIT(0), false, false } };
    script(1, o);
    CHECK(lx_run(&lx, false) == LX_END_HALT, "normal boot");
    CHECK(fake.counter_at_spawn[0] == 1, "counter 1 while starting (%d)", fake.counter_at_spawn[0]);
    CHECK(lx_boot_counter_read(&cfg) == 0, "counter cleared after 90 s");
    CHECK(!lx_exists(pathf("%s/%s", p_boot, BOARD_LAUNCHER_BOOT_COUNTER)), "no counter file after a good boot");

    /* A power off or restart asked for before 90 s is not a failed boot. */
    for (int code = 0; code < 2; code++) {
        lx_boot_counter_write(&cfg, 2);
        lx = new_lx();
        outcome_t r[] = { { 30000, EXIT(code ? BOARD_LAUNCHER_EXIT_REBOOT : 0), false, false } };
        script(1, r);
        lx_run(&lx, false);
        CHECK(fake.counter_at_spawn[0] == 3 && lx_boot_counter_read(&cfg) == 0, "clean exit %d clears the counter (%d)",
              code ? BOARD_LAUNCHER_EXIT_REBOOT : 0, lx_boot_counter_read(&cfg));
    }

    /* Only a change is written (the flash). */
    lx_boot_counter_write(&cfg, 1);
    struct stat st1, st2;
    const char * counter_file = pathf("%s/%s", p_boot, BOARD_LAUNCHER_BOOT_COUNTER);
    CHECK(stat(counter_file, &st1) == 0, "counter file");
    lx_boot_counter_write(&cfg, 1);
    CHECK(stat(counter_file, &st2) == 0 && st1.st_ino == st2.st_ino, "same value not rewritten");
    lx_boot_counter_write(&cfg, 0);

    /* Three boots that never get through 90 s. No backup on the card yet, so
     * each ends at the factory player, which restarts the device. */
    write_bytes(p_factory, "#!/bin/sh\n", 10, 0755);
    lx_rm_rf(backup_dir());
    for (int boot = 1; boot <= 3; boot++) {
        lx = new_lx();
        outcome_t r[] = { { 1000, SIG(11), false, false }, { 1000, SIG(11), false, false }, { 1000, SIG(11), false, false },
                          { 30000, EXIT(BOARD_LAUNCHER_EXIT_REBOOT), false, false } };
        script(4, r);
        lx_run(&lx, false);
        CHECK(lx_boot_counter_read(&cfg) == boot, "boot %d counted (%d)", boot, lx_boot_counter_read(&cfg));
        CHECK(strcmp(level(0), "current") == 0, "boot %d still tries current", boot);
    }
    /* The 4th boot, now with a backup on the card: rolled back before
     * anything starts; the restored version starts afresh. */
    make_slot("previous", "1.0");
    lx = new_lx();
    script(1, o);
    lx_run(&lx, false);
    CHECK(strcmp(level(0), "previous") == 0, "4th boot starts previous (%s)", level(0));
    CHECK(strcmp(fake.notices[0], "boot_loop") == 0, "boot-loop notice (%s)", fake.notices[0]);
    char ver[64];
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "1.0 restored (%s)", ver);
    CHECK(lx_slot_version(&cfg, "previous", ver, sizeof(ver)) && strcmp(ver, "2.0") == 0, "2.0 on the card (%s)", ver);
    CHECK(lx_boot_counter_read(&cfg) == 0, "counter cleared by the roll back");
    lx_boot_counter_write(&cfg, 3);

    /* A menu-only session (Vol- held, then Power off) is not a boot attempt. */
    lx = new_lx();
    keys("u s");
    CHECK(lx_run(&lx, true) == LX_END_POWEROFF && lx_boot_counter_read(&cfg) == 3, "menu-only session not counted");
    lx_boot_counter_write(&cfg, 1);
    lx = new_lx();
    keys("u s");
    lx_run(&lx, true);
    CHECK(lx_boot_counter_read(&cfg) == 1, "menu-only session leaves the counter (%d)", lx_boot_counter_read(&cfg));
    lx_boot_counter_write(&cfg, 3);

    /* Start Mellowtone by hand from the menu (Vol- held): a fresh try. */
    lx = new_lx();
    script(1, o);
    keys("s");
    lx_run(&lx, true);
    CHECK(fake.nspawned == 1 && strcmp(level(0), "current") == 0, "menu start tries current again (%s)", level(0));
    CHECK(fake.counter_at_spawn[0] == 1, "fresh try counted as 1 (%d)", fake.counter_at_spawn[0]);
    CHECK(lx_boot_counter_read(&cfg) == 0, "and cleared after 90 s");

    /* Boot loop with an update on the card: the countdown keeps the
     * rollback, Start chosen by hand tries current. */
    lx_boot_counter_write(&cfg, 3);
    make_package("r3pro", "3.0", "r3pro", false, false, false);
    lx = new_lx();
    script(1, o);
    keys("");
    lx_run(&lx, false);
    CHECK(fake.nspawned == 1 && strcmp(level(0), "previous") == 0, "countdown keeps the rollback (%s)", level(0));
    lx_boot_counter_write(&cfg, 3);
    lx = new_lx();
    script(1, o);
    keys("s");
    lx_run(&lx, false);
    CHECK(fake.nspawned == 1 && strcmp(level(0), "current") == 0 && !fake.notices[0][0],
          "Start by hand with an update on the card tries current (%s, %s)", level(0), fake.notices[0]);

    /* Installing a fix from the menu: the counter starts again, so the new
     * current is what the next boot starts. */
    lx_boot_counter_write(&cfg, 3);
    lx = new_lx();
    keys("d s d s s u u u s"); /* Install..., Install (confirm), OK, Reboot */
    CHECK(lx_run(&lx, false) == LX_END_REBOOT, "install, then reboot");
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "3.0") == 0, "3.0 installed (%s)", ver);
    CHECK(lx_boot_counter_read(&cfg) == 0, "install resets the counter (%d)", lx_boot_counter_read(&cfg));
    lx_boot_counter_write(&cfg, 3);
    make_package("r3pro", "4.0", "r3pro", false, false, false);
    lx = new_lx();
    script(1, o);
    keys("d s d s s u s"); /* Install..., Install (confirm), OK, Start */
    lx_run(&lx, false);
    CHECK(fake.nspawned == 1 && strcmp(level(0), "current") == 0 && !fake.notices[0][0] && fake.counter_at_spawn[0] == 1,
          "installed version starts fresh (%s, %s, %d)", level(0), fake.notices[0], fake.counter_at_spawn[0]);

    /* Rolling back from the menu resets the counter too. */
    lx_boot_counter_write(&cfg, 3);
    lx = new_lx();
    keys("d s d s s u u u s"); /* Roll back, confirm, OK, Reboot */
    CHECK(lx_run(&lx, true) == LX_END_REBOOT, "roll back, then reboot");
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "3.0") == 0, "rolled back to 3.0 (%s)", ver);
    CHECK(lx_boot_counter_read(&cfg) == 0, "roll back resets the counter (%d)", lx_boot_counter_read(&cfg));

    /* Boot loop without a previous version: the factory player, with its notice. */
    lx_rm_rf(backup_dir());
    lx_boot_counter_write(&cfg, 3);
    lx = new_lx();
    script(1, o);
    lx_run(&lx, false);
    CHECK(fake.nspawned == 1 && strcmp(level(0), "factory") == 0 && strcmp(fake.notices[0], "factory") == 0,
          "boot loop, no previous: factory with its notice (%s, %s)", level(0), fake.notices[0]);
    unlink(p_factory);
    lx_boot_counter_write(&cfg, 0);
}

static void test_menu_flag(void) {
    reset_tree();
    make_slot("current", "2.0");
    lx_t lx = new_lx();
    lx_mkdir_p(p_boot);
    write_text(pathf("%s/%s", p_boot, BOARD_LAUNCHER_MENU_FLAG), "");
    outcome_t o[] = { { 200000, EXIT(0), false, false } };
    script(1, o);
    keys("s");
    CHECK(lx_run(&lx, false) == LX_END_HALT, "flag: menu, then start");
    CHECK(fake.nextkey == 1, "the menu read the key");
    CHECK(!lx_menu_flag_present(&cfg), "flag consumed");
    CHECK(fake.nspawned == 1, "started once");

    /* "Restart into recovery menu" in the player: flag + exit 75 -> menu
     * without a reboot; the flag works once. */
    lx = new_lx();
    outcome_t o2[] = { { 120000, EXIT(BOARD_LAUNCHER_EXIT_REBOOT), true, false }, { 120000, EXIT(BOARD_LAUNCHER_EXIT_REBOOT), false, false } };
    script(2, o2);
    keys("s");
    lx_end_t end = lx_run(&lx, false);
    CHECK(fake.nspawned == 2 && fake.nextkey == 1, "menu opened between the runs");
    CHECK(end == LX_END_REBOOT, "second exit 75 without the flag reboots");
}

static void test_menu_keys(void) {
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.0");
    write_bytes(p_factory, "#!/bin/sh\n", 10, 0755);
    outcome_t o[] = { { 200000, EXIT(0), false, false } };
    /* Items: Start, Roll back to 1.0, Start factory, ADB, Reset, Info, Reboot, Power off. */
    struct { const char * keys; const char * want; } cases[] = {
        { "d d s", "factory" },
        { "b d b d d s", "factory" },        /* Back in the main menu only returns to the top */
        { "u u s", "reboot" },
        { "u s", "poweroff" },
        { "t70", "current" },                /* first row on a 480x800 screen */
        { "d s d s s u s", "current" },      /* Roll back, confirm, OK, Start */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lx_t lx = new_lx();
        script(1, o);
        keys(cases[i].keys);
        lx_end_t end = lx_run(&lx, true);
        const char * got = end == LX_END_REBOOT ? "reboot" : end == LX_END_POWEROFF ? "poweroff"
                           : fake.nspawned ? level(0) : "nothing";
        CHECK(strcmp(got, cases[i].want) == 0, "keys '%s': %s, got %s", cases[i].keys, cases[i].want, got);
    }
    char ver[64];
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "menu roll back (%s)", ver);
    /* Device info, then ADB, then power off. */
    lx_t lx = new_lx();
    keys("u u u s s u u s s d d d d s"); /* Device info, OK, Enable ADB, OK, Power off */
    lx_end_t end = lx_run(&lx, true);
    CHECK(end == LX_END_POWEROFF, "info/adb/poweroff (%d)", end);
    CHECK(fake.adbs == 1, "ADB enabled once (%d)", fake.adbs);
    CHECK(lx_exists(p_adb), "ADB flag written");
    /* Idle in the menu: power off. */
    lx = new_lx();
    lx.menu_idle_ms = 60000;
    keys("");
    CHECK(lx_run(&lx, true) == LX_END_POWEROFF && fake.key_starved, "idle menu powers off");
    unlink(p_factory);
}

static bool fail_at_step(const char * step);
static const char * fail_step;
static int fail_count, fail_after;
static bool fail_at_step(const char * step) {
    if (fail_step && strcmp(step, fail_step) == 0 && ++fail_count > fail_after) return false;
    return true;
}

static void check_clean_slots(const char * what) {
    CHECK(!lx_exists(pathf("%s/next", p_slots)), "%s: next left", what);
    CHECK(!lx_exists(pathf("%s/next.partial", p_slots)), "%s: next.partial left", what);
    CHECK(!lx_exists(pathf("%s/current.old", p_slots)), "%s: current.old left", what);
    CHECK(!lx_exists(pathf("%s/previous", p_slots)), "%s: a previous version in flash", what);
    CHECK(!lx_exists(pathf("%s/swap.journal", p_slots)), "%s: journal left", what);
    CHECK(!lx_exists(pathf("%s.partial", backup_dir())), "%s: card player.partial left", what);
    CHECK(!lx_exists(pathf("%s.new", backup_dir())), "%s: card player.new left", what);
    CHECK(!lx_exists(pathf("%s.old", backup_dir())), "%s: card player.old left", what);
}

static void test_packages(void) {
    char ver[64], err[200];
    lx_pkg_ref_t ref;

    /* Install: v1 current, v0 previous -> v2 current, v1 previous. */
    reset_tree();
    make_slot("current", "1.0");
    make_slot("previous", "0.9");
    char * pkg = make_package("r3pro", "2.0", "r3pro", false, false, false);
    make_package("r3pro", "1.5", "r3pro", false, false, false);
    make_package("r3proii", "9.0", "r3proii", false, false, false);
    CHECK(lx_pkg_find(&cfg, &ref) && strcmp(ref.version, "2.0") == 0, "newest package for this board (%s)", ref.version);
    CHECK(lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err)), "install: %s", err);
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "2.0") == 0, "current is 2.0 (%s)", ver);
    CHECK(lx_slot_version(&cfg, "previous", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "previous is 1.0 (%s)", ver);
    CHECK(lx_is_executable(pathf("%s/current/mellowtone", p_slots)), "player executable");
    CHECK(file_contains(pathf("%s/current/fonts/a.ttf", p_slots), font_v2), "font unpacked");
    CHECK(!lx_exists(pkg), "package deleted");
    CHECK(file_contains(pathf("%s/update.log", p_logs), "version 2.0 installed"), "update.log");
    check_clean_slots("install");

    /* Refusals leave everything as it was. */
    struct { const char * board, * file_board, * why; bool corrupt, extra, traversal; } bad[] = {
        { "r3proii", "r3pro", "is for r3proii", false, false, false },
        { "r3pro", "r3pro", "checksum mismatch", true, false, false },
        { "r3pro", "r3pro", "not in the manifest", false, true, false },
        { "r3pro", "r3pro", "unsafe path", false, false, true },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_tree();
        make_slot("current", "1.0");
        pkg = make_package(bad[i].board, "3.0", bad[i].file_board, bad[i].corrupt, bad[i].extra, bad[i].traversal);
        CHECK(lx_pkg_find(&cfg, &ref), "found %zu", i);
        CHECK(!lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err)), "refused %zu", i);
        CHECK(strstr(err, bad[i].why) != NULL, "reason '%s' contains '%s'", err, bad[i].why);
        CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "current kept (%s)", ver);
        CHECK(lx_exists(pkg), "refused package stays on the card");
        CHECK(!lx_exists(backup_dir()), "refused: nothing saved to the card");
        check_clean_slots("refused");
    }
    /* Truncated archive. */
    reset_tree();
    make_slot("current", "1.0");
    pkg = make_package("r3pro", "3.0", "r3pro", false, false, false);
    CHECK(truncate(pkg, 1024 + 512 + 10) == 0, "truncate");
    lx_pkg_find(&cfg, &ref);
    CHECK(!lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err)) && strstr(err, "truncated"), "truncated: %s", err);
    /* Not a package at all. */
    write_text(pkg, "hello");
    CHECK(!lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err)), "garbage refused: %s", err);

    /* Power loss at every step of an install, with and without a previous
     * version on the card. */
    const char * steps[] = { "backup-saved", "journal", "backup-rotated", "current-removed", "extract",
                             "next-ready", "next-to-current", "journal-removed" };
    for (int with_prev = 0; with_prev < 2; with_prev++) {
        for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
            bool is_extract = strcmp(steps[s], "extract") == 0;
            for (int after = 0; after < (is_extract ? 2 : 1); after++) {
                reset_tree();
                make_slot("current", "1.0");
                if (with_prev) make_slot("previous", "0.9");
                make_package("r3pro", "2.0", "r3pro", false, false, false);
                lx_pkg_find(&cfg, &ref);
                fail_step = steps[s];
                fail_count = 0;
                fail_after = after;
                lx_step_hook = fail_at_step;
                bool ok = lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err));
                lx_step_hook = NULL;
                bool hit = fail_count > after;
                CHECK(!ok || !hit, "power loss at %s reported success", steps[s]);
                /* Stopped between the journal and its removal: the next start finishes it. */
                bool journaled = hit && s >= 1 && s <= 6;
                CHECK(lx_slots_swap_pending(&cfg) == journaled, "after %s: journal %d", steps[s], journaled);
                /* The next start. */
                CHECK(lx_slots_recover(&cfg), "recover after %s", steps[s]);
                CHECK(lx_is_executable(pathf("%s/current/mellowtone", p_slots)), "bootable current after %s (prev %d)",
                      steps[s], with_prev);
                lx_slot_version(&cfg, "current", ver, sizeof(ver));
                const char * want = !hit || s >= 5 ? "2.0" : "1.0";
                CHECK(strcmp(ver, want) == 0, "after power loss at %s(+%d): current %s, want %s", steps[s], after, ver, want);
                if (strcmp(ver, "2.0") == 0)
                    CHECK(file_contains(pathf("%s/current/fonts/a.ttf", p_slots), font_v2), "complete tree after %s", steps[s]);
                /* The card: the old current once the journal exists, else as before. */
                bool card_has = lx_slot_version(&cfg, "previous", ver, sizeof(ver));
                if (!hit || s >= 1)
                    CHECK(card_has && strcmp(ver, "1.0") == 0, "after %s: 1.0 on the card (%s)", steps[s], ver);
                else if (with_prev)
                    CHECK(card_has && strcmp(ver, "0.9") == 0, "after %s: card backup untouched (%s)", steps[s], ver);
                else
                    CHECK(!card_has, "after %s: no backup yet", steps[s]);
                check_clean_slots(steps[s]);
            }
        }
    }
    /* The first install ever: no current yet. */
    reset_tree();
    pkg = make_package("r3pro", "2.0", "r3pro", false, false, false);
    lx_pkg_find(&cfg, &ref);
    CHECK(lx_pkg_install(&cfg, &ref, NULL, NULL, err, sizeof(err)), "first install: %s", err);
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "2.0") == 0, "first install current");
    CHECK(!lx_exists(pathf("%s/previous", p_slots)) && !lx_exists(backup_dir()), "no previous after the first install");

    /* An install that fails after the current version was removed (a write
     * error while unpacking) gets it back from the card. */
    reset_tree();
    make_slot("current", "1.0");
    CHECK(lx_slots_install_begin(&cfg, "2.0", 100, err, sizeof(err)), "begin: %s", err);
    CHECK(!lx_is_dir(pathf("%s/current", p_slots)) && lx_slots_swap_pending(&cfg), "current removed, journal written");
    CHECK(lx_slots_install_abort(&cfg, err, sizeof(err)), "abort: %s", err);
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "1.0 restored (%s)", ver);
    CHECK(!lx_slots_swap_pending(&cfg), "abort removes the journal");
    check_clean_slots("abort");

    /* No writable card: refused before anything changes. */
    lx_config_t nocard = cfg;
    nocard.sd_root = pathf("%s/no-card", root);
    CHECK(!lx_slots_install_begin(&nocard, "2.0", 100, err, sizeof(err)) && strstr(err, "no writable card"),
          "no card: %s", err);
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "no card: current kept");

    /* A previous version in flash from the two-slot launcher moves to the card. */
    reset_tree();
    make_slot("current", "2.0");
    make_tree(pathf("%s/previous", p_slots), "1.0");
    CHECK(lx_slots_recover(&cfg), "migration");
    CHECK(!lx_exists(pathf("%s/previous", p_slots)), "flash previous removed");
    CHECK(lx_slot_version(&cfg, "previous", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "on the card (%s)", ver);
    reset_tree();
    make_slot("current", "2.0");
    make_slot("previous", "1.5");
    make_tree(pathf("%s/previous", p_slots), "1.0");
    CHECK(lx_slots_recover(&cfg) && !lx_exists(pathf("%s/previous", p_slots)), "migration with a card backup");
    CHECK(lx_slot_version(&cfg, "previous", ver, sizeof(ver)) && strcmp(ver, "1.5") == 0, "card backup kept (%s)", ver);

    /* Rollback, and power loss at each of its steps. */
    const char * rsteps[] = { "backup-saved", "journal", "current-removed", "committed", "next-ready",
                              "next-to-current", "backup-rotated", "journal-removed" };
    for (size_t s = 0; s <= sizeof(rsteps) / sizeof(rsteps[0]); s++) {
        reset_tree();
        make_slot("current", "2.0");
        make_slot("previous", "1.0");
        fail_step = s < sizeof(rsteps) / sizeof(rsteps[0]) ? rsteps[s] : NULL;
        fail_count = fail_after = 0;
        lx_step_hook = fail_at_step;
        bool ok = lx_slots_rollback(&cfg);
        lx_step_hook = NULL;
        CHECK(lx_slots_recover(&cfg), "recover rollback");
        char cur[64], prev[64];
        lx_slot_version(&cfg, "current", cur, sizeof(cur));
        lx_slot_version(&cfg, "previous", prev, sizeof(prev));
        bool swapped = strcmp(cur, "1.0") == 0 && strcmp(prev, "2.0") == 0;
        bool same = strcmp(cur, "2.0") == 0 && strcmp(prev, "1.0") == 0;
        CHECK(swapped || same, "rollback step %zu: current %s previous %s", s, cur, prev);
        CHECK(!fail_step || s < 2 || swapped, "rollback past removing current finishes (%s)", rsteps[s < 8 ? s : 0]);
        CHECK(!fail_step || s >= 2 || same, "rollback stopped before removing current changes nothing (%zu)", s);
        CHECK(fail_step || (ok && swapped), "rollback without power loss");
        check_clean_slots("rollback");
    }
    /* No backup on the card: nothing to roll back to. */
    reset_tree();
    make_slot("current", "2.0");
    CHECK(!lx_slots_rollback(&cfg) && !lx_slots_swap_pending(&cfg), "no backup: refused, nothing changed");

    /* Through the menu: the package is offered, installed, then started. */
    reset_tree();
    make_slot("current", "1.0");
    make_package("r3pro", "2.0", "r3pro", false, false, false);
    lx_t lx = new_lx();
    outcome_t o[] = { { 200000, EXIT(0), false, false } };
    script(1, o);
    keys("d s d s s u s"); /* Install..., Install (confirm), OK, then back to Start */
    lx_end_t end = lx_run(&lx, false);
    CHECK(end == LX_END_HALT && fake.nspawned == 1, "menu install then start (%d, %d)", end, fake.nspawned);
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "2.0") == 0, "menu installed 2.0 (%s)", ver);

    /* Opened only because of the package: starts by itself after 30 s. */
    reset_tree();
    make_slot("current", "1.0");
    make_package("r3pro", "2.0", "r3pro", false, false, false);
    lx = new_lx();
    script(1, o);
    keys("");
    int64_t t0 = fake.now;
    end = lx_run(&lx, false);
    CHECK(end == LX_END_HALT && fake.nspawned == 1 && fake.now - t0 >= 30000, "auto start after the countdown");
    CHECK(lx_slot_version(&cfg, "current", ver, sizeof(ver)) && strcmp(ver, "1.0") == 0, "nothing installed without asking");
}

static void test_firmware(void) {
    for (size_t i = 0; i < sizeof(kernel_data); i++) kernel_data[i] = (char) (i * 7);
    for (size_t i = 0; i < sizeof(rootfs_data); i++) rootfs_data[i] = (char) (i * 13 + 1);
    char path[PATH_MAX];
    lx_fw_info_t info;

    reset_tree();
    make_r3pro_image("update.upt", "r3pro", false, false);
    CHECK(lx_fw_find(&cfg, path, sizeof(path)) && strstr(path, "update.upt"), "update.upt found");
    make_r3pro_image("r3pro.upt", "r3pro", false, false);
    CHECK(lx_fw_find(&cfg, path, sizeof(path)) && strstr(path, "r3pro.upt"), "r3pro.upt first (%s)", path);
    write_text(pathf("%s/other.upt", p_sd), "x");
    unlink(pathf("%s/r3pro.upt", p_sd));
    unlink(pathf("%s/update.upt", p_sd));
    CHECK(!lx_fw_find(&cfg, path, sizeof(path)), "R3 Pro ignores other names");
    unlink(pathf("%s/other.upt", p_sd));

    make_r3pro_image("r3pro.upt", "r3pro", false, false);
    lx_fw_find(&cfg, path, sizeof(path));
    CHECK(lx_fw_inspect(&cfg, path, &info), "inspect: %s", info.error);
    CHECK(info.nparts == 2 && strcmp(info.parts[0].name, "kernel") == 0 && strcmp(info.parts[1].name, "rootfs") == 0,
          "parts listed");
    CHECK(strcmp(info.product, "r3pro") == 0 && strstr(info.version, "2023-02-07"), "model/version from VERSION.TXT");
    CHECK(info.parts[1].size == (long long) sizeof(rootfs_data), "part size");
    CHECK(lx_fw_verify(&info, cfg.fw_format, NULL, NULL), "md5 ok: %s", info.error);

    make_r3pro_image("r3pro.upt", "r3pro", true, false);
    lx_fw_inspect(&cfg, path, &info);
    CHECK(!lx_fw_verify(&info, cfg.fw_format, NULL, NULL) && strstr(info.error, "rootfs: md5 mismatch"), "md5 mismatch: %s",
          info.error);
    make_r3pro_image("r3pro.upt", "r3proii", false, false);
    CHECK(!lx_fw_inspect(&cfg, path, &info) && strstr(info.error, "is for r3proii"), "wrong model: %s", info.error);
    make_r3pro_image("r3pro.upt", "r3pro", false, true);
    CHECK(!lx_fw_inspect(&cfg, path, &info) && strstr(info.error, "boot loader"), "boot loader refused: %s", info.error);
    write_text(path, "not an iso");
    CHECK(!lx_fw_inspect(&cfg, path, &info), "garbage image refused");

    /* Through the menu. Items with an image: Start, Install firmware, ... */
    make_slot("current", "1.0");
    outcome_t o[] = { { 200000, EXIT(0), false, false } };
    /* Low battery: refused before anything else. */
    make_r3pro_image("r3pro.upt", "r3pro", false, false);
    lx_t lx = new_lx();
    fake.battery = 49;
    script(1, o);
    keys("d s s u u s"); /* Install firmware, OK (refusal), Power off */
    lx_end_t end = lx_run(&lx, true);
    CHECK(end == LX_END_POWEROFF && fake.recoveries == 0, "low battery refused (%d, %d)", end, fake.recoveries);
    /* Unknown battery: refused too. */
    lx = new_lx();
    fake.battery = -1;
    script(1, o);
    keys("d s s u u s");
    lx_run(&lx, true);
    CHECK(fake.recoveries == 0, "unknown battery refused");
    /* md5 mismatch: refused after the check. */
    make_r3pro_image("r3pro.upt", "r3pro", true, false);
    lx = new_lx();
    script(1, o);
    keys("d s d s s u u s"); /* Install firmware, Check the image, OK, Power off */
    end = lx_run(&lx, true);
    CHECK(end == LX_END_POWEROFF && fake.recoveries == 0, "damaged image refused (%d)", fake.recoveries);
    /* Good image: two confirmations, then recovery. Cancel at the second first. */
    make_r3pro_image("r3pro.upt", "r3pro", false, false);
    lx = new_lx();
    script(1, o);
    keys("d s d s d s s u u s"); /* Install firmware, Check, Install, Cancel (default), Power off */
    end = lx_run(&lx, true);
    CHECK(fake.recoveries == 0, "cancelled at the last check");
    lx = new_lx();
    script(1, o);
    keys("d s d s d s d s s u u s"); /* ..., Yes install now, OK (recovery failed), Power off */
    end = lx_run(&lx, true);
    CHECK(fake.recoveries == 1 && end == LX_END_POWEROFF, "confirmed twice: recovery entered (%d)", fake.recoveries);
    CHECK(file_contains(pathf("%s/update.log", p_logs), "entering recovery (battery 80"), "logged");

    /* X1600 OTA image: chunked parts, any .upt name. */
    lx_config_t x = cfg;
    static const char * const x_names[] = { "update.upt", NULL };
    x.board = "r1";
    x.fw_names = x_names;
    x.fw_any_upt = true;
    x.fw_format = BOARD_FW_INGENIC_OTA;
    x.fw_product = NULL;
    reset_tree();
    make_ota_image("whatever.upt", false, "r1");
    CHECK(lx_fw_find(&x, path, sizeof(path)) && strstr(path, "whatever.upt"), "any .upt on X1600");
    CHECK(lx_fw_inspect(&x, path, &info) && info.nparts == 1 && strcmp(info.parts[0].file, "rootfs.squashfs") == 0,
          "OTA parts: %s", info.error);
    CHECK(strcmp(info.product, "r1") == 0, "OTA board from the marker (%s)", info.product);
    CHECK(lx_fw_verify(&info, x.fw_format, NULL, NULL), "OTA md5 over the chunks: %s", info.error);
    make_ota_image("whatever.upt", true, "r1");
    lx_fw_inspect(&x, path, &info);
    CHECK(!lx_fw_verify(&info, x.fw_format, NULL, NULL) && strstr(info.error, "md5 mismatch"), "OTA mismatch: %s", info.error);
    make_ota_image("whatever.upt", false, "r3proii");
    CHECK(!lx_fw_inspect(&x, path, &info) && strstr(info.error, "is for r3proii"), "OTA for another board: %s", info.error);
    make_ota_image("whatever.upt", false, NULL);
    CHECK(lx_fw_inspect(&x, path, &info) && info.nparts == 1 && !info.product[0], "OTA without a marker: %s", info.error);
    make_ota_image("r1.upt", false, "r1");
    CHECK(lx_fw_find(&x, path, sizeof(path)) && strstr(path, "/r1.upt"), "<board>.upt first");
}

static void test_reset(void) {
    reset_tree();
    make_slot("current", "1.0");
    write_text(pathf("%s/.mellowtone/config/settings.txt", p_sd), "volume=10\n");
    write_text(pathf("%s/.mellowtone/library/db", p_sd), "lib");
    lx_boot_counter_write(&cfg, 2);
    lx_t lx = new_lx();
    keys("d d s d s s d d d s"); /* Reset, confirm, OK, Power off */
    lx_end_t end = lx_run(&lx, true);
    CHECK(end == LX_END_POWEROFF, "reset flow ends (%d)", end);
    CHECK(!lx_exists(pathf("%s/.mellowtone/config/settings.txt", p_sd)), "settings moved");
    CHECK(lx_exists(pathf("%s/.mellowtone/library/db", p_sd)), "library kept");
    DIR * d = opendir(pathf("%s/.mellowtone/backup", p_sd));
    bool backed_up = false;
    struct dirent * e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, "reset-", 6) == 0 &&
            lx_exists(pathf("%s/.mellowtone/backup/%s/settings.txt", p_sd, e->d_name)))
            backed_up = true;
    if (d) closedir(d);
    CHECK(backed_up, "settings in backup/reset-<date>/");
    CHECK(lx_boot_counter_read(&cfg) == 0, "boot counter cleared by the reset");
}

static void test_misc(void) {
    CHECK(lx_version_cmp("0.10.0", "0.9.9") > 0, "0.10 > 0.9");
    CHECK(lx_version_cmp("1.2", "1.2.1") < 0, "1.2 < 1.2.1");
    CHECK(lx_version_cmp("2.0", "2.0") == 0, "equal");
    lx_fb_t * fb = lx_fb_open_mem(40, 20);
    lx_fb_fill(fb, 0);
    int w = lx_fb_text(fb, 0, 0, 1, "Hi", 0xFFFFFF);
    CHECK(w == 12, "text width %d", w);
    bool lit = false;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 5; x++)
            if (lx_fb_pixel(fb, x, y)) lit = true;
    CHECK(lit, "glyph drawn");
    lx_fb_close(fb);
}

/* --check-package DIR: installs DIR/sd/.mellowtone/update/mellowtone-r3pro-*.tar
 * (made by tools/release/make_update_package.sh) into DIR/slots: the script and the
 * launcher agree on the format. */
static int check_package(const char * dir) {
    char slots[PATH_MAX], sd[PATH_MAX], ver[64], err[200];
    snprintf(slots, sizeof(slots), "%s/slots", dir);
    snprintf(sd, sizeof(sd), "%s/sd", dir);
    lx_config_t c = { .board = "r3pro", .slots_root = slots, .sd_root = sd, .log_dir = dir };
    lx_pkg_ref_t ref;
    CHECK(lx_pkg_find(&c, &ref), "script package found");
    CHECK(lx_pkg_install(&c, &ref, NULL, NULL, err, sizeof(err)), "script package installs: %s", err);
    CHECK(lx_slot_version(&c, "current", ver, sizeof(ver)) && strcmp(ver, ref.version) == 0, "version %s", ver);
    CHECK(lx_is_executable(pathf("%s/current/mellowtone", slots)), "player executable");
    printf("launcher package script: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char ** argv) {
    if (argc == 3 && strcmp(argv[1], "--check-package") == 0) return check_package(argv[2]);
    char tmpl[] = "/tmp/mt-launcher-test-XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    snprintf(root, sizeof(root), "%s/t", tmpl);
    snprintf(p_slots, sizeof(p_slots), "%s/data/mellowtone", root);
    snprintf(p_boot, sizeof(p_boot), "%s/data/mellowtone-boot", root);
    snprintf(p_sd, sizeof(p_sd), "%s/sd", root);
    snprintf(p_install, sizeof(p_install), "%s/data/mellowtone", root);
    snprintf(p_logs, sizeof(p_logs), "%s/sd/.mellowtone/logs", root);
    snprintf(p_factory, sizeof(p_factory), "%s/usr/bin/hiby_player", root);
    snprintf(p_redeploy, sizeof(p_redeploy), "%s/redeploy", root);
    snprintf(p_adb, sizeof(p_adb), "%s/data/mellowtone/adb_on", root);
    cfg = (lx_config_t) {
        .board = "r3pro", .model = "HiBy R3 Pro", .slots_root = p_slots, .boot_dir = p_boot, .sd_root = p_sd,
        .install_root = p_install, .factory_player = NULL, .stock_player = p_factory, .adb_flag = p_adb,
        .log_dir = p_logs, .redeploy_marker = p_redeploy, .fw_names = r3pro_names, .fw_any_upt = false,
        .fw_format = BOARD_FW_ISO_UPDATE_TXT, .fw_product = "r3pro", .launcher_version = "test",
    };
    fake.fb = lx_fb_open_mem(480, 800);
    lx_log_set_file(NULL);

    test_misc();
    test_fallback_chain();
    test_boot_counter();
    test_menu_flag();
    test_menu_keys();
    test_packages();
    test_firmware();
    test_reset();

    lx_fb_close(fake.fb);
    lx_rm_rf(tmpl);
    printf("launcher: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
