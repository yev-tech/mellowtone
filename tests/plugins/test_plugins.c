/* Host tests for the plugin sandbox pieces that need no UI:
 *
 *   json      plugin.json_decode/json_encode (src/plugins/plugin_json.c) at
 *             and past the nesting limit, on a Lua built with API checks
 *             (every push must have stack room), under ASan/LSan (no tree
 *             leaks on the error paths)
 *   paths     plugin_storage_path_is_reserved()/_holds_reserved(): the
 *             plugin-data and config trees, other spellings of them (case,
 *             symlinks), ancestors for rename
 *   storage   plugin ids that differ only in case share one directory
 *
 * Everything happens under mkdtemp(), with the simulator card in ./music. */
#define _GNU_SOURCE
#include "plugin_json.h"
#include "plugin_storage.h"
#include "storage_paths.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- json ------------------------------------------------------------------ */

/* Runs chunk in a fresh state with json_decode/json_encode as globals and
 * returns its string result ("" when it returned none). */
static char * run_lua(const char * chunk) {
    lua_State * L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, l_plugin_json_decode);
    lua_setglobal(L, "json_decode");
    lua_pushcfunction(L, l_plugin_json_encode);
    lua_setglobal(L, "json_encode");
    char * out;
    if (luaL_dostring(L, chunk) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        size_t n = strlen(err ? err : "") + 8;
        out = malloc(n);
        snprintf(out, n, "error: %s", err ? err : "");
    } else {
        const char * s = lua_tostring(L, -1);
        out = strdup(s ? s : "");
    }
    lua_close(L);
    return out;
}

static void check_lua(const char * chunk, const char * want) {
    char * got = run_lua(chunk);
    CHECK(strcmp(got, want) == 0, "%s\n  want: %s\n  got:  %s", chunk, want, got);
    free(got);
}

static void test_json(void) {
    /* Deepest allowed nesting both ways, ceiling 64: each level needs more
     * stack than the LUA_MINSTACK a C function starts with. */
    check_lua("local s = string.rep('[', 64) .. '1' .. string.rep(']', 64)\n"
              "local v, err = json_decode(s, { max_nesting = 64 })\n"
              "if not v then return err end\n"
              "local d = 0 while type(v) == 'table' do v = v[1] d = d + 1 end\n"
              "return tostring(d) .. ':' .. tostring(v)",
              "64:1.0");
    check_lua("local s = string.rep('{\"k\":', 64) .. '\"x\"' .. string.rep('}', 64)\n"
              "local v, err = json_decode(s, { max_nesting = 64 })\n"
              "if not v then return err end\n"
              "local d = 0 while type(v) == 'table' do v = v.k d = d + 1 end\n"
              "return tostring(d) .. ':' .. v",
              "64:x");
    check_lua("local t = 'leaf' for i = 1, 64 do t = { t } end\n"
              "local s, err = json_encode(t, { max_nesting = 64 })\n"
              "return s and #s or err",
              "134");
    check_lua("local t = 'leaf' for i = 1, 64 do t = { k = t } end\n"
              "local s, err = json_encode(t, { max_nesting = 64 })\n"
              "return s and #s or err",
              "390");
    /* Past the limit: a clean error, never a crash. */
    check_lua("local s = string.rep('[', 65) .. string.rep(']', 65)\n"
              "local v, err = json_decode(s, { max_nesting = 64 })\n"
              "return tostring(v) .. ' ' .. err",
              "nil json too deeply nested");
    check_lua("local t = 1 for i = 1, 70 do t = { t } end\n"
              "local s, err = json_encode(t, { max_nesting = 64 })\n"
              "return tostring(s) .. ' ' .. err",
              "nil json too deeply nested");
    /* Deep nesting from inside a deep Lua call chain: the stack check is
     * per call, not only at the top. */
    check_lua("local function deep(n) if n == 0 then\n"
              "  local v = json_decode(string.rep('[', 60) .. string.rep(']', 60), { max_nesting = 64 })\n"
              "  return v and 'ok' or 'fail' end\n"
              "  return (deep(n - 1)) end\n"
              "return deep(150)",
              "ok");
    /* Error paths free the partly built tree (LeakSanitizer checks). */
    check_lua("local t = { a = { 1, 2, { b = print } } }\n"
              "local s, err = json_encode(t) return tostring(s) .. ' ' .. err",
              "nil cannot encode value");
    check_lua("local t = { 1, 2, { [{}] = 1 } }\n"
              "local s, err = json_encode(t) return tostring(s) .. ' ' .. err",
              "nil object keys must be strings");
    check_lua("local v, err = json_decode('[1,2,{\"a\":[1,2,3]}', nil) return tostring(v) .. ' ' .. err",
              "nil invalid json");
    check_lua("local t = {} for i = 1, 20 do t[i] = { i } end\n"
              "local v, err = json_decode(json_encode(t), { max_entries = 10 })\n"
              "return tostring(v) .. ' ' .. err",
              "nil too many entries");
}

/* ---- paths ----------------------------------------------------------------- */

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
    FILE * f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static int rm_cb(const char * path, const struct stat * st, int flag, struct FTW * ftw) {
    (void) st; (void) flag; (void) ftw;
    return remove(path);
}

static void test_paths(void) {
    mkdirs("music/.mellowtone/plugin-data/org.example/storage");
    mkdirs("music/.mellowtone/config");
    mkdirs("music/.mellowtone/plugins");
    mkdirs("music/Playlists");
    write_text("music/.mellowtone/config/settings.txt", "subsonic_password=secret\n");

    static const char * const reserved[] = {
        "music/.mellowtone/config/settings.txt",
        "music/.mellowtone/config",
        "music/.mellowtone/config/",
        "music/.mellowtone/config/subsonic_servers.tsv",
        "music/.mellowtone/plugin-data",
        "music/.mellowtone/plugin-data/org.example/storage/x",
        "music/Playlists/../.mellowtone/./config/settings.txt",
        /* Other spellings the card's file system maps to the same names. */
        "music/.MELLOWTONE/config/settings.txt",
        "music/.mellowtone/CONFIG/settings.txt",
        "music/.Mellowtone/Plugin-Data/org.example",
        /* Symlinks into the trees (existing and dangling targets). */
        "music/link_config/settings.txt",
        "music/link_data",
        "music/dangling/new_file",
    };
    if (symlink(".mellowtone/config", "music/link_config") != 0) perror("symlink");
    if (symlink(".mellowtone/plugin-data", "music/link_data") != 0) perror("symlink");
    if (symlink(".mellowtone/config/new_dir", "music/dangling") != 0) perror("symlink");
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        CHECK(plugin_storage_path_is_reserved(reserved[i]), "reserved: %s", reserved[i]);
        CHECK(plugin_storage_path_holds_reserved(reserved[i]), "holds reserved: %s", reserved[i]);
    }

    static const char * const free_paths[] = {
        "music/.mellowtone/plugins/Some.lua",
        "music/.mellowtone/plugins",
        "music/.mellowtone/configs/x",
        "music/.mellowtone/plugin-data-old/x",
        "music/Playlists/a.m3u",
        "music/new_file.txt",
    };
    for (size_t i = 0; i < sizeof(free_paths) / sizeof(free_paths[0]); i++) {
        CHECK(!plugin_storage_path_is_reserved(free_paths[i]), "not reserved: %s", free_paths[i]);
        CHECK(!plugin_storage_path_holds_reserved(free_paths[i]), "holds nothing: %s", free_paths[i]);
    }

    /* rename: an ancestor of a reserved tree moves the tree with it. */
    static const char * const ancestors[] = {
        "music/.mellowtone", "music/.mellowtone/", "music/.MELLOWTONE", "music", "music/./.mellowtone/../.mellowtone",
        "/", ".",
    };
    for (size_t i = 0; i < sizeof(ancestors) / sizeof(ancestors[0]); i++) {
        CHECK(!plugin_storage_path_is_reserved(ancestors[i]), "an ancestor is not inside: %s", ancestors[i]);
        CHECK(plugin_storage_path_holds_reserved(ancestors[i]), "ancestor holds reserved: %s", ancestors[i]);
    }
    if (symlink(".mellowtone", "music/link_state") != 0) perror("symlink");
    CHECK(plugin_storage_path_holds_reserved("music/link_state"), "a symlink to the state root holds reserved");
}

static void test_storage(void) {
    mt_state_init();
    CHECK(plugin_storage_set("Org.Example.Case", "k", "v1", 2), "set under a mixed-case id");
    char * value = NULL;
    size_t len = 0;
    CHECK(plugin_storage_get("org.example.case", "k", &value, &len) && len == 2 && memcmp(value, "v1", 2) == 0,
          "same data under the lower-case id");
    free(value);
    struct stat st;
    CHECK(stat("music/.mellowtone/plugin-data/org.example.case", &st) == 0 && S_ISDIR(st.st_mode),
          "the directory is the lower-case id");
    CHECK(stat("music/.mellowtone/plugin-data/Org.Example.Case", &st) != 0, "no mixed-case directory");
}

int main(void) {
    test_json();

    char tmpl[] = "/tmp/mt-plugintest-XXXXXX";
    char * base = mkdtemp(tmpl);
    if (!base) { perror("mkdtemp"); return 2; }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    if (chdir(base) != 0) { perror("chdir"); return 2; }
    test_paths();
    test_storage();
    if (chdir(cwd) != 0) {}
    nftw(base, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

    printf("plugins: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
