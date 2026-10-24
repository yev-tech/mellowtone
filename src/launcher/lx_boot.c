/* Boot state, the fallback chain and the launcher's whole flow. */
#define _GNU_SOURCE
#include "lx.h"
#include "lx_update.h"
#include "board_launcher.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- boot state in flash --------------------------------------------------- */

static bool boot_path(const lx_config_t * cfg, const char * name, char * out, size_t size) {
    return cfg->boot_dir && lx_join(out, size, cfg->boot_dir, name);
}

int lx_boot_counter_read(const lx_config_t * cfg) {
    char path[PATH_MAX], buf[32];
    if (!boot_path(cfg, BOARD_LAUNCHER_BOOT_COUNTER, path, sizeof(path)) || !lx_read_file(path, buf, sizeof(buf))) return 0;
    int n = atoi(buf);
    return n < 0 ? 0 : n;
}

void lx_boot_counter_write(const lx_config_t * cfg, int n) {
    char path[PATH_MAX];
    if (!boot_path(cfg, BOARD_LAUNCHER_BOOT_COUNTER, path, sizeof(path))) return;
    /* Only a change reaches the flash (the no-display retry rewrites the
     * same value every minute). */
    if (lx_boot_counter_read(cfg) == (n < 0 ? 0 : n)) return;
    if (n <= 0) {
        if (unlink(path) == 0) lx_fsync_dir(cfg->boot_dir);
        return;
    }
    char text[16];
    snprintf(text, sizeof(text), "%d\n", n);
    if (!lx_mkdir_p(cfg->boot_dir) || !lx_write_file_sync(path, text)) lx_log("boot counter: cannot write %s", path);
}

bool lx_menu_flag_present(const lx_config_t * cfg) {
    char path[PATH_MAX];
    return boot_path(cfg, BOARD_LAUNCHER_MENU_FLAG, path, sizeof(path)) && lx_exists(path);
}

bool lx_menu_flag_take(const lx_config_t * cfg) {
    char path[PATH_MAX];
    if (!boot_path(cfg, BOARD_LAUNCHER_MENU_FLAG, path, sizeof(path)) || unlink(path) != 0) return false;
    lx_fsync_dir(cfg->boot_dir);
    return true;
}

const char * lx_level_path(const lx_config_t * cfg, lx_level_t level, char * buf, size_t size) {
    switch (level) {
    case LX_LEVEL_CURRENT: {
        char dir[PATH_MAX];
        if (!lx_join(dir, sizeof(dir), cfg->slots_root, "current") || !lx_join(buf, size, dir, LX_PLAYER_EXE))
            return NULL;
        return lx_is_executable(buf) ? buf : NULL;
    }
    case LX_LEVEL_PREVIOUS:
        /* The previous version lives on the card and runs from flash after a
         * roll back (run_chain()): this is the player that will then start. */
        if (!lx_slots_backup_available(cfg)) return NULL;
        {
            char dir[PATH_MAX];
            if (!lx_join(dir, sizeof(dir), cfg->slots_root, "current") || !lx_join(buf, size, dir, LX_PLAYER_EXE))
                return NULL;
        }
        return buf;
    case LX_LEVEL_FACTORY:
        /* The factory Mellowtone of an image install; on a bundle install
         * (none in the rootfs) the stock player that is still there. */
        if (lx_is_executable(cfg->factory_player)) return snprintf(buf, size, "%s", cfg->factory_player), buf;
        if (lx_is_executable(cfg->stock_player)) return snprintf(buf, size, "%s", cfg->stock_player), buf;
        return NULL;
    default:
        return NULL;
    }
}

/* ---- running the player -------------------------------------------------------- */

typedef enum {
    RUN_HALT,      /* exit 0: the player ran poweroff/reboot through init */
    RUN_REBOOT,    /* exit 75 without the menu flag */
    RUN_MENU,      /* exit 75 with the menu flag ("Restart into recovery menu") */
    RUN_FAILED,    /* every level failed: menu with lx->reason */
} run_result_t;

static const char * const level_names[] = { "current", "previous", "factory" };

static void describe_exit(char * out, size_t size, int status, int64_t ms) {
    if (WIFEXITED(status)) snprintf(out, size, "exit %d after %lld s", WEXITSTATUS(status), (long long) (ms / 1000));
    else if (WIFSIGNALED(status)) snprintf(out, size, "signal %d after %lld s", WTERMSIG(status), (long long) (ms / 1000));
    else snprintf(out, size, "status 0x%x after %lld s", (unsigned) status, (long long) (ms / 1000));
}

static bool take_redeploy_marker(const lx_config_t * cfg) {
    return cfg->redeploy_marker && unlink(cfg->redeploy_marker) == 0;
}

static void player_log(const lx_config_t * cfg, char * out, size_t size) {
    out[0] = '\0';
    if (!cfg->log_dir) return;
    char prev[PATH_MAX];
    if (!lx_join(out, size, cfg->log_dir, "player.out") || !lx_join(prev, sizeof(prev), cfg->log_dir, "player.prev.out")) {
        out[0] = '\0';
        return;
    }
    if (lx_exists(out)) rename(out, prev);
}

/* Runs the chain from `level` down: each level restarts after a crash, and
 * moves on after LX_MAX_QUICK_CRASHES quick ones (the fallback chain). */
static run_result_t run_chain(lx_t * lx, lx_level_t level, bool count_boot) {
    const lx_config_t * cfg = lx->cfg;
    const lx_platform_t * pf = lx->pf;
    char failures[200] = "";
    for (; level < LX_LEVEL_COUNT; level++) {
        char path[PATH_MAX];
        if (!lx_level_path(cfg, level, path, sizeof(path))) {
            lx_log("chain: no %s player", level_names[level]);
            /* Falling past a missing level: the player that does start is
             * the factory one. */
            if (lx->notice[0] && level + 1 == LX_LEVEL_FACTORY) snprintf(lx->notice, sizeof(lx->notice), "factory");
            continue;
        }
        if (level == LX_LEVEL_PREVIOUS) {
            /* Bring the previous version back from the card; the crashing
             * one takes its place there (the menu can roll forward again). */
            if (!lx_slots_rollback(cfg)) {
                lx_log("chain: rolling back to the previous version failed");
                lx_update_log(cfg, "automatic rollback: FAILED");
                if (lx->notice[0] && level + 1 == LX_LEVEL_FACTORY) snprintf(lx->notice, sizeof(lx->notice), "factory");
                continue;
            }
            lx_update_log(cfg, "automatic rollback: done");
            /* The restored version is the current one from now on. */
            lx_boot_counter_write(cfg, 0);
            if (!lx_is_executable(path)) continue;
        }
        int quick = 0;
        for (;;) {
            char log_path[PATH_MAX];
            player_log(cfg, log_path, sizeof(log_path));
            if (count_boot && level == LX_LEVEL_CURRENT && !lx->boot_counted) {
                /* The boot-loop counter's first write: only a boot that
                 * really starts the current version counts (a menu-only
                 * session does not). */
                lx_boot_counter_write(cfg, lx_boot_counter_read(cfg) + 1);
                lx->boot_counted = true;
            }
            lx_log("chain: starting %s (%s)%s%s", level_names[level], path, lx->notice[0] ? ", notice " : "", lx->notice);
            int64_t start = pf->now_ms(pf->ctx);
            long h = pf->spawn(pf->ctx, path, cfg->install_root, log_path[0] ? log_path : NULL, level_names[level],
                               lx->notice[0] ? lx->notice : NULL);
            lx->notice[0] = '\0'; /* shown once */
            int status = 0;
            if (h < 0) {
                lx_log("chain: cannot start %s", path);
                snprintf(lx->last_exit, sizeof(lx->last_exit), "%s: could not start", level_names[level]);
                break;
            }
            bool exited = pf->wait(pf->ctx, h, LX_STABLE_MS, &status);
            if (!exited) {
                /* Ran 90 s: this boot counts as good (the second boot-counter write). */
                if (count_boot && level == LX_LEVEL_CURRENT) lx_boot_counter_write(cfg, 0);
                quick = 0;
                while (!pf->wait(pf->ctx, h, -1, &status)) {}
            }
            int64_t ran = pf->now_ms(pf->ctx) - start;
            char how[80];
            describe_exit(how, sizeof(how), status, ran);
            snprintf(lx->last_exit, sizeof(lx->last_exit), "%s: %s", level_names[level], how);
            lx_log("chain: %s", lx->last_exit);
            if (take_redeploy_marker(cfg)) {
                lx_log("chain: developer redeploy, restarting without counting a crash");
                quick = 0;
                continue;
            }
            bool clean = WIFEXITED(status) &&
                         (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == BOARD_LAUNCHER_EXIT_REBOOT);
            /* Power off, restart or the menu asked for by the user: not a failed boot. */
            if (clean && count_boot && level == LX_LEVEL_CURRENT) lx_boot_counter_write(cfg, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return RUN_HALT;
            if (WIFEXITED(status) && WEXITSTATUS(status) == BOARD_LAUNCHER_EXIT_REBOOT)
                return lx_menu_flag_take(cfg) ? RUN_MENU : RUN_REBOOT;
            quick = ran < LX_QUICK_CRASH_MS ? quick + 1 : 1;
            if (quick >= LX_MAX_QUICK_CRASHES) {
                lx_log("chain: %d quick crashes of %s", quick, level_names[level]);
                size_t n = strlen(failures);
                snprintf(failures + n, sizeof(failures) - n, "%s%s crashed (%s)", n ? "; " : "", level_names[level], how);
                break;
            }
            pf->sleep_ms(pf->ctx, 1000);
        }
        if (level + 1 < LX_LEVEL_COUNT)
            snprintf(lx->notice, sizeof(lx->notice), "%s", level == LX_LEVEL_CURRENT ? "rolled_back" : "factory");
    }
    lx->notice[0] = '\0';
    snprintf(lx->reason, sizeof(lx->reason), "Mellowtone did not start: %s", failures[0] ? failures : "no player installed");
    return RUN_FAILED;
}

/* ---- the whole flow ------------------------------------------------------------ */

lx_end_t lx_run(lx_t * lx, bool menu_key_held) {
    const lx_config_t * cfg = lx->cfg;
    if (!lx_slots_recover(cfg)) lx_log("slots: recovery incomplete");

    bool flag = lx_menu_flag_take(cfg);
    char fw[PATH_MAX];
    lx_pkg_ref_t pkg;
    bool has_fw = lx_fw_find(cfg, fw, sizeof(fw));
    bool has_pkg = lx_pkg_find(cfg, &pkg);
    if (has_fw) lx_log("card: firmware image %s", fw);
    if (has_pkg) lx_log("card: player update %s", pkg.path);

    /* Boot-loop counter: one write when current starts, one more once it has
     * run LX_STABLE_MS (run_chain). At the limit, stop trying current. */
    int boots = lx_boot_counter_read(cfg);
    lx_level_t start = LX_LEVEL_CURRENT;
    if (boots >= LX_MAX_BOOT_ATTEMPTS) {
        lx_log("boot: %d boots without reaching %d s: starting the previous version", boots, LX_STABLE_MS / 1000);
        snprintf(lx->notice, sizeof(lx->notice), "boot_loop");
        start = LX_LEVEL_PREVIOUS;
    }

    lx_action_t action;
    if (menu_key_held || flag || has_fw || has_pkg) {
        lx_log("menu: %s", menu_key_held ? "menu key held" : flag ? "requested by the player" : "update on the card");
        /* Opened only for an update: start the player if nobody answers. */
        bool asked = menu_key_held || flag;
        action = lx_menu(lx, asked ? 0 : 30000);
        if ((asked || lx->fresh_try) && action == LX_ACT_START_CURRENT && start != LX_LEVEL_CURRENT) {
            /* Chosen by hand, or a new current installed / rolled back: a
             * fresh, counted try of the current version. */
            start = LX_LEVEL_CURRENT;
            lx->notice[0] = '\0';
            lx_boot_counter_write(cfg, 1);
            lx->boot_counted = true;
        }
    } else {
        action = start == LX_LEVEL_CURRENT ? LX_ACT_START_CURRENT : LX_ACT_START_PREVIOUS;
    }

    for (;;) {
        lx_level_t level;
        switch (action) {
        case LX_ACT_REBOOT: return LX_END_REBOOT;
        case LX_ACT_POWEROFF: return LX_END_POWEROFF;
        case LX_ACT_START_PREVIOUS: level = LX_LEVEL_PREVIOUS; break;
        case LX_ACT_START_FACTORY: level = LX_LEVEL_FACTORY; break;
        case LX_ACT_START_CURRENT:
        default:
            level = start;
            start = LX_LEVEL_CURRENT; /* the boot-loop rollback applies once */
            break;
        }
        run_result_t r = run_chain(lx, level, level == LX_LEVEL_CURRENT);
        switch (r) {
        case RUN_HALT: return LX_END_HALT;
        case RUN_REBOOT: return LX_END_REBOOT;
        case RUN_MENU:
            lx->reason[0] = '\0';
            break;
        case RUN_FAILED:
            lx_log("menu: %s", lx->reason);
            break;
        }
        action = lx_menu(lx, 0);
        if (action == LX_ACT_START_CURRENT) {
            lx_boot_counter_write(cfg, 1); /* a fresh try, counted */
            lx->boot_counted = true;
        }
    }
}
