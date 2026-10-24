#ifndef OPUS_DECODER_H
#define OPUS_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* Ogg-Opus (.opus) decoding via libopus (github.com/xiph/opus, BSD-3-Clause)
 * for the SILK+CELT audio decode, and ogg_demux.h (hand-written in-tree,
 * not vendored libogg) for the Ogg container framing -- see ogg_demux.h for
 * why. Named opus_decoder_wrap_t (not opus_decoder_t) to avoid clashing
 * with libopus's own OpusDecoder type name. */

#include "decoder_result.h"
#include "mt_features.h"

typedef struct opus_decoder_wrap opus_decoder_wrap_t;

#if MT_HAS_FEATURE(CODEC_OPUS)
opus_decoder_wrap_t * opus_open_file(const char * path);

unsigned int opus_get_channels(const opus_decoder_wrap_t * dec);
unsigned int opus_get_sample_rate(const opus_decoder_wrap_t * dec); /* always 48000 -- Ogg Opus's PCM is fixed at libopus's internal rate, unlike dr_flac/dr_wav which preserve a real native rate */
uint64_t     opus_get_total_pcm_frame_count(const opus_decoder_wrap_t * dec); /* already excludes the pre-skip samples trimmed from decode start */

decoder_read_result_t opus_read_pcm_frames_s16(opus_decoder_wrap_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool     opus_seek_to_pcm_frame(opus_decoder_wrap_t * dec, uint64_t frame_index);

void opus_close(opus_decoder_wrap_t * dec);
#else
/* Not built (MT_CODEC_OPUS off): audio.c and the library never route a file here
 * (file_browser.c's playable list leaves the type out); inert, so the
 * decoder dispatch needs no #if. */
static inline opus_decoder_wrap_t * opus_open_file(const char * path) { (void) path; return NULL; }
static inline unsigned int opus_get_channels(const opus_decoder_wrap_t * dec) { (void) dec; return 0; }
static inline unsigned int opus_get_sample_rate(const opus_decoder_wrap_t * dec) { (void) dec; return 0; }
static inline uint64_t opus_get_total_pcm_frame_count(const opus_decoder_wrap_t * dec) { (void) dec; return 0; }
static inline decoder_read_result_t opus_read_pcm_frames_s16(opus_decoder_wrap_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    (void) dec; (void) frames_to_read; (void) buffer_out;
    return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
}
static inline bool opus_seek_to_pcm_frame(opus_decoder_wrap_t * dec, uint64_t frame_index) { (void) dec; (void) frame_index; return false; }
static inline void opus_close(opus_decoder_wrap_t * dec) { (void) dec; }
#endif

#endif /* OPUS_DECODER_H */
