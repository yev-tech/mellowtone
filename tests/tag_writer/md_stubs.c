/* Host-only glue so the app's real reader (src/library/metadata.c) can be
 * linked into the tag_writer test tool for FLAC/MP3 cross-checks. Only the
 * FLAC (dr_flac) and MP3 paths are exercised; everything else is stubbed. */
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "artwork_coordinator.h"
#include "ogg_demux.h"
#include "stb_vorbis.h"
#include "mbedtls/base64.h"

size_t system_get_mem_available_bytes(void) { return (size_t) 512 * 1024 * 1024; }
bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes) {
    (void) prio; (void) estimated_bytes;
    return true;
}
size_t artwork_reserve_bytes_for_priority(artwork_priority_t prio) { (void) prio; return 0; }

ogg_codec_t ogg_detect_codec(const char * path) { (void) path; return (ogg_codec_t) 0; }
ogg_demux_t * ogg_demux_open(const char * path) { (void) path; return NULL; }
ogg_demux_t * ogg_demux_open_metadata(const char * path, bool skip_large_values) { (void) path; (void) skip_large_values; return NULL; }
unsigned int ogg_demux_get_comment_count(const ogg_demux_t * d) { (void) d; return 0; }
const char * ogg_demux_get_comment(const ogg_demux_t * d, unsigned int index, uint32_t * out_len) {
    (void) d; (void) index; (void) out_len;
    return NULL;
}
void ogg_demux_close(ogg_demux_t * d) { (void) d; }

stb_vorbis * stb_vorbis_open_filename(const char * filename, int * error, const stb_vorbis_alloc * alloc_buffer) {
    (void) filename; (void) alloc_buffer;
    if (error) *error = 1;
    return NULL;
}
stb_vorbis_comment stb_vorbis_get_comment(stb_vorbis * f) {
    stb_vorbis_comment c;
    (void) f;
    memset(&c, 0, sizeof(c));
    return c;
}
void stb_vorbis_close(stb_vorbis * f) { (void) f; }

int mbedtls_base64_decode(unsigned char * dst, size_t dlen, size_t * olen, const unsigned char * src, size_t slen) {
    (void) dst; (void) dlen; (void) src; (void) slen;
    if (olen) *olen = 0;
    return -1;
}
