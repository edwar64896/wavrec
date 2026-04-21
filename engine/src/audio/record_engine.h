#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../audio/audio_io.h"   /* AudioRing */
#include "../util/ringbuf.h"

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Per-track feed rings owned by the Record Engine.
 *
 * The Record Engine is the sole producer for all four ring types.
 * Each ring has a dedicated consumer:
 *
 *   disk_rings   → Disk Writer thread (raw float32, no gain applied)
 *   meter_rings  → Metering thread
 *   wfm_rings    → Waveform thread
 *   txcr_rings   → Transcription thread
 *
 * All rings use the same AudioRing type (8192 frames, ~170ms at 48kHz).
 * Transcription rings are larger to absorb whisper.cpp inference latency.
 * ---------------------------------------------------------------------- */

#define TXCR_RING_FRAMES  65536   /* ~1.36s at 48kHz — power of 2 */

typedef struct {
    _Atomic uint32_t wp;
    char _pad0[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];
    _Atomic uint32_t rp;
    char _pad1[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];
    float f[TXCR_RING_FRAMES];
} TxcrRing;

static inline void txcr_ring_init(TxcrRing *rb)
{
    atomic_init(&rb->wp, 0u);
    atomic_init(&rb->rp, 0u);
}

static inline uint32_t txcr_ring_write(TxcrRing *rb, const float *src, uint32_t n)
{
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);
    uint32_t free_slots = TXCR_RING_FRAMES - (w - r);
    if (n > free_slots) n = free_slots;
    if (n == 0) return 0;
    uint32_t wi   = w & (TXCR_RING_FRAMES - 1);
    uint32_t tail = TXCR_RING_FRAMES - wi;
    if (n <= tail) {
        memcpy(rb->f + wi, src,        n    * sizeof(float));
    } else {
        memcpy(rb->f + wi, src,        tail * sizeof(float));
        memcpy(rb->f,      src + tail, (n - tail) * sizeof(float));
    }
    atomic_store_explicit(&rb->wp, w + n, memory_order_release);
    return n;
}

static inline uint32_t txcr_ring_read(TxcrRing *rb, float *dst, uint32_t n)
{
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);
    uint32_t avail = w - r;
    if (n > avail) n = avail;
    if (n == 0) return 0;
    uint32_t ri   = r & (TXCR_RING_FRAMES - 1);
    uint32_t tail = TXCR_RING_FRAMES - ri;
    if (n <= tail) {
        memcpy(dst,        rb->f + ri, n    * sizeof(float));
    } else {
        memcpy(dst,        rb->f + ri, tail * sizeof(float));
        memcpy(dst + tail, rb->f,      (n - tail) * sizeof(float));
    }
    atomic_store_explicit(&rb->rp, r + n, memory_order_release);
    return n;
}

static inline uint32_t txcr_ring_count(const TxcrRing *rb)
{
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);
    return w - r;
}

/* -------------------------------------------------------------------------
 * RecordEngine context
 * ---------------------------------------------------------------------- */

#define WAVREC_MAX_CHANNELS 128

typedef struct RecordEngine {
    /* Feed rings — Record Engine writes, consumers read.
     * Raw float32 samples, no gain applied anywhere in this path. */
    AudioRing   disk_rings [WAVREC_MAX_CHANNELS]; /* → Disk Writer      */
    AudioRing   meter_rings[WAVREC_MAX_CHANNELS]; /* → Metering thread  */
    AudioRing   wfm_rings  [WAVREC_MAX_CHANNELS]; /* → Waveform thread  */
    TxcrRing    txcr_rings [WAVREC_MAX_CHANNELS]; /* → Transcription    */

    /* Per-track transcription enable flag (set via CMD_TRANSCRIPTION_CONFIG) */
    bool        txcr_enabled[WAVREC_MAX_CHANNELS];

    /* Overflow counters — incremented when a downstream ring is full.
     * Non-fatal: the sample is dropped from that feed only. */
    _Atomic uint32_t disk_overflows;
    _Atomic uint32_t meter_overflows;
    _Atomic uint32_t wfm_overflows;
    _Atomic uint32_t txcr_overflows;

    /* Thread */
    void           *thread;      /* PlatformThread */
    _Atomic int     running;

    struct WavRecEngine *eng;
} RecordEngine;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool record_engine_init(struct WavRecEngine *eng);
void record_engine_start(struct WavRecEngine *eng);
void record_engine_stop(struct WavRecEngine *eng);
void record_engine_shutdown(struct WavRecEngine *eng);

/* Feed ring accessors used by downstream consumer threads. */
AudioRing *record_engine_disk_ring (struct WavRecEngine *eng, int track_id);
AudioRing *record_engine_meter_ring(struct WavRecEngine *eng, int track_id);
AudioRing *record_engine_wfm_ring  (struct WavRecEngine *eng, int track_id);
TxcrRing  *record_engine_txcr_ring (struct WavRecEngine *eng, int track_id);

/* Enable/disable transcription feed for a track. */
void record_engine_set_txcr(struct WavRecEngine *eng, int track_id, bool enabled);
