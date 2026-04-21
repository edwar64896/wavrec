#include "metering.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../audio/record_engine.h"
#include "../ipc/ipc_shm.h"
#include "../ipc/ipc_protocol.h"
#include "../track/track.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define METERING_UPDATE_MS     16     /* ~60 Hz */
#define METERING_CHUNK_FRAMES  512    /* samples read per ring drain step */

/* Peak-hold decay per 16ms tick: drops ~60 dB in ~3 seconds at 60 Hz.
 * factor = 10^(-0.333 dB / 20) per tick */
#define PEAK_HOLD_DECAY        0.96234f

/* Clip threshold — flag if |sample| reaches or exceeds 0 dBFS */
#define CLIP_THRESHOLD         1.0f

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static MeteringEngine *get_me(struct WavRecEngine *eng)
{
    return (MeteringEngine *)engine_metering(eng);
}

/* Process a block of samples for one track: accumulate peak, RMS, clip. */
static void accumulate(MeteringEngine *me, int i,
                       const float *buf, uint32_t n)
{
    float peak     = me->peak[i];
    float sum_sq   = me->sum_sq[i];

    for (uint32_t s = 0; s < n; s++) {
        float v = buf[s];
        float av = v < 0.0f ? -v : v;

        if (av >= CLIP_THRESHOLD)
            me->clip[i] = true;

        if (av > peak)
            peak = av;

        sum_sq += v * v;
    }

    me->peak[i]         = peak;
    me->sum_sq[i]       = sum_sq;
    me->sample_count[i] += n;
}

/* Compute final values, update peak hold, write one channel slot. */
static void finalise_channel(MeteringEngine *me, int i,
                              WavRecMeterChannel *ch,
                              bool active)
{
    float peak = me->peak[i];
    float rms  = 0.0f;

    if (me->sample_count[i] > 0)
        rms = sqrtf(me->sum_sq[i] / (float)me->sample_count[i]);

    /* Peak hold: if current peak exceeds hold, latch it; otherwise decay. */
    if (peak >= me->peak_hold[i])
        me->peak_hold[i] = peak;
    else
        me->peak_hold[i] *= me->peak_hold_decay;

    ch->true_peak  = peak;
    ch->rms        = rms;
    ch->peak_hold  = me->peak_hold[i];
    ch->clip       = me->clip[i] ? 1 : 0;
    ch->active     = active ? 1 : 0;

    /* Reset cycle accumulators. */
    me->peak[i]         = 0.0f;
    me->sum_sq[i]       = 0.0f;
    me->sample_count[i] = 0;
}

/* -------------------------------------------------------------------------
 * Metering thread
 * ---------------------------------------------------------------------- */

static void metering_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    MeteringEngine      *me  = get_me(eng);

    float buf[METERING_CHUNK_FRAMES];

    while (atomic_load_explicit(&me->running, memory_order_acquire)) {

        platform_sleep_ms(METERING_UPDATE_MS);

        WavRecMeterRegion *region = ipc_shm_meters(eng);
        int n_tracks = engine_n_tracks(eng);

        /* Drain meter rings for all armed/active tracks. */
        for (int i = 0; i < n_tracks; i++) {
            AudioRing *ring = record_engine_meter_ring(eng, i);
            if (!ring) continue;

            const Track *t = (const Track *)engine_get_track(eng, i);
            bool active = t && (t->armed || t->monitor || t->state == TRACK_STATE_RECORDING);

            if (!active) {
                /* No new samples — still decay peak hold and write zeros. */
                if (region) {
                    uint8_t slot = 1u - atomic_load_explicit(
                                          &region->header.write_index,
                                          memory_order_relaxed);
                    WavRecMeterChannel *ch =
                        (slot == 0) ? region->ch : region->ch_b;
                    finalise_channel(me, i, &ch[i], false);
                }
                continue;
            }

            /* Drain all available samples this cycle. */
            for (;;) {
                uint32_t got = audio_ring_read(ring, buf,
                                               METERING_CHUNK_FRAMES);
                if (got == 0) break;
                accumulate(me, i, buf, got);
            }
        }

        /* Write the completed frame to the inactive shared-memory slot,
         * then atomically flip write_index so the UI picks it up. */
        if (!region) continue;

        uint8_t cur_slot  = atomic_load_explicit(&region->header.write_index,
                                                  memory_order_relaxed);
        uint8_t next_slot = 1u - cur_slot;

        WavRecMeterChannel *ch =
            (next_slot == 0) ? region->ch : region->ch_b;

        /* Fill all channel slots in the inactive frame. */
        for (int i = 0; i < n_tracks; i++) {
            const Track *t = (const Track *)engine_get_track(eng, i);
            bool active = t && (t->armed || t->monitor || t->state == TRACK_STATE_RECORDING);
            finalise_channel(me, i, &ch[i], active);
        }

        /* Zero any slots beyond n_tracks. */
        if (n_tracks < WAVREC_MAX_CHANNELS)
            memset(&ch[n_tracks], 0,
                   (WAVREC_MAX_CHANNELS - n_tracks) * sizeof(WavRecMeterChannel));

        /* Release store — UI acquire-loads this. */
        atomic_store_explicit(&region->header.write_index, next_slot,
                               memory_order_release);
    }
}

/* -------------------------------------------------------------------------
 * Init / start / stop / shutdown
 * ---------------------------------------------------------------------- */

bool metering_init(struct WavRecEngine *eng)
{
    MeteringEngine *me = (MeteringEngine *)calloc(1, sizeof(MeteringEngine));
    if (!me) return false;

    me->eng             = eng;
    me->peak_hold_decay = PEAK_HOLD_DECAY;
    atomic_init(&me->running, 0);

    /* All accumulators, peak_hold, and clip flags are zero from calloc. */

    engine_set_metering(eng, me);
    return true;
}

void metering_start(struct WavRecEngine *eng)
{
    MeteringEngine *me = get_me(eng);
    if (!me || atomic_load_explicit(&me->running, memory_order_relaxed)) return;

    atomic_store_explicit(&me->running, 1, memory_order_release);
    me->thread = platform_thread_create(metering_thread, eng,
                                        PLATFORM_PRIO_ABOVE_NORMAL,
                                        "metering");
}

void metering_reset_clips(struct WavRecEngine *eng)
{
    MeteringEngine *me = get_me(eng);
    if (!me) return;
    memset(me->clip, 0, sizeof(me->clip));
}

void metering_shutdown(struct WavRecEngine *eng)
{
    MeteringEngine *me = get_me(eng);
    if (!me) return;

    if (atomic_load_explicit(&me->running, memory_order_relaxed)) {
        atomic_store_explicit(&me->running, 0, memory_order_release);
        platform_thread_join(me->thread);
        platform_thread_destroy(me->thread);
    }

    free(me);
    engine_set_metering(eng, NULL);
}
