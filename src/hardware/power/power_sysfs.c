#include "power_sysfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const board_desc_t * test_board;
static const char * test_root;

const board_desc_t * power_board(void) { return test_board ? test_board : &BOARD; }
const char * power_sysfs_root(void) { return test_root ? test_root : "/sys"; }

void power_test_setup(const board_desc_t * board, const char * sysfs_root) {
    test_board = board;
    test_root = sysfs_root;
}

/* dirent names can be up to 255 bytes: leave room for the root, the class
 * prefix and the attribute instead of truncating into another path. */
#define POWER_PATH_MAX 640

static bool full_path(const char * rel, char * out, size_t size) {
    int n = snprintf(out, size, "%s/%s", power_sysfs_root(), rel);
    return n > 0 && (size_t) n < size;
}

bool power_sysfs_read(const char * rel, char * out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    char path[POWER_PATH_MAX];
    if (!full_path(rel, path, sizeof(path))) return false;
    FILE * f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int) out_size, f) != NULL;
    fclose(f);
    if (!ok) {
        out[0] = '\0';
        return false;
    }
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return len > 0;
}

bool power_sysfs_read_int(const char * rel, int * out) {
    char buf[32];
    if (!power_sysfs_read(rel, buf, sizeof(buf))) return false;
    char * end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (end == buf || errno != 0) return false;
    if (out) *out = (int) v;
    return true;
}

bool power_sysfs_write_str(const char * rel, const char * value) {
    char path[POWER_PATH_MAX];
    if (!full_path(rel, path, sizeof(path))) return false;
    FILE * f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", value) > 0;
    if (fclose(f) != 0) ok = false; /* sysfs store errors surface at close */
    return ok;
}

bool power_sysfs_write_int(const char * rel, int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return power_sysfs_write_str(rel, buf);
}

bool power_sysfs_write_int_if_changed(const char * rel, int value) {
    int current;
    if (power_sysfs_read_int(rel, &current) && current == value) return true;
    return power_sysfs_write_int(rel, value);
}

bool power_sysfs_writable(const char * rel) {
    char path[POWER_PATH_MAX];
    return full_path(rel, path, sizeof(path)) && access(path, W_OK) == 0;
}

bool power_supply_rel(const char * node, const char * attr, char * out, size_t out_size) {
    if (!node || !node[0] || !attr) return false;
    int n = snprintf(out, out_size, "class/power_supply/%s/%s", node, attr);
    return n > 0 && (size_t) n < out_size;
}

bool power_supply_read(const char * node, const char * attr, char * out, size_t out_size) {
    char rel[POWER_PATH_MAX];
    if (!power_supply_rel(node, attr, rel, sizeof(rel))) {
        if (out && out_size) out[0] = '\0';
        return false;
    }
    return power_sysfs_read(rel, out, out_size);
}

bool power_supply_read_int(const char * node, const char * attr, int * out) {
    char rel[POWER_PATH_MAX];
    return power_supply_rel(node, attr, rel, sizeof(rel)) && power_sysfs_read_int(rel, out);
}

bool power_supply_writable(const char * node, const char * attr) {
    char rel[POWER_PATH_MAX];
    return power_supply_rel(node, attr, rel, sizeof(rel)) && power_sysfs_writable(rel);
}
