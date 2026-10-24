#ifndef WMA_DECODER_H
#define WMA_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* WMA (v1/v2 "WMA Standard", the lossy codecs -- not WMA Pro or WMA
 * Lossless) decoding, driven by src/audio/asf_demux.c for container parsing. A
 * from-scratch reimplementation of the algorithm described by FFmpeg's
 * libavcodec/wma.c + wmadec.c (the codec itself was never officially
 * published by Microsoft; FFmpeg's decoder is the practical reference).
 * The Huffman/critical-band/LSP-codebook constants in wma_tables.h are
 * format-mandated numeric data (an encoder and decoder must agree on them
 * bit-for-bit), not creative expression, and were pulled out with a small
 * extraction script rather than hand-copied.
 *
 * Verified against a real ground-truth file: a 440Hz sine wave encoded to
 * WMAv2/128kbps with ffmpeg, decoded here and compared against ffmpeg's
 * own decode of the same file. */

#include "decoder_result.h"
#include "mt_features.h"

typedef struct wma_decoder wma_decoder_t;

#if MT_HAS_FEATURE(CODEC_WMA)
wma_decoder_t * wma_open_file(const char * path);

unsigned int wma_get_channels(const wma_decoder_t * dec);
unsigned int wma_get_sample_rate(const wma_decoder_t * dec);
uint64_t wma_get_total_pcm_frame_count(const wma_decoder_t * dec);

decoder_read_result_t wma_read_pcm_frames_s16(wma_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool wma_seek_to_pcm_frame(wma_decoder_t * dec, uint64_t frame_index);

void wma_close(wma_decoder_t * dec);
#else
/* Not built (MT_CODEC_WMA off): audio.c and the library never route a file here
 * (file_browser.c's playable list leaves the type out); inert, so the
 * decoder dispatch needs no #if. */
static inline wma_decoder_t * wma_open_file(const char * path) { (void) path; return NULL; }
static inline unsigned int wma_get_channels(const wma_decoder_t * dec) { (void) dec; return 0; }
static inline unsigned int wma_get_sample_rate(const wma_decoder_t * dec) { (void) dec; return 0; }
static inline uint64_t wma_get_total_pcm_frame_count(const wma_decoder_t * dec) { (void) dec; return 0; }
static inline decoder_read_result_t wma_read_pcm_frames_s16(wma_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    (void) dec; (void) frames_to_read; (void) buffer_out;
    return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
}
static inline bool wma_seek_to_pcm_frame(wma_decoder_t * dec, uint64_t frame_index) { (void) dec; (void) frame_index; return false; }
static inline void wma_close(wma_decoder_t * dec) { (void) dec; }
#endif

#endif /* WMA_DECODER_H */
