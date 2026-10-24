#include "r3pro_stock_ini.h"
#include "board.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* BOARD.paths.stock_gain_ini: /data/user.ini on the R3 Pro; NULL elsewhere,
 * where both functions do nothing. */
#define STOCK_INI (BOARD.paths.stock_gain_ini)
#define STOCK_INI_SIZE 0x738
#define STOCK_INI_MAGIC 0xFFu
#define STOCK_INI_VERSION 0x0133F022u
#define STOCK_GAIN_OFFSET 0x24C

static uint32_t le32(const uint8_t * p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

/* Whole file, only when it is the layout the offsets were taken from. */
static bool load(uint8_t buf[STOCK_INI_SIZE]) {
    if (!STOCK_INI) return false;
    FILE * f = fopen(STOCK_INI, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, STOCK_INI_SIZE, f);
    int extra = fgetc(f);
    fclose(f);
    return n == STOCK_INI_SIZE && extra == EOF && le32(buf) == STOCK_INI_MAGIC && le32(buf + 4) == STOCK_INI_VERSION;
}

bool r3pro_stock_gain_read(int current, int * out_level) {
    uint8_t buf[STOCK_INI_SIZE];
    if (!out_level || !load(buf)) return false;
    bool high = le32(buf + STOCK_GAIN_OFFSET) != 0; /* stock treats any non-zero as High */
    *out_level = high ? 0 : (current == 2 ? 2 : 1);
    return true;
}

bool r3pro_stock_gain_write(int level) {
    uint8_t buf[STOCK_INI_SIZE];
    if (!load(buf)) return false;
    uint32_t v = level == 0 ? 1 : 0;
    if (le32(buf + STOCK_GAIN_OFFSET) == v) return true;
    for (int i = 0; i < 4; i++) buf[STOCK_GAIN_OFFSET + i] = (uint8_t) (v >> (8 * i));
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", STOCK_INI);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, buf, STOCK_INI_SIZE) == STOCK_INI_SIZE && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(tmp, STOCK_INI) == 0;
    if (!ok) unlink(tmp);
    return ok;
}
