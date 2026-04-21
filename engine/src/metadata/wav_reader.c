#include "wav_reader.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Little-endian read helpers (no alignment assumption)
 * ---------------------------------------------------------------------- */

static uint16_t r16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t r32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* -------------------------------------------------------------------------
 * wav_read_info — scan RIFF chunks for "fmt " and "data"
 * ---------------------------------------------------------------------- */

bool wav_read_info(FILE *fp, WavInfo *info)
{
    memset(info, 0, sizeof(*info));

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 && memcmp(hdr, "RF64", 4) != 0) return false;
    if (memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool found_fmt  = false;
    bool found_data = false;

    uint8_t chunk_hdr[8];
    while (!found_data && fread(chunk_hdr, 1, 8, fp) == 8) {
        uint32_t chunk_size = r32le(chunk_hdr + 4);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt_buf[40];
            uint32_t to_read = chunk_size < sizeof(fmt_buf)
                             ? chunk_size : (uint32_t)sizeof(fmt_buf);
            if (fread(fmt_buf, 1, to_read, fp) != to_read) return false;

            info->audio_format   = r16le(fmt_buf + 0);
            info->channels       = r16le(fmt_buf + 2);
            info->sample_rate    = r32le(fmt_buf + 4);
            info->bits_per_sample = r16le(fmt_buf + 14);

            /* Derive WavSampleFormat */
            if (info->audio_format == 3) {
                info->fmt = WAVREC_FMT_FLOAT32;
            } else if (info->bits_per_sample == 16) {
                info->fmt = WAVREC_FMT_PCM16;
            } else if (info->bits_per_sample == 24) {
                info->fmt = WAVREC_FMT_PCM24;
            } else {
                info->fmt = WAVREC_FMT_PCM32;
            }

            /* Skip remaining bytes of an extended fmt chunk */
            if (chunk_size > to_read)
                fseek(fp, (long)(chunk_size - to_read), SEEK_CUR);

            found_fmt = true;

        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            if (!found_fmt) return false;
            info->data_offset = ftell(fp);
            uint32_t bytes_per_frame = info->channels
                                     * (info->bits_per_sample / 8);
            info->data_frames = bytes_per_frame
                              ? (chunk_size / bytes_per_frame) : 0;
            found_data = true;

        } else {
            /* Skip unknown chunk — pad to even size per RIFF spec */
            long skip = (long)chunk_size + (chunk_size & 1);
            fseek(fp, skip, SEEK_CUR);
        }
    }

    return found_fmt && found_data;
}
