/* The USB DAC bridge (usb_dac_bridge.c, built as on the device) against a
 * fake /dev/uac_sa: the driver calls go through usb_dac_bridge_test_setup()'s
 * io table, the data through a pipe, and the output is a recording
 * audio_output. Covers both ABIs (READ_EPERM: format at open, EPERM until
 * armed; PREPARE_IOCTL: nothing read before ioctl 0 says armed), PCM in
 * 32-bit containers to S24, a rate change, DoP passed through untouched and
 * native DSD repacked as DoP (only where the board has CAP_USB_DAC_DSD). */
#include "audio.h"
#include "audio_output.h"
#include "board.h"
#include "db_log.h"
#include "storage_paths.h"
#include "uac_sa.h"
#include "usb_dac_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

const board_desc_t BOARD = { .name = "test" };

/* ---- stubs: the player and the storage layer ------------------------------ */

void audio_stop(void) {}
bool audio_is_playing(void) { return false; }
bool audio_is_paused(void) { return false; }
bool db_log_enabled(void) { return false; }
bool mt_state_mkdirs(const char * dir) {
    (void) dir;
    return false;
}

/* ---- a recording audio_output ---------------------------------------------- */

#define REC_MAX (1 << 20)
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static int32_t rec[REC_MAX]; /* interleaved samples written with write_s24 */
static size_t rec_count;
static unsigned int open_rate;
static bool open_dop;
static int opens;
static bool refuse_dop;

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate, bool low_latency, bool want_s24) {
    (void) channels;
    (void) low_latency;
    (void) want_s24;
    pthread_mutex_lock(&rec_mutex);
    if (open_rate != sample_rate || open_dop) opens++;
    open_rate = sample_rate;
    open_dop = false;
    pthread_mutex_unlock(&rec_mutex);
    return true;
}

bool audio_output_ensure_dop(unsigned int channels, unsigned int sample_rate) {
    (void) channels;
    if (refuse_dop) return false;
    pthread_mutex_lock(&rec_mutex);
    if (open_rate != sample_rate || !open_dop) opens++;
    open_rate = sample_rate;
    open_dop = true;
    pthread_mutex_unlock(&rec_mutex);
    return true;
}

bool audio_output_dop_supported(void) { return true; }
bool audio_output_is_s24_active(void) { return true; }
bool audio_output_is_dop_active(void) { return open_dop; }
bool audio_output_is_bt_active(void) { return false; }
bool audio_output_is_usb_active(void) { return false; }
void audio_output_close(void) {}
void audio_output_set_usb_requested(bool requested, const char * alsa_device) {
    (void) requested;
    (void) alsa_device;
}
void audio_output_set_bt_requested(bool requested) { (void) requested; }

bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * out) {
    (void) buf;
    (void) channels;
    if (out) *out = frames;
    return true;
}

bool audio_output_write_s24(const int32_t * buf, uint64_t frames, unsigned int channels, uint64_t * out) {
    pthread_mutex_lock(&rec_mutex);
    size_t n = (size_t) frames * channels;
    if (rec_count + n <= REC_MAX) {
        memcpy(rec + rec_count, buf, n * sizeof(int32_t));
        rec_count += n;
    }
    pthread_mutex_unlock(&rec_mutex);
    if (out) *out = frames;
    usleep(200); /* a little pacing, like a device */
    return true;
}

static void rec_reset(void) {
    pthread_mutex_lock(&rec_mutex);
    rec_count = 0;
    opens = 0;
    open_rate = 0;
    open_dop = false;
    pthread_mutex_unlock(&rec_mutex);
}

static size_t rec_size(void) {
    pthread_mutex_lock(&rec_mutex);
    size_t n = rec_count;
    pthread_mutex_unlock(&rec_mutex);
    return n;
}

/* ---- the fake /dev/uac_sa --------------------------------------------------- */

static int pipe_rd = -1, pipe_wr = -1;
static atomic_int fmt_format, fmt_rate, fmt_bits;
static atomic_int armed;          /* PREPARE_IOCTL: the answer to ioctl 0 */
static atomic_int prepare_calls, info_calls;
static atomic_bool eperm;         /* READ_EPERM: read() fails with EPERM */

static int fake_open(const char * path, int flags) {
    (void) flags;
    if (strcmp(path, UAC_SA_DEVICE_PATH) != 0) {
        errno = ENOENT;
        return -1;
    }
    return dup(pipe_rd);
}

static int fake_ioctl(int fd, unsigned long request, void * arg) {
    (void) fd;
    if (request == UAC_SA_IOC_PREPARE) {
        atomic_fetch_add(&prepare_calls, 1);
        *(int *) arg = atomic_load(&armed);
        return 0;
    }
    if (request == UAC_SA_IOC_GET_INFO) {
        atomic_fetch_add(&info_calls, 1);
        int * info = arg;
        info[0] = atomic_load(&fmt_format);
        info[1] = atomic_load(&fmt_rate);
        info[2] = atomic_load(&fmt_bits);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static ssize_t fake_read(int fd, void * buf, size_t len) {
    if (atomic_load(&eperm)) {
        errno = EPERM;
        return -1;
    }
    ssize_t n = read(fd, buf, len);
    if (n < 0 && errno == EAGAIN) return 0; /* the driver answers 0 when nothing is queued */
    return n;
}

static int fake_poll(int fd, int timeout_ms, bool * readable, bool * error) {
    return uac_sa_io_default()->poll_in(fd, timeout_ms, readable, error);
}

static int fake_close(int fd) { return close(fd); }

static const uac_sa_io_t fake_io = {
    .open = fake_open, .ioctl = fake_ioctl, .read = fake_read, .poll_in = fake_poll, .close = fake_close,
};

static void put32(uint8_t * p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static void feed(const uint8_t * data, size_t len) {
    while (len > 0) {
        ssize_t n = write(pipe_wr, data, len);
        if (n < 0 && errno == EAGAIN) {
            usleep(1000);
            continue;
        }
        if (n <= 0) return;
        data += n;
        len -= (size_t) n;
    }
}

/* PCM ramp in 32-bit containers: sample i (both channels) = i << 8 (L) and
 * -(i << 8) (R), MSB-aligned as the driver delivers it. */
static uint32_t ramp_next;
static void feed_pcm(size_t frames) {
    uint8_t * buf = malloc(frames * 8);
    for (size_t i = 0; i < frames; i++, ramp_next++) {
        put32(buf + i * 8, (ramp_next & 0xffffff) << 8);
        put32(buf + i * 8 + 4, (uint32_t) -(int32_t) ((ramp_next & 0xffffff) << 8));
    }
    feed(buf, frames * 8);
    free(buf);
}

static void sleep_ms(int ms) { usleep((useconds_t) ms * 1000); }

static bool wait_for(bool (*cond)(void), int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) {
        if (cond()) return true;
        sleep_ms(10);
    }
    return cond();
}

static size_t want_samples;
static bool enough(void) { return rec_size() >= want_samples; }
static bool streaming(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    return info.streaming;
}

static void new_pipe(void) {
    if (pipe_rd >= 0) close(pipe_rd);
    if (pipe_wr >= 0) close(pipe_wr);
    int fds[2];
    if (pipe(fds) != 0) exit(2);
    pipe_rd = fds[0];
    pipe_wr = fds[1];
    fcntl(pipe_rd, F_SETFL, O_NONBLOCK);
    fcntl(pipe_wr, F_SETFL, O_NONBLOCK);
}

/* Output samples are a contiguous stretch of the ramp, right-justified
 * (after the first 10 ms fade-in). */
static bool ramp_contiguous(size_t from) {
    pthread_mutex_lock(&rec_mutex);
    bool ok = rec_count >= from + 4;
    for (size_t i = from; ok && i + 3 < rec_count; i += 2) {
        int32_t l = rec[i], l2 = rec[i + 2];
        if (l2 != l + 1 || rec[i + 1] != -l) ok = false;
    }
    pthread_mutex_unlock(&rec_mutex);
    return ok;
}

static board_desc_t x1600 = { .name = "x1600" };
static board_desc_t r3pro = { .name = "r3pro" };

static void test_read_eperm_pcm(void) {
    new_pipe();
    rec_reset();
    usb_dac_bridge_test_setup(&x1600, &fake_io);
    atomic_store(&fmt_format, 1);
    atomic_store(&fmt_rate, 48000);
    atomic_store(&fmt_bits, 32);
    atomic_store(&eperm, true);
    atomic_store(&info_calls, 0);
    usb_dac_bridge_start();
    sleep_ms(300);
    CHECK(atomic_load(&info_calls) > 0, "READ_EPERM reads the format at open");
    CHECK(!streaming(), "EPERM: not streaming");
    atomic_store(&eperm, false);
    ramp_next = 0;
    feed_pcm(9600); /* 200 ms; what queued up before the output opened is trimmed to 40 ms */
    want_samples = 2 * 2400;
    CHECK(wait_for(enough, 3000), "PCM reached the output (%zu)", rec_size());
    CHECK(open_rate == 48000 && !open_dop, "opened PCM at 48 kHz (%u, dop %d)", open_rate, open_dop);
    CHECK(ramp_contiguous(2 * 480), "PCM 32-bit -> S24 untouched after the fade");

    /* The host changes rate: output reopened at the new rate. */
    atomic_store(&fmt_rate, 96000);
    sleep_ms(300);
    rec_reset();
    feed_pcm(19200);
    want_samples = 2 * 8000;
    CHECK(wait_for(enough, 3000), "after the rate change (%zu)", rec_size());
    CHECK(open_rate == 96000, "reopened at 96 kHz (%u)", open_rate);

    /* DoP over PCM is not touched on a board without DSD. */
    atomic_store(&fmt_rate, 176400);
    sleep_ms(300);
    rec_reset();
    uint8_t buf[4096 * 8];
    for (int i = 0; i < 4096; i++) {
        uint32_t m = (i % 2) ? 0xFA : 0x05;
        put32(buf + i * 8, m << 24 | 0x6969u << 8);
        put32(buf + i * 8 + 4, m << 24 | 0x6969u << 8);
    }
    feed(buf, sizeof(buf));
    want_samples = 2 * 2000;
    CHECK(wait_for(enough, 3000), "DoP-looking PCM played");
    CHECK(!open_dop, "no DSD capability: plays as PCM");
    usb_dac_bridge_stop();
}

static void test_prepare_ioctl(void) {
    new_pipe();
    rec_reset();
    r3pro.caps = CAP_USB_DAC | CAP_USB_DAC_DSD;
    r3pro.usb.uac_abi = BOARD_UAC_SA_ABI_PREPARE_IOCTL;
    usb_dac_bridge_test_setup(&r3pro, &fake_io);
    atomic_store(&eperm, false);
    atomic_store(&armed, 0);
    atomic_store(&prepare_calls, 0);
    atomic_store(&info_calls, 0);
    atomic_store(&fmt_format, 1);
    atomic_store(&fmt_rate, 44100);
    atomic_store(&fmt_bits, 32);
    ramp_next = 0;
    feed_pcm(4410);
    usb_dac_bridge_start();
    sleep_ms(500);
    CHECK(atomic_load(&prepare_calls) >= 2, "polls prepare while unarmed (%d)", atomic_load(&prepare_calls));
    CHECK(atomic_load(&info_calls) == 0, "no GET_INFO before armed");
    CHECK(rec_size() == 0, "nothing read before armed");

    atomic_store(&armed, 1);
    want_samples = 2 * 1500;
    CHECK(wait_for(enough, 3000), "armed: PCM played (%zu)", rec_size());
    CHECK(open_rate == 44100 && !open_dop, "44.1 kHz PCM (%u)", open_rate);

    /* DoP at 176.4 kHz on a DSD board: through the DoP switch, untouched. */
    atomic_store(&fmt_rate, 176400);
    sleep_ms(300);
    rec_reset();
    uint8_t buf[4096 * 8];
    for (int i = 0; i < 4096; i++) {
        uint32_t m = (i % 2) ? 0xFA : 0x05;
        put32(buf + i * 8, m << 24 | (uint32_t) (i & 0xffff) << 8);
        put32(buf + i * 8 + 4, m << 24 | (uint32_t) (~i & 0xffff) << 8);
    }
    feed(buf, sizeof(buf));
    want_samples = 2 * 3000;
    CHECK(wait_for(enough, 3000), "DoP played (%zu)", rec_size());
    CHECK(open_dop && open_rate == 176400, "DoP output at 176.4 kHz (%u, dop %d)", open_rate, open_dop);
    pthread_mutex_lock(&rec_mutex);
    bool markers = rec_count > 8;
    for (size_t i = 2; markers && i + 1 < rec_count; i += 2) {
        uint32_t m = (uint32_t) rec[i] >> 16 & 0xff, prev = (uint32_t) rec[i - 2] >> 16 & 0xff;
        if ((m != 0x05 && m != 0xFA) || m == prev || ((uint32_t) rec[i + 1] >> 16 & 0xff) != m) markers = false;
    }
    /* Bit-exact: each frame is the fed frame (marker, i, ~i) shifted to 24
     * bits, frames consecutive; nothing resampled, faded or cut. */
    bool exact = rec_count > 8;
    uint32_t first = (uint32_t) rec[0] & 0xffff;
    for (size_t k = 0; exact && 2 * k + 1 < rec_count; k++) {
        uint32_t i = (first + (uint32_t) k) & 0xffff;
        int32_t m = i % 2 ? (int8_t) 0xFA : 0x05; /* right-justified, so sign-extended */
        if (rec[2 * k] != (int32_t) ((uint32_t) m << 16 | i) ||
            rec[2 * k + 1] != (int32_t) ((uint32_t) m << 16 | (~i & 0xffff)))
            exact = false;
    }
    pthread_mutex_unlock(&rec_mutex);
    CHECK(markers, "DoP markers intact and alternating");
    CHECK(exact, "DoP passed bit-exact");

    /* Native DSD64: format 2, repacked to DoP at twice the frame rate. */
    atomic_store(&fmt_format, 2);
    atomic_store(&fmt_rate, 88200);
    atomic_store(&fmt_bits, 1);
    sleep_ms(300);
    rec_reset();
    for (int i = 0; i < 4096; i++) {
        uint8_t * f = buf + i * 8;
        for (int k = 0; k < 4; k++) {
            f[k] = (uint8_t) (i * 4 + k);       /* L bytes in time order */
            f[4 + k] = (uint8_t) ~(i * 4 + k);  /* R */
        }
    }
    feed(buf, sizeof(buf));
    want_samples = 2 * 4000;
    CHECK(wait_for(enough, 3000), "DSD played (%zu)", rec_size());
    CHECK(open_dop && open_rate == 176400, "DSD64 as DoP at 176.4 kHz (%u)", open_rate);
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    CHECK(info.input_dsd && info.output_kind == USB_DAC_STREAM_DSD && info.input_sample_rate == 88200,
          "stream info: native DSD");
    pthread_mutex_lock(&rec_mutex);
    bool ok = rec_count > 16;
    for (size_t i = 2; ok && i + 1 < rec_count; i += 2) {
        uint32_t l = (uint32_t) rec[i] & 0xffffff, pl = (uint32_t) rec[i - 2] & 0xffffff;
        uint32_t r = (uint32_t) rec[i + 1] & 0xffffff;
        uint8_t m = (uint8_t) (l >> 16), pm = (uint8_t) (pl >> 16);
        uint8_t a = (uint8_t) (l >> 8), b = (uint8_t) l, pb = (uint8_t) pl;
        /* markers alternate; bytes continue the sequence; R is ~L */
        if ((m != 0x05 && m != 0xFA) || m == pm || (uint8_t) (pb + 1) != a || (uint8_t) (a + 1) != b ||
            (uint8_t) (r >> 8) != (uint8_t) ~a || (uint8_t) r != (uint8_t) ~b)
            ok = false;
    }
    pthread_mutex_unlock(&rec_mutex);
    CHECK(ok, "DSD bytes in time order under alternating markers");

    /* The DAC refuses the rate: dropped, reported, nothing played. */
    refuse_dop = true;
    atomic_store(&fmt_rate, 176400);
    sleep_ms(300);
    rec_reset();
    feed(buf, sizeof(buf));
    sleep_ms(500);
    usb_dac_bridge_get_stream_info(&info);
    CHECK(info.output_unsupported && rec_size() == 0, "refused DSD rate: nothing played (%zu)", rec_size());
    refuse_dop = false;

    /* The host stops: read() fails, the bridge prepares again. */
    atomic_store(&fmt_format, 1);
    atomic_store(&fmt_rate, 48000);
    atomic_store(&fmt_bits, 32);
    int before = atomic_load(&prepare_calls);
    atomic_store(&eperm, true); /* any read error */
    feed_pcm(480);              /* something to read, so read() is called and fails */
    sleep_ms(100);
    atomic_store(&eperm, false);
    sleep_ms(300);
    CHECK(atomic_load(&prepare_calls) > before, "re-prepared after a failing read");
    rec_reset();
    feed_pcm(9600);
    want_samples = 2 * 2000;
    CHECK(wait_for(enough, 3000), "streams again (%zu)", rec_size());
    CHECK(open_rate == 48000 && !open_dop, "back to PCM 48 kHz (%u)", open_rate);
    usb_dac_bridge_stop();
}

int main(void) {
    x1600.caps = CAP_USB_DAC;
    x1600.usb.uac_abi = BOARD_UAC_SA_ABI_READ_EPERM;
    test_read_eperm_pcm();
    test_prepare_ioctl();
    usb_dac_bridge_test_setup(NULL, NULL);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("uac_bridge: all tests passed\n");
    return 0;
}
