#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../engine.h"          /* WavSampleFormat */
#include "../audio/audio_io.h"  /* AudioRing */

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Per-track playback state
 * ---------------------------------------------------------------------- */

#define PLAYBACK_MAX_PATH  1024

typedef struct {
    FILE           *fp;
    bool            valid;
    WavSampleFormat fmt;
    uint16_t        channels;    /* channel count in the file (typically 1) */
    uint32_t        sample_rate; /* sample rate of the file */
    long            data_offset; /* byte offset of first audio frame */
    uint64_t        total_frames;
    uint64_t        read_frame;  /* current read position within the file */
} PlaybackFile;

/* -------------------------------------------------------------------------
 * PlaybackEngine context
 * ---------------------------------------------------------------------- */

#define PLAYBACK_CHUNK_FRAMES 512   /* frames mixed per iteration */

typedef struct PlaybackEngine {
    PlaybackFile    files[128];          /* WAVREC_MAX_CHANNELS */
    bool            track_active[128];

    /* Independent play head — advanced every iteration */
    _Atomic uint64_t play_head;

    /* Per-iteration mix scratch (stereo L/R) */
    float           mix_l[PLAYBACK_CHUNK_FRAMES];
    float           mix_r[PLAYBACK_CHUNK_FRAMES];

    /* File read scratch — widest format: 4 bytes/sample */
    uint8_t         raw_buf[PLAYBACK_CHUNK_FRAMES * 4];
    float           decoded[PLAYBACK_CHUNK_FRAMES];

    /* Thread */
    void           *thread;
    _Atomic int     running;

    struct WavRecEngine *eng;
} PlaybackEngine;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool playback_engine_init(struct WavRecEngine *eng);

/* Open files and begin playback from position_frames.
 * Looks for recorded files in the first valid recording target. */
void playback_engine_start(struct WavRecEngine *eng, uint64_t position_frames);

void playback_engine_stop(struct WavRecEngine *eng);

/* Seek the play head without stopping. */
void playback_engine_locate(struct WavRecEngine *eng, uint64_t position_frames);

uint64_t playback_engine_play_head(const struct WavRecEngine *eng);

void playback_engine_shutdown(struct WavRecEngine *eng);
