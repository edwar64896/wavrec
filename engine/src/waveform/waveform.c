#include "waveform.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../audio/record_engine.h"
#include "../ipc/ipc_shm.h"
#include "../ipc/ipc_protocol.h"
#include "../track/track.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static WaveformEngine *get_we(struct WavRecEngine *eng)
{
    return (WaveformEngine *)engine_waveform(eng);
}

/* Emit one WavRecWfmBlock to the shared memory waveform ring.
 * Returns true if written, false if the ring was full (block dropped). */
static bool emit_block(WavRecWfmRegion *region,
                       uint8_t channel_id,
                       uint64_t timeline_frame,
                       uint32_t n_samples,
                       float min_val, float max_val)
{
    uint32_t w = atomic_load_explicit(&region->write_pos, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&region->read_pos,  memory_order_acquire);

    if ((w - r) >= WAVREC_WFM_RING_SLOTS)
        return false; /* ring full — UI not consuming fast enough; drop block */

    WavRecWfmBlock *b = &region->blocks[w & (WAVREC_WFM_RING_SLOTS - 1)];
    b->channel_id     = channel_id;
    b->_pad[0] = b->_pad[1] = b->_pad[2] = 0;
    b->n_samples      = n_samples;
    b->timeline_frame = timeline_frame;
    b->min            = min_val;
    b->max            = max_val;

    atomic_store_explicit(&region->write_pos, w + 1, memory_order_release);
    return true;
}

/* -------------------------------------------------------------------------
 * Waveform thread
 *
 * Polls wfm_rings at ~100Hz (10ms sleep).  Accumulates samples into
 * decimation blocks, emits WavRecWfmBlock to shared memory on completion.
 *
 * No gain is applied — raw samples, consistent with the record path.
 * ---------------------------------------------------------------------- */

static void waveform_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    WaveformEngine      *we  = get_we(eng);

    float buf[WFM_CHUNK_FRAMES];

    while (atomic_load_explicit(&we->running, memory_order_acquire)) {

        platform_sleep_ms(10);

        if (!engine_is_recording(eng)) continue;

        WavRecWfmRegion *region = ipc_shm_waveforms(eng);
        if (!region) continue;

        uint32_t decim = we->decimation;
        if (decim == 0) decim = region->decimation;
        if (decim == 0) decim = 512;

        int n_tracks = engine_n_tracks(eng);

        for (int i = 0; i < n_tracks; i++) {
            const Track *t = (const Track *)engine_get_track(eng, i);
            if (!t || !t->armed) continue;

            AudioRing     *ring  = record_engine_wfm_ring(eng, i);
            WfmTrackState *state = &we->tracks[i];
            if (!ring) continue;

            /* Drain the ring entirely this tick. */
            for (;;) {
                uint32_t got = audio_ring_read(ring, buf, WFM_CHUNK_FRAMES);
                if (got == 0) break;

                for (uint32_t s = 0; s < got; s++) {
                    float v = buf[s];

                    if (v < state->min) state->min = v;
                    if (v > state->max) state->max = v;
                    state->count++;

                    if (state->count >= decim) {
                        uint64_t frame = we->record_origin_frame
                                       + state->blocks_emitted * decim;

                        emit_block(region, (uint8_t)i, frame,
                                   decim, state->min, state->max);

                        state->blocks_emitted++;
                        /* Reset accumulator for next block */
                        state->min   =  1.0f;
                        state->max   = -1.0f;
                        state->count = 0;
                    }
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Init / start / shutdown
 * ---------------------------------------------------------------------- */

bool waveform_init(struct WavRecEngine *eng)
{
    WaveformEngine *we = (WaveformEngine *)calloc(1, sizeof(WaveformEngine));
    if (!we) return false;

    we->eng        = eng;
    we->decimation = 512; /* default; overridden by wfm region on start */
    atomic_init(&we->running, 0);

    /* Prime accumulators to neutral values */
    for (int i = 0; i < 128; i++) {
        we->tracks[i].min = 1.0f;
        we->tracks[i].max = -1.0f;
    }

    engine_set_waveform(eng, we);
    return true;
}

void waveform_start(struct WavRecEngine *eng)
{
    WaveformEngine *we = get_we(eng);
    if (!we || atomic_load_explicit(&we->running, memory_order_relaxed)) return;

    /* Latch the engine frame at record start for timeline alignment */
    we->record_origin_frame = engine_frame_counter(eng);

    /* Read decimation from the shm region if available */
    WavRecWfmRegion *region = ipc_shm_waveforms(eng);
    if (region && region->decimation > 0)
        we->decimation = region->decimation;

    /* Reset all track states */
    for (int i = 0; i < 128; i++) {
        we->tracks[i].min          =  1.0f;
        we->tracks[i].max          = -1.0f;
        we->tracks[i].count        = 0;
        we->tracks[i].blocks_emitted = 0;
    }

    atomic_store_explicit(&we->running, 1, memory_order_release);
    we->thread = platform_thread_create(waveform_thread, eng,
                                        PLATFORM_PRIO_NORMAL,
                                        "waveform");
}

void waveform_shutdown(struct WavRecEngine *eng)
{
    WaveformEngine *we = get_we(eng);
    if (!we) return;

    if (atomic_load_explicit(&we->running, memory_order_relaxed)) {
        atomic_store_explicit(&we->running, 0, memory_order_release);
        platform_thread_join(we->thread);
        platform_thread_destroy(we->thread);
    }

    free(we);
    engine_set_waveform(eng, NULL);
}
