#include "record_engine.h"
#include "audio_io.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../track/track.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static RecordEngine *get_rec(struct WavRecEngine *eng)
{
    return (RecordEngine *)engine_record_engine(eng);
}

/* -------------------------------------------------------------------------
 * Record Engine thread
 *
 * Polls input rings every 1ms.  Drains whatever is available, distributes
 * raw float32 samples to disk, meter, waveform, and transcription feeds.
 *
 * Design notes:
 *   - NO gain applied here.  Samples are written exactly as received from
 *     the audio callback.  Gain/EQ lives in the playback/monitoring path.
 *   - 1ms poll is safe: AudioRing holds ~170ms before overflowing, so
 *     there is ample headroom even under brief scheduling jitter.
 *   - If a downstream ring is full, that feed's overflow counter is
 *     incremented and the frame is dropped for that feed only.  Recording
 *     to disk takes priority — its ring is drained first.
 * ---------------------------------------------------------------------- */

#define RECORD_ENGINE_CHUNK_FRAMES 1024   /* scratch buffer size */

static void record_engine_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    RecordEngine        *rec = get_rec(eng);
    AudioIO             *aio = audio_io_get(eng);

    float buf[RECORD_ENGINE_CHUNK_FRAMES];

    while (atomic_load_explicit(&rec->running, memory_order_acquire)) {

        if (!aio) { platform_sleep_ms(1); continue; }

        bool is_recording = engine_is_recording(eng);
        int  n_tracks     = engine_n_tracks(eng);
        bool did_work     = false;

        for (int i = 0; i < n_tracks; i++) {
            const Track *t = (const Track *)engine_get_track(eng, i);
            if (!t) continue;

            /* Process this track if it has data in its input ring.
             * Armed tracks write during recording; monitored tracks write
             * whenever the device is open (so metering is always live). */
            if (!t->armed && !t->monitor) continue;

            AudioRing *in = &aio->input_rings[i];

            for (;;) {
                uint32_t got = audio_ring_read(in, buf, RECORD_ENGINE_CHUNK_FRAMES);
                if (got == 0) break;
                did_work = true;

                /* --- Disk feed — only when actually recording ---------- */
                if (is_recording && t->armed) {
                    uint32_t w = audio_ring_write(&rec->disk_rings[i], buf, got);
                    if (w < got)
                        atomic_fetch_add(&rec->disk_overflows, 1u);
                }

                /* --- Meter feed — always when track is active --------- */
                uint32_t w = audio_ring_write(&rec->meter_rings[i], buf, got);
                if (w < got)
                    atomic_fetch_add(&rec->meter_overflows, 1u);

                /* --- Waveform feed ------------------------------------ */
                w = audio_ring_write(&rec->wfm_rings[i], buf, got);
                if (w < got)
                    atomic_fetch_add(&rec->wfm_overflows, 1u);

                /* --- Transcription feed — only when recording --------- */
                if (is_recording && rec->txcr_enabled[i]) {
                    w = txcr_ring_write(&rec->txcr_rings[i], buf, got);
                    if (w < got)
                        atomic_fetch_add(&rec->txcr_overflows, 1u);
                }
            }
        }

        if (!did_work)
            platform_sleep_ms(1);
    }
}

/* -------------------------------------------------------------------------
 * Init / start / stop / shutdown
 * ---------------------------------------------------------------------- */

bool record_engine_init(struct WavRecEngine *eng)
{
    /* sizeof(RecordEngine) ≈ 44MB — dominated by 128×256KB txcr rings.
     * Single contiguous allocation; normal for a 128-channel workstation. */
    RecordEngine *rec = (RecordEngine *)calloc(1, sizeof(RecordEngine));
    if (!rec) return false;

    rec->eng = eng;
    atomic_init(&rec->running,         0);
    atomic_init(&rec->disk_overflows,  0u);
    atomic_init(&rec->meter_overflows, 0u);
    atomic_init(&rec->wfm_overflows,   0u);
    atomic_init(&rec->txcr_overflows,  0u);

    for (int i = 0; i < WAVREC_MAX_CHANNELS; i++) {
        audio_ring_init(&rec->disk_rings[i]);
        audio_ring_init(&rec->meter_rings[i]);
        audio_ring_init(&rec->wfm_rings[i]);
        txcr_ring_init(&rec->txcr_rings[i]);
        rec->txcr_enabled[i] = false;
    }

    engine_set_record_engine(eng, rec);
    return true;
}

void record_engine_start(struct WavRecEngine *eng)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return;

    if (atomic_load_explicit(&rec->running, memory_order_relaxed)) return;

    atomic_store_explicit(&rec->running, 1, memory_order_release);

    rec->thread = platform_thread_create(record_engine_thread, eng,
                                         PLATFORM_PRIO_HIGH,
                                         "rec_engine");
}

void record_engine_stop(struct WavRecEngine *eng)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return;
    if (!atomic_load_explicit(&rec->running, memory_order_relaxed)) return;

    atomic_store_explicit(&rec->running, 0, memory_order_release);
    platform_thread_join(rec->thread);
    platform_thread_destroy(rec->thread);
    rec->thread = NULL;

    /* Reset overflow counters on each stop so the next session starts clean. */
    atomic_store_explicit(&rec->disk_overflows,  0u, memory_order_relaxed);
    atomic_store_explicit(&rec->meter_overflows, 0u, memory_order_relaxed);
    atomic_store_explicit(&rec->wfm_overflows,   0u, memory_order_relaxed);
    atomic_store_explicit(&rec->txcr_overflows,  0u, memory_order_relaxed);
}

void record_engine_shutdown(struct WavRecEngine *eng)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return;
    record_engine_stop(eng);
    free(rec);
    engine_set_record_engine(eng, NULL);
}

/* -------------------------------------------------------------------------
 * Feed ring accessors for downstream consumer threads
 * ---------------------------------------------------------------------- */

AudioRing *record_engine_disk_ring(struct WavRecEngine *eng, int track_id)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec || track_id < 0 || track_id >= WAVREC_MAX_CHANNELS) return NULL;
    return &rec->disk_rings[track_id];
}

AudioRing *record_engine_meter_ring(struct WavRecEngine *eng, int track_id)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec || track_id < 0 || track_id >= WAVREC_MAX_CHANNELS) return NULL;
    return &rec->meter_rings[track_id];
}

AudioRing *record_engine_wfm_ring(struct WavRecEngine *eng, int track_id)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec || track_id < 0 || track_id >= WAVREC_MAX_CHANNELS) return NULL;
    return &rec->wfm_rings[track_id];
}

TxcrRing *record_engine_txcr_ring(struct WavRecEngine *eng, int track_id)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec || track_id < 0 || track_id >= WAVREC_MAX_CHANNELS) return NULL;
    return &rec->txcr_rings[track_id];
}

void record_engine_set_txcr(struct WavRecEngine *eng, int track_id, bool enabled)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec || track_id < 0 || track_id >= WAVREC_MAX_CHANNELS) return;
    rec->txcr_enabled[track_id] = enabled;
}
