/* The recovery menu: large text, one item per line, keys only
 * (Vol+/Vol- or the knob move, Play selects, Power goes back), touch where
 * there is a panel. */
#define _GNU_SOURCE
#include "lx.h"
#include "lx_fb.h"
#include "lx_update.h"
#include "state_reset.h"
#include "board_launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>

#define COL_BG 0x121212
#define COL_TITLE_BG 0x1E4D6B
#define COL_TEXT 0xF0F0F0
#define COL_DIM 0xA8A8A8
#define COL_SEL_BG 0x35556B
#define COL_WARN 0xFFB060
#define COL_BAR 0x4FA3D9

/* Row geometry of the last frame, for taps. */
static int rows_y0, rows_h, rows_first, rows_count;

static int item_scale(const lx_fb_t * fb) { return lx_fb_w(fb) >= 480 ? 3 : 2; }
static int body_scale(const lx_fb_t * fb) { return lx_fb_w(fb) >= 480 ? 2 : 2; }

/* Draws text wrapped at word boundaries; returns the next y. */
static int draw_wrapped(lx_fb_t * fb, int x, int y, int max_w, int scale, const char * text, uint32_t col, int max_y) {
    int per_line = max_w / lx_fb_char_w(scale);
    if (per_line < 4) per_line = 4;
    const char * p = text;
    char line[160];
    while (*p && y + lx_fb_line_h(scale) <= max_y) {
        const char * nl = strchr(p, '\n');
        int len = nl ? (int) (nl - p) : (int) strlen(p);
        int take = len;
        if (take > per_line) {
            take = per_line;
            while (take > 0 && p[take] != ' ') take--;
            if (take == 0) take = per_line;
        }
        if (take >= (int) sizeof(line)) take = (int) sizeof(line) - 1;
        memcpy(line, p, (size_t) take);
        line[take] = '\0';
        lx_fb_text(fb, x, y, scale, line, col);
        y += lx_fb_line_h(scale);
        p += take;
        while (*p == ' ') p++;
        if (nl && p == nl) p++;
    }
    return y;
}

static void render(lx_t * lx, const char * title, const char * body, const char * const * items, int n, int sel,
                   const char * footer) {
    lx_fb_t * fb = lx->fb;
    int w = lx_fb_w(fb), h = lx_fb_h(fb);
    int is = item_scale(fb), bs = body_scale(fb);
    int pad = 4 * is;
    lx_fb_fill(fb, COL_BG);
    int title_h = lx_fb_line_h(is) + 2 * pad;
    lx_fb_rect(fb, 0, 0, w, title_h, COL_TITLE_BG);
    lx_fb_text(fb, pad, pad + is, is, title, COL_TEXT);
    int footer_h = footer ? lx_fb_line_h(bs) + pad : 0;
    int y = title_h + pad;
    if (body && body[0]) y = draw_wrapped(fb, pad, y, w - 2 * pad, bs, body, COL_DIM, h - footer_h - lx_fb_line_h(is)) + pad;
    rows_h = lx_fb_line_h(is) + 2 * pad;
    rows_y0 = y;
    int fit = (h - footer_h - y) / rows_h;
    if (fit < 1) fit = 1;
    rows_first = 0;
    if (sel >= fit) rows_first = sel - fit + 1;
    rows_count = n - rows_first < fit ? n - rows_first : fit;
    for (int i = 0; i < rows_count; i++) {
        int idx = rows_first + i;
        int ry = y + i * rows_h;
        if (idx == sel) {
            lx_fb_rect(fb, 0, ry, w, rows_h, COL_SEL_BG);
            lx_fb_rect(fb, 0, ry, is * 2, rows_h, COL_BAR);
        }
        lx_fb_text(fb, pad + is * 2, ry + pad + is / 2, is, items[idx], idx == sel ? COL_TEXT : COL_DIM);
    }
    if (rows_first > 0) lx_fb_text(fb, w - lx_fb_char_w(is) - pad, y - lx_fb_line_h(is) / 2, is, "^", COL_DIM);
    if (rows_first + rows_count < n) lx_fb_text(fb, w - lx_fb_char_w(is) - pad, y + rows_count * rows_h - lx_fb_line_h(is) + pad, is, "v", COL_DIM);
    if (footer && lx_fb_text_w(bs, footer) <= w - 2 * pad) lx_fb_text(fb, pad, h - footer_h + pad / 2, bs, footer, COL_DIM);
    lx_fb_flush(fb);
}

int lx_ui_choose(lx_t * lx, const char * title, const char * body, const char * const * items, int n, int sel,
                 int timeout_ms) {
    const lx_platform_t * pf = lx->pf;
    if (n <= 0) return -1;
    if (sel < 0 || sel >= n) sel = 0;
    if (!lx->fb) return timeout_ms > 0 ? -2 : -1;
    int64_t start = pf->now_ms(pf->ctx);
    int64_t last_input = start;
    bool counting = timeout_ms > 0;
    for (;;) {
        char body2[600];
        const char * shown = body;
        int64_t now = pf->now_ms(pf->ctx);
        if (counting) {
            int64_t left = timeout_ms - (now - start);
            if (left <= 0) return -2;
            snprintf(body2, sizeof(body2), "%s%sStarting Mellowtone in %lld s (any key stops this).", body ? body : "",
                     body && body[0] ? "\n" : "", (long long) ((left + 999) / 1000));
            shown = body2;
        }
        if (lx->menu_idle_ms > 0 && now - last_input >= lx->menu_idle_ms) return -3;
        render(lx, title, shown, items, n, sel, "Vol+/-: move  Play: OK  Power: back");
        int tap_y = -1;
        lx_key_t k = pf->next_key(pf->ctx, counting ? 1000 : 5000, &tap_y);
        if (k == LX_KEY_NONE) continue;
        counting = false;
        last_input = pf->now_ms(pf->ctx);
        switch (k) {
        case LX_KEY_UP: sel = (sel + n - 1) % n; break;
        case LX_KEY_DOWN: sel = (sel + 1) % n; break;
        case LX_KEY_SELECT: return sel;
        case LX_KEY_BACK: return -1;
        case LX_KEY_TAP:
            if (tap_y >= rows_y0 && rows_h > 0) {
                int row = (tap_y - rows_y0) / rows_h;
                if (row < rows_count) return rows_first + row;
            }
            break;
        default: break;
        }
    }
}

void lx_ui_message(lx_t * lx, const char * title, const char * body) {
    lx_log("ui: %s: %s", title, body);
    static const char * const ok[] = { "OK" };
    (void) lx_ui_choose(lx, title, body, ok, 1, 0, 0);
}

void lx_ui_progress(lx_t * lx, const char * title, const char * body, int percent) {
    lx_fb_t * fb = lx->fb;
    if (!fb) return;
    int w = lx_fb_w(fb), h = lx_fb_h(fb);
    int is = item_scale(fb), pad = 4 * is;
    lx_fb_fill(fb, COL_BG);
    int title_h = lx_fb_line_h(is) + 2 * pad;
    lx_fb_rect(fb, 0, 0, w, title_h, COL_TITLE_BG);
    lx_fb_text(fb, pad, pad + is, is, title, COL_TEXT);
    int y = draw_wrapped(fb, pad, title_h + pad, w - 2 * pad, body_scale(fb), body ? body : "", COL_DIM, h / 2) + pad;
    int bar_w = w - 4 * pad, bar_h = 6 * is;
    lx_fb_rect(fb, 2 * pad, y, bar_w, bar_h, 0x303030);
    if (percent > 0) lx_fb_rect(fb, 2 * pad, y, bar_w * (percent > 100 ? 100 : percent) / 100, bar_h, COL_BAR);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d %%", percent);
    lx_fb_text(fb, 2 * pad, y + bar_h + pad, is, pct, COL_TEXT);
    lx_fb_flush(fb);
}

/* ---- menu actions ------------------------------------------------------------- */

typedef struct {
    lx_t * lx;
    const char * title;
    char body[200];
} prog_ctx_t;

static void progress_cb(void * c, int percent, const char * what) {
    prog_ctx_t * p = c;
    char body[300];
    snprintf(body, sizeof(body), "%s\n%s", p->body, what ? what : "");
    lx_ui_progress(p->lx, p->title, body, percent);
}

static bool confirm(lx_t * lx, const char * title, const char * body, const char * yes) {
    const char * const items[] = { "Cancel", yes };
    return lx_ui_choose(lx, title, body, items, 2, 0, 0) == 1;
}

/* A new current version (installed or rolled back): its first start is a
 * fresh one, without the boot-loop rollback to the old version. */
static void new_current(lx_t * lx) {
    lx_boot_counter_write(lx->cfg, 0);
    lx->notice[0] = '\0';
    lx->fresh_try = true;
}

static void do_install_package(lx_t * lx, const lx_pkg_ref_t * pkg) {
    char cur[64], body[300];
    bool have = lx_slot_version(lx->cfg, "current", cur, sizeof(cur));
    snprintf(body, sizeof(body), "Install Mellowtone %s?\nInstalled now: %s\n%s", pkg->version, have ? cur : "none",
             have ? "The installed version is saved to the card as the previous one." : "");
    if (!confirm(lx, "Player update", body, "Install")) return;
    prog_ctx_t pc = { lx, "Installing", "" };
    snprintf(pc.body, sizeof(pc.body), "Mellowtone %s. Do not switch off.", pkg->version);
    lx_ui_progress(lx, pc.title, pc.body, 0);
    char err[200];
    if (lx_pkg_install(lx->cfg, pkg, progress_cb, &pc, err, sizeof(err))) {
        new_current(lx);
        snprintf(body, sizeof(body), "Mellowtone %s is installed.", pkg->version);
        lx_ui_message(lx, "Player update", body);
    } else if (lx_slots_swap_pending(lx->cfg)) {
        snprintf(body, sizeof(body), "Not finished: %s.\nThe install will be completed at the next start.", err);
        lx_ui_message(lx, "Player update", body);
    } else {
        snprintf(body, sizeof(body), "Not installed: %s.\nThe installed version is kept.", err);
        lx_ui_message(lx, "Player update", body);
    }
}

static void do_install_firmware(lx_t * lx, const char * path) {
    const lx_config_t * cfg = lx->cfg;
    const lx_platform_t * pf = lx->pf;
    static lx_fw_info_t info;
    char body[600];
    if (!lx_fw_inspect(cfg, path, &info)) {
        snprintf(body, sizeof(body), "%s:\n%s.", info.file, info.error);
        lx_ui_message(lx, "Firmware update", body);
        return;
    }
    int battery = pf->battery_percent(pf->ctx);
    char parts[160] = "";
    for (int i = 0; i < info.nparts; i++) {
        size_t n = strlen(parts);
        snprintf(parts + n, sizeof(parts) - n, "%s%s", i ? ", " : "", info.parts[i].name);
    }
    char battery_text[24];
    if (battery >= 0) snprintf(battery_text, sizeof(battery_text), "%d %%", battery);
    else snprintf(battery_text, sizeof(battery_text), "unknown");
    snprintf(body, sizeof(body), "File: %s (%.1f MB)\nModel: %s\nVersion: %s\nParts: %s\nBattery: %s%s%s", info.file,
             (double) info.size / (1024.0 * 1024.0), info.product[0] ? info.product : "not named",
             info.version[0] ? info.version : "not named", parts, battery_text, info.warning[0] ? "\n" : "",
             info.warning);
    if (battery < LX_MIN_BATTERY) {
        char refuse[700];
        snprintf(refuse, sizeof(refuse), "%s\n\nThe battery must be at %d %% or more. Charge first.", body, LX_MIN_BATTERY);
        lx_ui_message(lx, "Firmware update", refuse);
        return;
    }
    if (!confirm(lx, "Firmware update", body, "Check the image")) return;
    prog_ctx_t pc = { lx, "Checking", "" };
    snprintf(pc.body, sizeof(pc.body), "%s: md5 of every part", info.file);
    lx_ui_progress(lx, pc.title, pc.body, 0);
    if (!lx_fw_verify(&info, cfg->fw_format, progress_cb, &pc)) {
        snprintf(body, sizeof(body), "%s is damaged (%s). Copy it to the card again.", info.file, info.error);
        lx_ui_message(lx, "Firmware update", body);
        lx_update_log(cfg, "firmware %s refused: %s", info.file, info.error);
        return;
    }
    snprintf(body, sizeof(body), "%s checked: every part is intact.\nInstall it now? The device restarts into the "
                                 "updater.", info.file);
    if (!confirm(lx, "Firmware update", body, "Install")) return;
    snprintf(body, sizeof(body), "Really install %s?\nDo not switch off or remove the card until the device has "
                                 "restarted by itself.", info.file);
    if (!confirm(lx, "Last check", body, "Yes, install now")) return;
    battery = pf->battery_percent(pf->ctx);
    if (battery < LX_MIN_BATTERY) {
        lx_ui_message(lx, "Firmware update", "The battery dropped below the limit. Charge first.");
        return;
    }
    lx_update_log(cfg, "firmware %s: entering recovery (battery %d %%)", info.file, battery);
    lx_ui_progress(lx, "Firmware update", "Restarting into the updater...", 100);
    pf->enter_recovery(pf->ctx);
    lx_update_log(cfg, "firmware %s: entering recovery failed", info.file);
    lx_ui_message(lx, "Firmware update", "Could not restart into the updater. Nothing was changed.");
}

static void do_enable_adb(lx_t * lx) {
    const lx_config_t * cfg = lx->cfg;
    if (cfg->adb_flag) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", cfg->adb_flag);
        char * slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            lx_mkdir_p(dir);
        }
        if (!lx_write_file_sync(cfg->adb_flag, "")) lx_log("adb: cannot write %s", cfg->adb_flag);
    }
    bool ok = lx->pf->enable_adb(lx->pf->ctx);
    lx_ui_message(lx, "ADB", ok ? "ADB is on. Switch it off in Mellowtone: Settings > About > Developer Options."
                                : "ADB did not start. See boot.log on the card.");
}

static void do_reset(lx_t * lx) {
    const lx_config_t * cfg = lx->cfg;
    if (!confirm(lx, "Reset settings", "Move Mellowtone's settings to the card's backup folder and start with "
                                       "defaults? The library and your music are kept.", "Reset"))
        return;
    char backup[PATH_MAX];
    struct stat st;
    bool card = cfg->sd_root && stat(cfg->sd_root, &st) == 0 && access(cfg->sd_root, W_OK) == 0;
    bool ok = card && mt_state_reset_config_at(cfg->sd_root, backup, sizeof(backup));
    if (ok) {
        lx_boot_counter_write(cfg, 0);
        lx_menu_flag_take(cfg);
    }
    char body[PATH_MAX + 80];
    if (!card) snprintf(body, sizeof(body), "No writable card: the settings live on the card.");
    else if (!ok) snprintf(body, sizeof(body), "The settings could not be moved.");
    else if (backup[0]) snprintf(body, sizeof(body), "Settings reset. The old ones are in %s.", backup);
    else snprintf(body, sizeof(body), "There were no settings to reset.");
    lx_log("reset: %s", body);
    lx_ui_message(lx, "Reset settings", body);
}

static void do_info(lx_t * lx) {
    const lx_config_t * cfg = lx->cfg;
    char cur[64], prev[64], path[PATH_MAX], card[96], body[900];
    bool has_cur = lx_slot_version(cfg, "current", cur, sizeof(cur));
    bool has_prev = lx_slot_version(cfg, "previous", prev, sizeof(prev));
    struct statvfs v;
    if (cfg->sd_root && statvfs(cfg->sd_root, &v) == 0 && lx_exists(cfg->sd_root))
        snprintf(card, sizeof(card), "%.40s, %lld MB free%s", cfg->sd_root,
                 (long long) v.f_bavail * (long long) v.f_frsize / (1024 * 1024),
                 access(cfg->sd_root, W_OK) == 0 ? "" : ", read-only");
    else snprintf(card, sizeof(card), "none");
    int battery = lx->pf->battery_percent(lx->pf->ctx);
    snprintf(body, sizeof(body),
             "Model: %.40s (%.20s)\nLauncher: %.40s\nCurrent: %s\nPrevious (card): %s\nFactory: %.120s\nBattery: %d %%\n"
             "Card: %s\nBoot attempts: %d\nLast exit: %s",
             cfg->model, cfg->board, cfg->launcher_version ? cfg->launcher_version : "?", has_cur ? cur : "none",
             has_prev ? prev : "none",
             lx_level_path(cfg, LX_LEVEL_FACTORY, path, sizeof(path)) ? path : "none", battery, card,
             lx_boot_counter_read(cfg), lx->last_exit[0] ? lx->last_exit : "none");
    lx_ui_message(lx, "Device info", body);
}

/* ---- the menu ------------------------------------------------------------------- */

typedef enum {
    IT_START, IT_PKG, IT_FW, IT_ROLLBACK, IT_FACTORY, IT_ADB, IT_RESET, IT_INFO, IT_REBOOT, IT_POWEROFF,
} item_t;

static lx_action_t menu_loop(lx_t * lx, int auto_start_ms);

lx_action_t lx_menu(lx_t * lx, int auto_start_ms) {
    const lx_platform_t * pf = lx->pf;
    if (!lx->fb && pf->display_open) lx->fb = pf->display_open(pf->ctx);
    lx_action_t a = menu_loop(lx, auto_start_ms);
    if (lx->fb && pf->display_close) pf->display_close(pf->ctx, lx->fb);
    lx->fb = NULL;
    return a;
}

static lx_action_t menu_loop(lx_t * lx, int auto_start_ms) {
    const lx_config_t * cfg = lx->cfg;
    if (!lx->fb) {
        /* No display: nothing to choose from. Keep a way in (ADB) when the
         * player keeps failing, then try again. */
        if (lx->reason[0]) {
            lx_log("menu: no display; enabling ADB and retrying in 60 s");
            lx->pf->enable_adb(lx->pf->ctx);
            lx->pf->sleep_ms(lx->pf->ctx, 60000);
        }
        return LX_ACT_START_CURRENT;
    }
    int sel = 0;
    for (;;) {
        const char * labels[16];
        item_t kinds[16];
        int n = 0;
        char pkg_label[96], fw_label[160], fw_path[PATH_MAX], tmp[PATH_MAX], factory_label[48], prev_label[96];
        lx_pkg_ref_t pkg;
        bool has_pkg = lx_pkg_find(cfg, &pkg);
        bool has_fw = lx_fw_find(cfg, fw_path, sizeof(fw_path));
#define ADD(k, l) do { kinds[n] = (k); labels[n] = (l); n++; } while (0)
        ADD(IT_START, "Start Mellowtone");
        if (has_pkg) {
            snprintf(pkg_label, sizeof(pkg_label), "Install player update %s", pkg.version);
            ADD(IT_PKG, pkg_label);
        }
        if (has_fw) {
            const char * base = strrchr(fw_path, '/');
            snprintf(fw_label, sizeof(fw_label), "Install firmware %.120s", base ? base + 1 : fw_path);
            ADD(IT_FW, fw_label);
        }
        char prev[64];
        if (lx_slots_backup_available(cfg) && lx_slot_version(cfg, "previous", prev, sizeof(prev))) {
            snprintf(prev_label, sizeof(prev_label), "Roll back to %s", prev);
            ADD(IT_ROLLBACK, prev_label);
        }
        if (lx_level_path(cfg, LX_LEVEL_FACTORY, tmp, sizeof(tmp))) {
            bool mellowtone = lx_is_executable(cfg->factory_player);
            snprintf(factory_label, sizeof(factory_label), "%s", mellowtone ? "Start factory version" : "Start HiBy player");
            ADD(IT_FACTORY, factory_label);
        }
        ADD(IT_ADB, "Enable ADB");
        ADD(IT_RESET, "Reset Mellowtone settings");
        ADD(IT_INFO, "Device info");
        ADD(IT_REBOOT, "Reboot");
        ADD(IT_POWEROFF, "Power off");
#undef ADD
        char body[400];
        snprintf(body, sizeof(body), "%s%s%s", lx->reason, lx->reason[0] && (has_pkg || has_fw) ? "\n" : "",
                 has_pkg || has_fw ? "An update is on the card." : "");
        if (sel >= n) sel = 0;
        int c = lx_ui_choose(lx, "Mellowtone recovery", body, labels, n, sel, auto_start_ms);
        auto_start_ms = 0;
        if (c == -2) return LX_ACT_START_CURRENT;
        if (c == -3) {
            lx_log("menu: idle, powering off");
            return LX_ACT_POWEROFF;
        }
        if (c < 0) continue;
        sel = c;
        switch (kinds[c]) {
        case IT_START:
            lx->fresh_try = true;
            return LX_ACT_START_CURRENT;
        case IT_PKG: do_install_package(lx, &pkg); break;
        case IT_FW: do_install_firmware(lx, fw_path); break;
        case IT_ROLLBACK:
            if (confirm(lx, "Roll back", "Make the previous version (on the card) the current one? The current "
                                         "one is saved to the card in its place.", "Roll back")) {
                bool ok = lx_slots_rollback(cfg);
                lx_update_log(cfg, "rollback: %s", ok ? "done" : "FAILED");
                if (ok) new_current(lx);
                lx_ui_message(lx, "Roll back", ok ? "Done."
                                               : lx_slots_swap_pending(cfg)
                                                   ? "Not finished: the roll back will be completed at the next start."
                                                   : "The versions could not be swapped. Is the card writable and "
                                                     "not full?");
            }
            break;
        case IT_FACTORY: return LX_ACT_START_FACTORY;
        case IT_ADB: do_enable_adb(lx); break;
        case IT_RESET: do_reset(lx); break;
        case IT_INFO: do_info(lx); break;
        case IT_REBOOT: return LX_ACT_REBOOT;
        case IT_POWEROFF: return LX_ACT_POWEROFF;
        }
    }
}
