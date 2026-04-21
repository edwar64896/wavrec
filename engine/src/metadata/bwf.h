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

/* Write RIFF/WAVE header + JUNK placeholder (for future ds64 promotion) +
 * BEXT + iXML (if non-NULL) + fmt + data chunks.  Returns byte offset of
 * the data chunk size field, or -1 on error.  If out_junk_offset is
 * non-NULL, writes the offset of the JUNK chunk header (always 12 in the
 * current layout). */
long bwf_write_header(FILE *fp, const BextChunk *bext,
                      const char *ixml_text,   /* NULL to omit iXML chunk */
                      uint32_t sample_rate, WavSampleFormat fmt,
                      uint8_t n_channels,
                      long *out_junk_offset);

/* Update on-disk size fields to reflect n_frames written so far.
 * Under 4 GB: stays as RIFF with 32-bit size fields; JUNK stays JUNK.
 * At or above 4 GB: promotes magic to RF64, promotes JUNK to ds64, writes
 * 64-bit sizes, and puts 0xFFFFFFFF sentinels in the 32-bit size slots.
 * Does fseek+fwrite but NOT fflush/fsync — caller should flush both.
 * Leaves the file position at end-of-file on success. */
bool bwf_update_sizes(FILE *fp,
                      long junk_offset,
                      long data_size_offset,
                      uint64_t n_frames,
                      uint8_t n_channels,
                      uint8_t bit_depth);

/* Final size update on close.  Equivalent to bwf_update_sizes() + fflush(). */
bool bwf_finalise(FILE *fp, long junk_offset, long data_size_offset,
                  uint64_t n_frames,
                  uint8_t n_channels, uint8_t bit_depth);
