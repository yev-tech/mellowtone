/* Host tests for the state root on the card, the radio lists, the
 * settings reset, settings writes (no USB mode saved) and card swaps:
 * src/core/state_reset.c, src/core/storage_paths.c, src/core/settings.c and
 * src/library/radio_list.c. Everything happens under mkdtemp(). */
#define _GNU_SOURCE
#include "board.h"
#include "radio_list.h"
#include "settings.h"
#include "state_reset.h"
#include "storage_paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* settings.c's board queries: a board with no capabilities. */
const board_desc_t BOARD = { 0 };
bool board_has(board_cap_t cap) { (void) cap; return false; }
bool board_impl_missing(board_cap_t cap) { (void) cap; return false; }

static int failures, checks;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

/* ---- file helpers ---------------------------------------------------------- */

static void path_fmt(char * out, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
static void path_fmt(char * out, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, PATH_MAX, fmt, ap);
    va_end(ap);
}

static void mkdirs(const char * dir) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", dir);
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void write_text(const char * path, const char * text) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirs(dir); }
    FILE * f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char * read_text(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strdup(buf);
}

static bool exists(const char * path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool is_dir(const char * path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_has(const char * path, const char * needle) {
    char * t = read_text(path);
    bool ok = t && strstr(t, needle);
    free(t);
    return ok;
}

static int count_entries(const char * dir) {
    DIR * d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent * e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

static int rm_cb(const char * path, const struct stat * st, int flag, struct FTW * ftw) {
    (void) st; (void) flag; (void) ftw;
    chmod(path, 0755);
    return remove(path);
}

static void rm_tree(const char * dir) {
    /* Make every directory writable first, then delete depth-first. */
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "chmod -R u+rwx '%s' 2>/dev/null", dir);
    if (system(cmd) != 0) {}
    nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static char * make_tmp(void) {
    char tmpl[] = "/tmp/mt-statetest-XXXXXX";
    char * d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(2); }
    return strdup(d);
}

/* ---- tests ------------------------------------------------------------------- */

/* The state root and its subdirectories; a second run changes nothing and
 * keeps what is there. */
static void test_layout(void) {
    char * base = make_tmp();
    char p[PATH_MAX];
    CHECK(mt_state_create_layout_at(base), "layout created");
    static const char * const dirs[] = {
        "config", "plugins", "plugin-data", "library", "cache", "logs", "backup", "update",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        path_fmt(p, "%s/.mellowtone/%s", base, dirs[i]);
        CHECK(is_dir(p), ".mellowtone/%s exists", dirs[i]);
    }
    path_fmt(p, "%s/.mellowtone", base);
    CHECK(count_entries(p) == 8, "only the standard directories (%d)", count_entries(p));
    path_fmt(p, "%s/.mellowtone/config/settings.txt", base);
    write_text(p, "volume=0.5\n");
    CHECK(mt_state_create_layout_at(base), "second run");
    CHECK(file_has(p, "volume=0.5"), "existing files kept");
    path_fmt(p, "%s/.mellowtone/cache", base);
    rmdir(p);
    write_text(p, "not a directory");
    CHECK(!mt_state_create_layout_at(base), "a file in the way fails");
    path_fmt(p, "%s/missing", base);
    CHECK(!mt_state_create_layout_at(p), "no card root, nothing created");
    CHECK(!exists(p), "the card root itself is never created");
    rm_tree(base);
    free(base);
}

static void test_radio_cases(void) {
    char * base = make_tmp();
    char b[PATH_MAX], s[PATH_MAX], h[PATH_MAX];
    path_fmt(b, "%s/base.txt", base);
    path_fmt(s, "%s/sd.txt", base);
    path_fmt(h, "%s/hidden.txt", base);

    /* normalization */
    char n1[RADIO_URL_MAX], n2[RADIO_URL_MAX];
    radio_url_normalize(" HTTP://Example.COM:80/Path/ ", n1, sizeof(n1));
    radio_url_normalize("http://example.com/Path", n2, sizeof(n2));
    CHECK(strcmp(n1, n2) == 0, "normalize: %s == %s", n1, n2);
    radio_url_normalize("https://example.com:8443/a?b=1", n1, sizeof(n1));
    CHECK(strcmp(n1, "https://example.com:8443/a?b=1") == 0, "normalize keeps other ports and queries: %s", n1);
    radio_url_normalize("http://a.example/x", n1, sizeof(n1));
    radio_url_normalize("https://a.example/x", n2, sizeof(n2));
    CHECK(strcmp(n1, n2) != 0, "http and https stay different");

    /* merged view with a hidden station and a bare URL line */
    write_text(b, "A | http://a.example/\nB | http://b.example/\nhttp://c.example/\n");
    write_text(s, "http://d.example/\nB2 | http://B.example\n");
    write_text(h, "http://a.example\n");
    radio_entries_t v;
    CHECK(radio_list_build(b, s, h, false, &v), "view builds");
    CHECK(v.count == 3 && strcmp(v.items[0].name, "B2") == 0 && strcmp(v.items[1].name, "http://c.example/") == 0 &&
              strcmp(v.items[2].name, "http://d.example/") == 0 && v.items[0].base && !v.items[2].base,
          "view: renamed base, bare URL, addition (count %d)", v.count);
    radio_entries_free(&v);
    CHECK(radio_list_build(b, s, h, true, &v) && v.count == 4 && v.items[0].hidden, "view with hidden");
    radio_entries_free(&v);
    rm_tree(base);
    free(base);
}

/* The player's own list API on the simulator layout (./Radio.txt is the
 * base, ./music the card). */
static void test_radio_api(void) {
    char * base = make_tmp();
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    if (chdir(base) != 0) { perror("chdir"); exit(2); }
    mkdirs("music");
    write_text("Radio.txt", "A | http://a.example/\nB | http://b.example/\n");
    mt_state_init();
    CHECK(radio_list_editable(), "the simulator card is writable");
    CHECK(radio_list_add("New", "http://new.example/s") == RADIO_OK, "add");
    CHECK(radio_list_add("Again", "http://NEW.example/s/") == RADIO_EXISTS, "add twice");
    CHECK(radio_list_add("x", "ftp://x") == RADIO_BAD_URL, "add a non-http URL");
    CHECK(file_has("music/Radio.txt", "New | http://new.example/s"), "added to the card list");
    CHECK(radio_list_remove("http://a.example/") == RADIO_OK, "remove a base station");
    CHECK(file_has("music/.mellowtone/config/radio_hidden.txt", "http://a.example/"), "base station hidden");
    CHECK(radio_list_hidden_count() == 1, "one hidden");
    CHECK(radio_list_remove("http://new.example/s") == RADIO_OK && !file_has("music/Radio.txt", "new.example"),
          "remove a card station");
    CHECK(radio_list_remove("http://nowhere.example/") == RADIO_NOT_FOUND, "remove an unknown station");
    CHECK(radio_list_add("A again", "http://a.example") == RADIO_ADDED_RESTORED, "adding a hidden station restores it");
    CHECK(radio_list_hidden_count() == 0, "nothing hidden");
    CHECK(radio_list_remove("http://b.example/") == RADIO_OK && radio_list_restore("http://b.example/") == RADIO_OK,
          "remove and restore");
    radio_entries_t v;
    CHECK(radio_list_load(&v, false) && v.count == 2, "two stations shown (%d)", v.count);
    radio_entries_free(&v);
    CHECK(file_has("Radio.txt", "A | http://a.example/\nB | http://b.example/\n"), "base list unchanged");

    /* settings reset: config moves to backup/reset-<date> */
    write_text("music/.mellowtone/config/settings.txt", "volume=0.1\n");
    CHECK(mt_state_reset_config(), "reset moves config");
    CHECK(!exists("music/.mellowtone/config/settings.txt"), "settings gone from config/");
    CHECK(count_entries("music/.mellowtone/backup") == 1, "one reset backup");
    if (chdir(cwd) != 0) {}
    rm_tree(base);
    free(base);
}

/* A save that cannot be written completely (a full card: here a file size
 * limit) leaves the settings file as it was and no temp file behind. */
static void test_settings_write(void) {
    char * base = make_tmp();
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    if (chdir(base) != 0) { perror("chdir"); exit(2); }
    mkdirs("music");
    CHECK(mt_state_create_layout(), "layout");
    mt_state_init();

    player_settings_t s;
    CHECK(!settings_load(&s), "no settings yet");
    s.setup_complete = true;
    s.volume = 0.25f;
    snprintf(s.subsonic_password, sizeof(s.subsonic_password), "secret");
    settings_save(&s);
    CHECK(file_has(SETTINGS_FILE_PATH, "volume=0.250") && file_has(SETTINGS_FILE_PATH, "setup_complete=1"),
          "saved");
    char * before = read_text(SETTINGS_FILE_PATH);

    struct rlimit old, lim;
    getrlimit(RLIMIT_FSIZE, &old);
    lim = old;
    lim.rlim_cur = 256;
    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &lim);
    s.volume = 0.75f;
    settings_save(&s);
    setrlimit(RLIMIT_FSIZE, &old);

    char * after = read_text(SETTINGS_FILE_PATH);
    CHECK(before && after && strcmp(before, after) == 0, "a failed write leaves the settings file as it was");
    CHECK(!exists(SETTINGS_FILE_PATH ".tmp"), "no temp file left");
    player_settings_t loaded;
    CHECK(settings_load(&loaded) && loaded.setup_complete && strcmp(loaded.subsonic_password, "secret") == 0,
          "setup and credentials survive");
    settings_save(&s);
    CHECK(file_has(SETTINGS_FILE_PATH, "volume=0.750"), "the next save writes");
    CHECK(!file_has(SETTINGS_FILE_PATH, "subsonic_saved_"), "saved servers are not in the settings file");
    /* The USB mode is per connection, never saved; ADB alone is, and an
     * older file's usb_mode=2 (ADB) still reads as it. */
    CHECK(!file_has(SETTINGS_FILE_PATH, "usb_mode") && file_has(SETTINGS_FILE_PATH, "adb_enabled=0"),
          "no USB mode saved");
    write_text(SETTINGS_FILE_PATH, "setup_complete=1\nusb_mode=1\n");
    CHECK(settings_load(&loaded) && !loaded.adb_enabled, "an old saved USB DAC mode is ignored");
    write_text(SETTINGS_FILE_PATH, "setup_complete=1\nusb_mode=2\n");
    CHECK(settings_load(&loaded) && loaded.adb_enabled, "an old saved ADB mode keeps ADB on");
    settings_save(&loaded);
    CHECK(!file_has(SETTINGS_FILE_PATH, "usb_mode") && file_has(SETTINGS_FILE_PATH, "adb_enabled=1"),
          "rewritten without the USB mode");
    free(before);
    free(after);

    if (chdir(cwd) != 0) {}
    rm_tree(base);
    free(base);
}

/* Config follows the card it was loaded from: away it is not written; the
 * same card back is written again; another card with its own settings is
 * left alone, also when swapped between two polls. */
static void test_card_swap(void) {
    char * base = make_tmp();
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    if (chdir(base) != 0) { perror("chdir"); exit(2); }
    mkdirs("music/.mellowtone/config");
    write_text("music/.mellowtone/config/settings.txt", "volume=0.1\n");
    CHECK(mt_state_create_layout(), "layout");
    mt_state_init();
    (void) mt_state_poll();
    CHECK(exists(MT_CARD_ID_FILE), "the card got an identity");
    CHECK(mt_config_writable(), "config writable on the boot card");

    CHECK(rename("music", "card_a") == 0, "card out");
    CHECK(mt_state_poll() == MT_NOTICE_NO_CARD && !mt_config_writable(), "no card: config not writable");
    CHECK(rename("card_a", "music") == 0, "card in");
    CHECK(mt_state_poll() == MT_NOTICE_NONE && mt_config_writable(), "the same card back: writable again");

    /* Swapped between two polls: card B has its own settings. */
    CHECK(rename("music", "card_a") == 0, "card A out");
    mkdirs("music/.mellowtone/config");
    write_text("music/.mellowtone/config/settings.txt", "volume=0.9\n");
    CHECK(mt_state_poll() == MT_NOTICE_RESTART_FOR_SETTINGS, "another card with settings: restart notice");
    CHECK(!mt_config_writable(), "its settings are not overwritten");
    CHECK(!mt_state_prepare_write(SETTINGS_FILE_PATH), "settings writes refused");
    CHECK(file_has("music/.mellowtone/config/settings.txt", "volume=0.9"), "card B's settings kept");

    if (chdir(cwd) != 0) {}
    rm_tree(base);
    free(base);
}

int main(void) {
    test_layout();
    test_radio_cases();
    test_radio_api();
    test_settings_write();
    test_card_swap();
    printf("state: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
