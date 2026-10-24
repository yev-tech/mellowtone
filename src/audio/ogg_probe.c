/* ogg_detect_codec() (ogg_demux.h): the header probe that routes .ogg/.oga
 * files. Always built, Vorbis needs it too; the Opus demuxer itself
 * (ogg_demux.c) only with MT_CODEC_OPUS. */
#include "ogg_demux.h"

#include <stdio.h>
#include <string.h>

ogg_codec_t ogg_detect_codec(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return OGG_CODEC_UNKNOWN;

    uint8_t header[27];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "OggS", 4) != 0 || header[4] != 0 || !(header[5] & 0x02)) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }

    uint8_t segment_count = header[26];
    uint8_t first_lacing = 0;
    if (segment_count == 0 || fread(&first_lacing, 1, 1, f) != 1) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }
    if (segment_count > 1 && fseek(f, segment_count - 1, SEEK_CUR) != 0) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }

    uint8_t signature[8];
    if (first_lacing < sizeof(signature) || fread(signature, 1, sizeof(signature), f) != sizeof(signature)) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }
    fclose(f);

    if (memcmp(signature, "OpusHead", 8) == 0) return OGG_CODEC_OPUS;
    if (signature[0] == 1 && memcmp(signature + 1, "vorbis", 6) == 0) return OGG_CODEC_VORBIS;
    return OGG_CODEC_UNKNOWN;
}
