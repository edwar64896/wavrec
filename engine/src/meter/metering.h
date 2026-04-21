#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * MeteringEngine context
 * ---------------------------------------------------------------------- */

typedef struct MeteringEngine {
    /* Per-track running accumulators — reset each update cycle. */
    float    sum_sq[128];       /* accumulates sample² for RMS */
    uint32_t sample_count[128]; /* samples processed this cycle */
    float    peak[128];         /* max|sample| this cycle */

    /* Peak hold — persists between cycles, decays toward silence. */
    float    peak_hold[128];

    /* Clip latch — set when |sample| >= 1.0; cleared by CMD_METER_RESET. */
    bool     clip[128];

    /* Peak-hold decay factor per 60Hz tick.
     * Default: 10^(-0.333/20) ≈ 0.9623 → drops ~60 dB in 3 seconds. */
    float    peak_hold_decay;

    /* Thread */
    void          *thread;    /* PlatformThread */
    _Atomic int    running;

    struct WavRecEngine *eng;
} MeteringEngine;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool metering_init(struct WavRecEngine *eng);
void metering_start(struct WavRecEngine *eng);
void metering_reset_clips(struct WavRecEngine *eng);
void metering_shutdown(struct WavRecEngine *eng);
