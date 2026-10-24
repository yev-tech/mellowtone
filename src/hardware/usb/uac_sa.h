#ifndef UAC_SA_H
#define UAC_SA_H

/* /dev/uac_sa: HiBy's f_uac_sa USB audio gadget driver, which the USB DAC
 * bridge (usb_dac_bridge.c) reads the host's stream from.
 *
 * Every kernel answers ioctl UAC_SA_IOC_GET_INFO with int[3] {format, rate,
 * bits}; samples arrive as interleaved stereo 32-bit containers, MSB-aligned.
 * How "the host has not started yet" is reported differs per kernel and
 * comes from the descriptor (BOARD.usb.uac_abi):
 *
 *   READ_EPERM      read() fails with EPERM (X1600; format is always 1)
 *   PREPARE_IOCTL   ioctl UAC_SA_IOC_PREPARE: > 0 armed, 0 not yet, < 0
 *                   error; a failing read() means the host stopped, and the
 *                   stream is prepared again (kernel 3.10, R3 Pro). Format 2
 *                   is native DSD: each frame is one 32-bit word of left DSD
 *                   bytes then one of right, bytes in time order, and the
 *                   rate is the DSD bit rate / 32.
 *
 * Native DSD and DoP both reach the DAC as DoP (audio_output_ensure_dop()):
 * 16 DSD bits per channel per frame under an alternating 0x05/0xFA marker.
 *
 * Everything here is pure except the io table, which tests replace. */

#include "board.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define UAC_SA_DEVICE_PATH "/dev/uac_sa"
#define UAC_SA_IOC_PREPARE 0  /* int *: PREPARE_IOCTL ABI only */
#define UAC_SA_IOC_GET_INFO 1 /* int[3] {format, rate, bits} */

#define UAC_SA_FORMAT_PCM 1
#define UAC_SA_FORMAT_DSD 2

#define UAC_SA_DOP_MARKER_A 0x05
#define UAC_SA_DOP_MARKER_B 0xFA

/* The driver calls; NULL members are not allowed. */
typedef struct uac_sa_io {
    int (*open)(const char * path, int flags);
    int (*ioctl)(int fd, unsigned long request, void * arg);
    ssize_t (*read)(int fd, void * buf, size_t len);
    /* poll(2) for POLLIN: > 0 with *readable (POLLIN) and *error (POLLERR or
     * POLLNVAL) set, 0 timeout, < 0 failure (errno). */
    int (*poll_in)(int fd, int timeout_ms, bool * readable, bool * error);
    int (*close)(int fd);
} uac_sa_io_t;

/* The real system calls. */
const uac_sa_io_t * uac_sa_io_default(void);

typedef enum {
    UAC_SA_STREAM_PCM = 0, /* PCM in 16- or 32-bit containers */
    UAC_SA_STREAM_DSD,     /* native DSD (format 2), played as DoP */
} uac_sa_stream_kind_t;

typedef struct {
    uac_sa_stream_kind_t kind;
    unsigned int rate;           /* PCM: frames/s. DSD: native 8-byte frames/s (bit rate / 32) */
    unsigned int container_bits; /* bytes per sample * 8 in the stream: 16 or 32 (DSD: 32) */
} uac_sa_format_t;

/* Validates a GET_INFO answer. PCM: 32-384 kHz, bits 16 or 24/32 (24 is a
 * 32-bit container, the only layout the driver has been seen to deliver).
 * DSD (format 2, PREPARE_IOCTL ABI and dsd_ok only): DSD64-DSD256, i.e. a
 * rate field of 88.2-384 kHz. The READ_EPERM ABI ignores the format field,
 * as the X1600 bridge always did. False: keep whatever was in effect. */
bool uac_sa_parse_info(const int info[3], board_uac_sa_abi_t abi, bool dsd_ok, uac_sa_format_t * out);

/* PREPARE_IOCTL: > 0 armed, 0 not yet, < 0 error (ioctl failure or a
 * negative answer). Always 1 for READ_EPERM (nothing to ask). */
int uac_sa_prepare(const uac_sa_io_t * io, int fd, board_uac_sa_abi_t abi);

/* True when frames (interleaved stereo, 32-bit MSB-aligned containers)
 * carry DoP: every frame's top bytes are a marker, the same on both
 * channels, alternating. At least 16 frames are needed to decide. */
bool uac_sa_is_dop(const uint8_t * data, size_t frames);

/* Native DSD frames -> DoP frames (two per native frame, 24-bit
 * right-justified in int32, interleaved stereo). *marker carries the
 * alternation across calls (0 to start). Returns the DoP frames written:
 * 2 * frames. */
size_t uac_sa_dsd_to_dop(const uint8_t * in, size_t frames, int32_t * out, uint8_t * marker);

/* DoP pass-through: 32-bit MSB-aligned DoP samples -> 24-bit right-justified,
 * bits untouched. */
void uac_sa_dop_passthrough(const uint8_t * in, size_t frames, int32_t * out);

/* "DSD64" ... for a native DSD rate field (bit rate / 32), "DSD" if unusual. */
const char * uac_sa_dsd_name(unsigned int native_rate);

#endif /* UAC_SA_H */
