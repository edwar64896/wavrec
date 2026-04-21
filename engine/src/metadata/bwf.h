#pragma once
/*
 * BWF / BEXT chunk writer.
 * Writes the RIFF/WAVE container with BEXT and iXML chunks.
 * File handles are managed by disk_writer; bwf_write_header() is called
 * once per file on open, bwf_finalise() updates sizes on close.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "../timecode/timecode.h"
#include "../engine.h"   /* WavSampleFormat, wavrec_fmt_bit_depth */

typedef struct {
    char     description[256];
    char     originator[32];
    char     originator_ref[32];
    char     origination_date[11]; /* "YYYY-MM-DD\0" */
    char     origination_time[9];  /* "HH:MM:SS\0" */
    uint64_t time_reference;       /* samples since midnight */
    uint16_t version;              /* = 1 */
    char     coding_history[512];
} BextChunk;

void bwf_build_bext(BextChunk *bext,
                    const char *description,
                    uint32_t sample_rate,
                    WavSampleFormat fmt,
                    uint8_t n_channels,
                    const WavRecTimecodeSource *tc,
                    uint64_t engine_frame);

/* Write RIFF/WAVE header + BEXT + fmt chunks to fp.
 * Returns byte offset of the data chunk size field (for finalise). */
long bwf_write_header(FILE *fp, const BextChunk *bext,
                      uint32_t sample_rate, WavSampleFormat fmt,
                      uint8_t n_channels);

/* Seek back and patch RIFF and data chunk sizes. */
bool bwf_finalise(FILE *fp, long data_size_offset, uint64_t n_frames,
                  uint8_t n_channels, uint8_t bit_depth);
