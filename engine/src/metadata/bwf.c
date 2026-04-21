#include "bwf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Little-endian write helpers
 * ---------------------------------------------------------------------- */

static void wu16(FILE *fp, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, fp);
}

static void wu32(FILE *fp, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, fp);
}

static void wu64_lo_hi(FILE *fp, uint64_t v)
{
    wu32(fp, (uint32_t)(v & 0xFFFFFFFF));
    wu32(fp, (uint32_t)(v >> 32));
}

static void wpad(FILE *fp, size_t n)
{
    uint8_t zero = 0;
    for (size_t i = 0; i < n; i++) fwrite(&zero, 1, 1, fp);
}

static void wfixed(FILE *fp, const char *src, size_t field_len)
{
    size_t src_len = src ? strlen(src) : 0;
    if (src_len > field_len) src_len = field_len;
    if (src_len) fwrite(src, 1, src_len, fp);
    wpad(fp, field_len - src_len);
}

/* -------------------------------------------------------------------------
 * BEXT chunk build
 * ---------------------------------------------------------------------- */

void bwf_build_bext(BextChunk *bext,
                    const char *description,
                    uint32_t sample_rate,
                    WavSampleFormat fmt,
                    uint8_t n_channels,
                    const WavRecTimecodeSource *tc,
                    uint64_t engine_frame)
{
    memset(bext, 0, sizeof(*bext));

    if (description)
        strncpy(bext->description, description, sizeof(bext->description) - 1);

    strncpy(bext->originator,     "WavRec", sizeof(bext->originator) - 1);
    strncpy(bext->originator_ref, "WavRec", sizeof(bext->originator_ref) - 1);

    /* Wall-clock date and time */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    snprintf(bext->origination_date, sizeof(bext->origination_date),
             "%04d-%02d-%02d",
             lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    snprintf(bext->origination_time, sizeof(bext->origination_time),
             "%02d:%02d:%02d",
             lt->tm_hour, lt->tm_min, lt->tm_sec);

    /* TimeReference: SMPTE frame count → samples since midnight */
    if (tc && tc->locked) {
        uint64_t tc_frames = tc->tc_frames_at_origin;
        const TcRateInfo *ri = tc_rate_info(tc->rate);
        bext->time_reference = (tc_frames * sample_rate) / ri->nominal;
    }
    (void)engine_frame;

    bext->version = 1;

    const char *mono_str = (n_channels == 1) ? "mono"
                         : (n_channels == 2) ? "stereo"
                         : "multi";
    const char *a_type   = (fmt == WAVREC_FMT_FLOAT32) ? "FLOAT" : "PCM";
    uint8_t     bit_depth = wavrec_fmt_bit_depth(fmt);

    snprintf(bext->coding_history, sizeof(bext->coding_history),
             "A=%s,F=%u,W=%u,M=%s,T=WavRec 0.1.0\r\n",
             a_type, sample_rate, bit_depth, mono_str);
}

/* -------------------------------------------------------------------------
 * RIFF/WAVE header writer
 *
 * Returns the file offset of the data chunk size field (for finalise).
 * Returns -1 on error.
 * ---------------------------------------------------------------------- */

long bwf_write_header(FILE *fp, const BextChunk *bext,
                      uint32_t sample_rate, WavSampleFormat fmt,
                      uint8_t n_channels)
{
    uint8_t  bit_depth     = wavrec_fmt_bit_depth(fmt);
    uint8_t  bytes_per_smp = wavrec_fmt_bytes(fmt);
    /* WAV AudioFormat: 1 = PCM integer, 3 = IEEE_FLOAT */
    uint16_t audio_format  = (fmt == WAVREC_FMT_FLOAT32) ? 3u : 1u;

    size_t coding_len   = strlen(bext->coding_history) + 1;
    if (coding_len & 1) coding_len++;
    size_t bext_payload = 602 + coding_len;

    uint16_t loudness_unset = 0x7FFF;

    /* RIFF header */
    fwrite("RIFF", 1, 4, fp);
    long riff_size_offset = ftell(fp);
    wu32(fp, 0);
    fwrite("WAVE", 1, 4, fp);

    /* bext chunk */
    fwrite("bext", 1, 4, fp);
    wu32(fp, (uint32_t)bext_payload);
    wfixed(fp, bext->description,     256);
    wfixed(fp, bext->originator,       32);
    wfixed(fp, bext->originator_ref,   32);
    wfixed(fp, bext->origination_date, 10);
    wfixed(fp, bext->origination_time,  8);
    wu64_lo_hi(fp, bext->time_reference);
    wu16(fp, bext->version);
    wpad(fp, 64);  /* UMID */
    wu16(fp, loudness_unset);
    wu16(fp, loudness_unset);
    wu16(fp, loudness_unset);
    wu16(fp, loudness_unset);
    wu16(fp, loudness_unset);
    wpad(fp, 180);
    {
        size_t raw_len = strlen(bext->coding_history);
        fwrite(bext->coding_history, 1, raw_len, fp);
        wpad(fp, coding_len - raw_len);
    }

    /* fmt chunk */
    fwrite("fmt ", 1, 4, fp);
    wu32(fp, 16);
    wu16(fp, audio_format);
    wu16(fp, n_channels);
    wu32(fp, sample_rate);
    wu32(fp, (uint32_t)sample_rate * n_channels * bytes_per_smp); /* ByteRate */
    wu16(fp, (uint16_t)(n_channels * bytes_per_smp));             /* BlockAlign */
    wu16(fp, bit_depth);

    /* data chunk header */
    fwrite("data", 1, 4, fp);
    long data_size_offset = ftell(fp);
    wu32(fp, 0);

    fflush(fp);
    (void)riff_size_offset;
    return data_size_offset;
}

/* -------------------------------------------------------------------------
 * Finalise: seek back and patch RIFF + data chunk sizes.
 * ---------------------------------------------------------------------- */

bool bwf_finalise(FILE *fp, long data_size_offset, uint64_t n_frames,
                  uint8_t n_channels, uint8_t bit_depth)
{
    uint64_t data_bytes = n_frames * n_channels * (bit_depth / 8);

    if (fseek(fp, data_size_offset, SEEK_SET) != 0) return false;
    wu32(fp, (uint32_t)(data_bytes & 0xFFFFFFFF));

    long file_end = data_size_offset + 4 + (long)data_bytes;
    if (fseek(fp, 4, SEEK_SET) != 0) return false;
    wu32(fp, (uint32_t)(file_end - 8));

    fflush(fp);
    return true;
}
