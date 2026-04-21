#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Per-track accumulator state
 * ---------------------------------------------------------------------- */

typedef struct {
    float    min;           /* running min sample value in current block */
    float    max;           /* running max sample value in current block */
    uint32_t count;         /* samples accumulated so far in this block  */
    uint64_t blocks_emitted;/* total blocks emitted for this track       */
} WfmTrackState;

/* -------------------------------------------------------------------------
 * WaveformEngine context
 * ---------------------------------------------------------------------- */

#define WFM_CHUNK_FRAMES 256   /* samples read from ring per step */

typedef struct WaveformEngine {
    WfmTrackState  tracks[128];         /* WAVREC_MAX_CHANNELS */

    /* Captured at waveform_start() so blocks carry correct timeline frames. */
    uint64_t       record_origin_frame;
    uint32_t       decimation;          /* samples per display block */

    void          *thread;              /* PlatformThread */
    _Atomic int    running;

    struct WavRecEngine *eng;
} WaveformEngine;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool waveform_init(struct WavRecEngine *eng);

/* Called when recording starts — resets accumulators and latches origin. */
void waveform_start(struct WavRecEngine *eng);

void waveform_shutdown(struct WavRecEngine *eng);
