#ifndef LX_H
#define LX_H

/* mellowtone-launcher core. Board-independent: everything about
 * the device comes in through lx_config_t (paths, built from BOARD by
 * main.c) and lx_platform_t (processes, clock, power, input), so the host
 * tests drive the same code with fakes and temporary directories. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LX_PLAYER_EXE "mellowtone"            /* the player inside a version slot */
#define LX_LAUNCHER_EXE "mellowtone-launcher" /* the launcher inside a slot / factory dir */
#define LX_MANIFEST "MANIFEST"                /* package manifest, kept in the slot */
#define LX_VERSION_FILE "VERSION"             /* a hand-made slot's version (no manifest) */

#define LX_QUICK_CRASH_MS 60000   /* a crash sooner than this after start is "quick" */
#define LX_STABLE_MS 90000        /* running this long clears the boot counter */
#define LX_MAX_QUICK_CRASHES 3
#define LX_MAX_BOOT_ATTEMPTS 3
#define LX_MIN_BATTERY 50         /* firmware update refused below this */

typedef enum { LX_LEVEL_CURRENT = 0, LX_LEVEL_PREVIOUS, LX_LEVEL_FACTORY, LX_LEVEL_COUNT } lx_level_t;

typedef enum {
    LX_KEY_NONE = 0, /* timeout */
    LX_KEY_UP,
    LX_KEY_DOWN,
    LX_KEY_SELECT,
    LX_KEY_BACK,
    LX_KEY_TAP, /* touch: *tap_y holds the screen row */
} lx_key_t;

/* How lx_run() ended: what main() does next. */
typedef enum {
    LX_END_HALT = 0,  /* the player shut the system down (exit 0): wait for init */
    LX_END_REBOOT,
    LX_END_POWEROFF,
} lx_end_t;

typedef struct lx_fb lx_fb_t;

typedef struct {
    void * ctx;
    int64_t (*now_ms)(void * ctx); /* monotonic */
    void (*sleep_ms)(void * ctx, int ms);
    /* Starts path with cwd, stdout/stderr appended to log_path (or NULL), and
     * MELLOWTONE_SLOT / MELLOWTONE_NOTICE in its environment (notice may be
     * NULL). Returns a handle > 0, or -1. */
    long (*spawn)(void * ctx, const char * path, const char * cwd, const char * log_path, const char * slot,
                  const char * notice);
    /* Waits up to timeout_ms (-1: forever). True when it exited: *status is
     * the wait status. */
    bool (*wait)(void * ctx, long handle, int timeout_ms, int * status);
    bool (*enter_recovery)(void * ctx); /* returns only on failure */
    bool (*enable_adb)(void * ctx);
    int (*battery_percent)(void * ctx); /* -1: unknown */
    /* Next key press, waiting up to timeout_ms (-1: forever). */
    lx_key_t (*next_key)(void * ctx, int timeout_ms, int * tap_y);
    /* The menu's screen and input: opened when the menu shows, closed before
     * a player starts (it takes the framebuffer over). NULL: no display. */
    lx_fb_t * (*display_open)(void * ctx);
    void (*display_close)(void * ctx, lx_fb_t * fb);
} lx_platform_t;

typedef struct {
    const char * board; /* "r3pro": package and image names */
    const char * model; /* "HiBy R3 Pro" */
    const char * slots_root;
    const char * boot_dir;
    const char * sd_root;
    const char * install_root;   /* the player's working directory */
    const char * factory_player; /* executable, or NULL */
    const char * stock_player;   /* executable, or NULL */
    const char * adb_flag;       /* flash flag "ADB on", or NULL */
    const char * log_dir;        /* boot.log / update.log / player.out */
    const char * redeploy_marker; /* developer redeploy (not a crash), or NULL */
    const char * const * fw_names;
    bool fw_any_upt;
    int fw_format;                /* board_fw_format_t */
    const char * fw_product;
    const char * launcher_version;
} lx_config_t;

typedef struct {
    const lx_config_t * cfg;
    const lx_platform_t * pf;
    lx_fb_t * fb;          /* while the menu shows (pf->display_open) */
    char reason[260];      /* why the menu opened, shown at its top */
    char last_exit[120];   /* the last player exit, for Device info */
    char notice[32];       /* MELLOWTONE_NOTICE for the next start */
    int menu_idle_ms;      /* power off after this long in the menu without input */
    bool boot_counted;     /* this boot's attempt is in the boot counter */
    bool fresh_try;        /* the menu asks for current despite a boot loop: Start chosen, or
                              a version installed / rolled back */
} lx_t;

/* ---- lx_util.c ---- */
void lx_log_set_file(const char * path); /* NULL: stderr only */
void lx_log(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
void lx_vlog(const char * fmt, va_list ap);
bool lx_join(char * out, size_t size, const char * a, const char * b);
bool lx_mkdir_p(const char * dir);
bool lx_rm_rf(const char * path);
void lx_fsync_dir(const char * dir);
bool lx_exists(const char * path);
bool lx_is_dir(const char * path);
bool lx_is_executable(const char * path);
bool lx_write_file_sync(const char * path, const char * text); /* tmp + fsync + rename + fsync dir */
bool lx_read_file(const char * path, char * buf, size_t size); /* NUL-terminated, false if missing */
/* Appends a line to <log_dir>/update.log. */
void lx_update_log(const lx_config_t * cfg, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
/* Compares version strings numerically per component ("0.10.1" > "0.9"). */
int lx_version_cmp(const char * a, const char * b);

/* ---- lx_boot.c: boot state, the fallback chain, the whole flow ---- */
int lx_boot_counter_read(const lx_config_t * cfg);
void lx_boot_counter_write(const lx_config_t * cfg, int n); /* 0 removes it */
bool lx_menu_flag_take(const lx_config_t * cfg);            /* true once per flag */
bool lx_menu_flag_present(const lx_config_t * cfg);
/* The executable of a level (NULL when there is none). */
const char * lx_level_path(const lx_config_t * cfg, lx_level_t level, char * buf, size_t size);
/* The whole launcher after the board hooks: slot recovery, menu flag,
 * updates on the card, boot counter, then the player under supervision with
 * the fallback chain and the menu. */
lx_end_t lx_run(lx_t * lx, bool menu_key_held);

/* ---- lx_menu.c ---- */
typedef enum {
    LX_ACT_START_CURRENT,
    LX_ACT_START_PREVIOUS,
    LX_ACT_START_FACTORY,
    LX_ACT_REBOOT,
    LX_ACT_POWEROFF,
} lx_action_t;
/* The recovery menu. Handles installs, ADB, reset and info
 * itself; returns what to start, or reboot / power off. auto_start_ms > 0:
 * starts the player after that long without input (the menu opened only
 * because an update is on the card). */
lx_action_t lx_menu(lx_t * lx, int auto_start_ms);
/* A list of choices with a title and body text; returns the chosen index,
 * -1 for Back, -2 on timeout. */
int lx_ui_choose(lx_t * lx, const char * title, const char * body, const char * const * items, int n, int sel,
                 int timeout_ms);
void lx_ui_message(lx_t * lx, const char * title, const char * body);
void lx_ui_progress(lx_t * lx, const char * title, const char * body, int percent);

#endif
