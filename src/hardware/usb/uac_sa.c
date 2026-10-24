/* /dev/uac_sa ABI and DSD helpers: see uac_sa.h. */
#include "uac_sa.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int real_open(const char * path, int flags) { return open(path, flags | O_CLOEXEC); }
static int real_ioctl(int fd, unsigned long request, void * arg) { return ioctl(fd, request, arg); }
static ssize_t real_read(int fd, void * buf, size_t len) { return read(fd, buf, len); }
static int real_close(int fd) { return close(fd); }
static int real_poll_in(int fd, int timeout_ms, bool * readable, bool * error) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    *readable = pr > 0 && (pfd.revents & POLLIN);
    *error = pr > 0 && (pfd.revents & (POLLERR | POLLNVAL));
    return pr;
}

static const uac_sa_io_t default_io = {
    .open = real_open, .ioctl = real_ioctl, .read = real_read, .poll_in = real_poll_in, .close = real_close,
};

const uac_sa_io_t * uac_sa_io_default(void) { return &default_io; }

bool uac_sa_parse_info(const int info[3], board_uac_sa_abi_t abi, bool dsd_ok, uac_sa_format_t * out) {
    if (abi == BOARD_UAC_SA_ABI_PREPARE_IOCTL && info[0] == UAC_SA_FORMAT_DSD) {
        /* DSD64 (88.2 kHz frames) to DSD256 (352.8/384 kHz frames). */
        if (!dsd_ok || info[1] < 88200 || info[1] > 384000) return false;
        out->kind = UAC_SA_STREAM_DSD;
        out->rate = (unsigned int) info[1];
        out->container_bits = 32;
        return true;
    }
    if (abi == BOARD_UAC_SA_ABI_PREPARE_IOCTL && info[0] != UAC_SA_FORMAT_PCM) return false;
    unsigned int rate = info[1] > 0 ? (unsigned int) info[1] : 0;
    unsigned int bits = info[2] == 16 ? 16 : (info[2] == 24 || info[2] == 32) ? 32 : 0;
    if (rate < 32000 || rate > 384000 || bits == 0) return false;
    out->kind = UAC_SA_STREAM_PCM;
    out->rate = rate;
    out->container_bits = bits;
    return true;
}

int uac_sa_prepare(const uac_sa_io_t * io, int fd, board_uac_sa_abi_t abi) {
    if (abi != BOARD_UAC_SA_ABI_PREPARE_IOCTL) return 1;
    int armed = 0;
    if (io->ioctl(fd, UAC_SA_IOC_PREPARE, &armed) != 0) return -1;
    return armed > 0 ? 1 : armed < 0 ? -1 : 0;
}

static uint32_t load_u32(const uint8_t * p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

bool uac_sa_is_dop(const uint8_t * data, size_t frames) {
    if (frames < 16) return false;
    uint8_t expect = 0;
    for (size_t i = 0; i < frames; i++) {
        uint8_t l = (uint8_t) (load_u32(data + i * 8) >> 24);
        uint8_t r = (uint8_t) (load_u32(data + i * 8 + 4) >> 24);
        if (l != r || (l != UAC_SA_DOP_MARKER_A && l != UAC_SA_DOP_MARKER_B)) return false;
        if (i > 0 && l != expect) return false;
        expect = l == UAC_SA_DOP_MARKER_A ? UAC_SA_DOP_MARKER_B : UAC_SA_DOP_MARKER_A;
    }
    return true;
}

static int32_t dop_word(uint8_t marker, uint8_t first, uint8_t second) {
    uint32_t word = (uint32_t) marker << 16 | (uint32_t) first << 8 | second;
    return (int32_t) (word << 8) >> 8; /* the S24 layout dsd_decoder.c's DoP uses */
}

size_t uac_sa_dsd_to_dop(const uint8_t * in, size_t frames, int32_t * out, uint8_t * marker) {
    uint8_t m = *marker == UAC_SA_DOP_MARKER_B ? UAC_SA_DOP_MARKER_B : UAC_SA_DOP_MARKER_A;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t * l = in + i * 8;
        const uint8_t * r = l + 4;
        for (int half = 0; half < 2; half++) {
            int32_t * o = out + (i * 2 + (size_t) half) * 2;
            o[0] = dop_word(m, l[half * 2], l[half * 2 + 1]);
            o[1] = dop_word(m, r[half * 2], r[half * 2 + 1]);
            m = m == UAC_SA_DOP_MARKER_A ? UAC_SA_DOP_MARKER_B : UAC_SA_DOP_MARKER_A;
        }
    }
    *marker = m;
    return frames * 2;
}

void uac_sa_dop_passthrough(const uint8_t * in, size_t frames, int32_t * out) {
    for (size_t i = 0; i < frames * 2; i++) out[i] = (int32_t) load_u32(in + i * 4) >> 8;
}

const char * uac_sa_dsd_name(unsigned int native_rate) {
    switch (native_rate) {
        case 88200: case 96000: return "DSD64";
        case 176400: case 192000: return "DSD128";
        case 352800: case 384000: return "DSD256";
    }
    return "DSD";
}
