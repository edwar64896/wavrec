#pragma once
/*
 * Minimal WAV / BWF reader — extracts format info and data-chunk position.
 * Used by the Playback Engine to open recorded files.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../engine.h"   /* WavSampleFormat */

typedef struct {
    uint16_t        audio_format;   /* 1=PCM, 3=IEEE_FLOAT */
    uint16_t        channels;
    uint32_t        sample_rate;
    uint16_t        bits_per_sample;
    WavSampleFormat fmt;            /* derived from audio_format + bits_per_sample */
    long            data_offset;    /* byte offset of first audio sample */
    uint64_t        data_frames;    /* total frames in the file */
} WavInfo;

/* Read and parse the WAV/BWF headers of an already-open file.
 * Leaves fp positioned at the start of the audio data on success.
 * Returns false if the file is not a valid RIFF/WAVE file. */
bool wav_read_info(FILE *fp, WavInfo *info);
