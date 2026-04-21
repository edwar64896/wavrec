#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "../util/ringbuf.h"

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * AudioRing — lock-free SPSC circular buffer of float32 samples.
 *
 * One ring per mono track (input path) and one per output channel (mix bus).
 * Capacity is in individual samples (not frames).
 * ONE producer thread calls audio_ring_write; ONE consumer calls audio_ring_read.
 * ---------------------------------------------------------------------- */

#define AUDIO_RING_FRAMES 32768  /* power of 2 — ~683ms@48k / ~171ms@192k.
                                  * Sized to absorb the stall when pre-roll
                                  * drains into disk_rings on record start. */

typedef struct {
    _Atomic uint32_t wp;
    char _pad0[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];
    _Atomic uint32_t rp;
    char _pad1[RINGBUF_CACHE_LINE - sizeof(_Atomic uint32_t)];
    float f[AUDIO_RING_FRAMES];
} AudioRing;

/* Write up to n samples from src. Returns samples actually written.
 * Safe to call from the audio callback (no allocation, no locks). */
static inline uint32_t audio_ring_write(AudioRing *rb,
                                        const float *src, uint32_t n)
{
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);
    uint32_t free_slots = AUDIO_RING_FRAMES - (w - r);
    if (n > free_slots) n = free_slots;
    if (n == 0) return 0;

    uint32_t wi   = w & (AUDIO_RING_FRAMES - 1);
    uint32_t tail = AUDIO_RING_FRAMES - wi;

    if (n <= tail) {
        memcpy(rb->f + wi, src, n * sizeof(float));
    } else {
        memcpy(rb->f + wi, src,        tail * sizeof(float));
        memcpy(rb->f,      src + tail, (n - tail) * sizeof(float));
    }
    atomic_store_explicit(&rb->wp, w + n, memory_order_release);
    return n;
}

/* Read up to n samples into dst. Returns samples actually read. */
static inline uint32_t audio_ring_read(AudioRing *rb,
                                       float *dst, uint32_t n)
{
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);
    uint32_t avail = w - r;
    if (n > avail) n = avail;
    if (n == 0) return 0;

    uint32_t ri   = r & (AUDIO_RING_FRAMES - 1);
    uint32_t tail = AUDIO_RING_FRAMES - ri;

    if (n <= tail) {
        memcpy(dst, rb->f + ri, n * sizeof(float));
    } else {
        memcpy(dst,        rb->f + ri, tail * sizeof(float));
        memcpy(dst + tail, rb->f,      (n - tail) * sizeof(float));
    }
    atomic_store_explicit(&rb->rp, r + n, memory_order_release);
    return n;
}

/* Approximate number of samples available to read. */
static inline uint32_t audio_ring_count(const AudioRing *rb)
{
    uint32_t w = atomic_load_explicit(&rb->wp, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&rb->rp, memory_order_acquire);
    return w - r;
}

static inline void audio_ring_init(AudioRing *rb)
{
    atomic_init(&rb->wp, 0u);
    atomic_init(&rb->rp, 0u);
}

/* -------------------------------------------------------------------------
 * AudioIO context (opaque to callers; defined fully here for engine.c)
 * ---------------------------------------------------------------------- */

/* Max callback period we'll pre-allocate a de-interleave buffer for. */
#define AUDIO_IO_MAX_PERIOD_FRAMES 4096

/* Use void* for miniaudio handles — full types only in audio_io.c where
 * miniaudio.h is included.  Avoids polluting the rest of the codebase
 * with the miniaudio header. */
typedef struct AudioIO {
    void        *context;        /* ma_context* — ASIO */
    void        *device;         /* ma_device*  */
    bool         device_open;

    /* Per-track mono input rings indexed by Track.id.
     * Written by the audio callback; read by the Record Engine thread. */
    AudioRing    input_rings[128];  /* WAVREC_MAX_CHANNELS = 128 */

    /* Stereo playback rings — written by Playback Engine, read by callback. */
    AudioRing    playback_ring_l;
    AudioRing    playback_ring_r;

    /* Per-callback monitor scratch — pre-allocated to avoid stack pressure
     * in the real-time callback.  The callback computes the monitor mix
     * here and sums it with playback before writing to the hardware output.
     * Sole writer: audio callback thread. */
    float        monitor_l[AUDIO_IO_MAX_PERIOD_FRAMES];
    float        monitor_r[AUDIO_IO_MAX_PERIOD_FRAMES];

    /* De-interleave scratch — written/read only within the callback. */
    float        tmp[AUDIO_IO_MAX_PERIOD_FRAMES];

    /* Diagnostics */
    _Atomic uint32_t  xrun_count;   /* input ring full — samples dropped */

    /* Back-reference to engine (set by audio_io_init) */
    struct WavRecEngine *eng;
} AudioIO;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Allocate AudioIO and initialise the audio backend context.
 * Does NOT open a device — call audio_io_open_device() after session init. */
bool audio_io_init(struct WavRecEngine *eng);

/* Open and start the audio device using the current session config.
 * Called when recording or playback begins. */
bool audio_io_open_device(struct WavRecEngine *eng);

/* Stop and close the device, but keep the context alive. */
void audio_io_close_device(struct WavRecEngine *eng);

/* Full teardown — stop device + uninit context + free AudioIO. */
void audio_io_shutdown(struct WavRecEngine *eng);

/* Enumerate available input and output devices.
 * Returns a malloc'd JSON array string; caller frees.
 * Format: [{"name":"...","type":"input","channels":N}, ...] */
char *audio_io_list_devices(struct WavRecEngine *eng);

/* Convenience accessors used by Record Engine / Playback Engine. */
AudioIO *audio_io_get(struct WavRecEngine *eng);
