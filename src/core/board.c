#include "board.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Plugin tokens, in catalogue order (bit i = entry i). */
static const char * const cap_names[BOARD_CAP_COUNT] = {
    "hw.audio_dac",
    "hw.gain_modes",
    "hw.bt_audio_out",
    "hw.bt_receiver",
    "hw.wifi",
    "hw.wifi_5ghz",
    "hw.usb_storage",
    "hw.usb_dac",
    "hw.usb_dac_dsd",
    "hw.usb_host_audio",
    "hw.charge_control",
    "hw.input_current",
    "hw.battery_current",
    "hw.leds",
    "hw.inline_remote",
    "hw.touch",
    "hw.keys",
};

_Static_assert(CAP_KEYS == 1u << (BOARD_CAP_COUNT - 1), "BOARD_CAP_COUNT out of step with board_cap_t");

bool board_has(board_cap_t cap) {
    return cap != 0 && (BOARD.caps & (uint32_t) cap) == (uint32_t) cap;
}

/* The temporary guard for listed but unimplemented caps (board.h). The gaps
 * themselves are listed per board in boards/<name>/board.c
 * (.caps_impl_missing); when the last one is gone, this function goes too. */
bool board_impl_missing(board_cap_t cap) {
    return board_has(cap) && (BOARD.caps_impl_missing & (uint32_t) cap) != 0;
}

board_cap_t board_cap_at(size_t i) {
    return i < BOARD_CAP_COUNT ? (board_cap_t) (1u << i) : (board_cap_t) 0;
}

const char * board_cap_name(board_cap_t cap) {
    for (size_t i = 0; i < BOARD_CAP_COUNT; i++)
        if ((uint32_t) cap == 1u << i) return cap_names[i];
    return NULL;
}

bool board_cap_from_name(const char * name, board_cap_t * out) {
    if (!name) return false;
    for (size_t i = 0; i < BOARD_CAP_COUNT; i++) {
        if (strcmp(name, cap_names[i]) == 0) {
            if (out) *out = (board_cap_t) (1u << i);
            return true;
        }
    }
    return false;
}

/* ---- install layout ----------------------------------------------------- */

static bool image_install;
static pthread_once_t image_once = PTHREAD_ONCE_INIT;

static void image_probe(void) {
    char factory[256];
    struct stat st;
    image_install = BOARD.paths.image_root && BOARD.paths.install_root &&
                    snprintf(factory, sizeof(factory), "%s/factory", BOARD.paths.image_root) < (int) sizeof(factory) &&
                    stat(factory, &st) == 0 && S_ISDIR(st.st_mode);
}

bool board_image_install(void) {
    pthread_once(&image_once, image_probe);
    return image_install;
}

const char * board_install_root(void) {
    return board_image_install() ? BOARD.paths.image_root : BOARD.paths.install_root;
}

/* Descriptor paths are a handful of constants: each one is mapped once and
 * kept, keyed by the descriptor's pointer. */
static pthread_mutex_t asset_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    const char * in;
    char out[256];
} asset_map[16];

const char * board_asset_path(const char * path) {
    if (!path || !board_image_install()) return path;
    const char * root = BOARD.paths.install_root;
    size_t n = strlen(root);
    if (strncmp(path, root, n) != 0 || (path[n] != '/' && path[n] != '\0')) return path;
    const char * out = path;
    pthread_mutex_lock(&asset_lock);
    for (size_t i = 0; i < sizeof(asset_map) / sizeof(asset_map[0]); i++) {
        if (asset_map[i].in == path) {
            out = asset_map[i].out;
            break;
        }
        if (!asset_map[i].in) {
            if (snprintf(asset_map[i].out, sizeof(asset_map[i].out), "%s%s", BOARD.paths.image_root, path + n) <
                (int) sizeof(asset_map[i].out)) {
                asset_map[i].in = path;
                out = asset_map[i].out;
            }
            break;
        }
    }
    pthread_mutex_unlock(&asset_lock);
    return out;
}

static char curve_buf[256];
static const char * curve_path;
static pthread_once_t curve_once = PTHREAD_ONCE_INIT;

static void curve_probe(void) {
    curve_path = BOARD.alsa.volume_curve;
    if (curve_path && access(curve_path, R_OK) != 0 && BOARD.alsa.volume_curve_select)
        curve_path = BOARD.alsa.volume_curve_select(curve_buf, sizeof(curve_buf));
}

const char * board_volume_curve(void) {
    pthread_once(&curve_once, curve_probe);
    return curve_path;
}
