#include "record_engine.h"
#include "audio_io.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../track/track.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

/* -------------------------------------------------------------------------
 * PreRollRing — single-threaded circular buffer with drop-oldest semantics.
 * Only touched by record_engine_thread except `count`, which is atomic for
 * the IPC command thread's query-the-min path.
 * ---------------------------------------------------------------------- */

static void preroll_init(PreRollRing *r)
{
    r->buf      = NULL;
    r->capacity = 0;
    r->head     = 0;
    atomic_init(&r->count, 0);
}

static void preroll_destroy(PreRollRing *r)
{
    free(r->buf);
    r->buf      = NULL;
    r->capacity = 0;
    r->head     = 0;
    atomic_store_explicit(&r->count, 0, memory_order_relaxed);
}

static void preroll_resize(PreRollRing *r, uint32_t new_cap)
{
    free(r->buf);
    r->buf      = (new_cap > 0) ? (float *)calloc(new_cap, sizeof(float)) : NULL;
    r->capacity = r->buf ? new_cap : 0;
    r->head     = 0;
    atomic_store_explicit(&r->count, 0, memory_order_relaxed);
}

static void preroll_clear(PreRollRing *r)
{
    r->head = 0;
    atomic_store_explicit(&r->count, 0, memory_order_release);
}

/* Append n samples, overwriting the oldest when full. */
static void preroll_write(PreRollRing *r, const float *src, uint32_t n)
{
    if (r->capacity == 0 || n == 0) return;

    /* If src is larger than the whole ring, only the last `capacity` count. */
    if (n >= r->capacity) {
        src += (n - r->capacity);
        n    = r->capacity;
        memcpy(r->buf, src, n * sizeof(float));
        r->head = 0;
        atomic_store_explicit(&r->count, r->capacity, memory_order_release);
        return;
    }

    uint32_t tail = r->capacity - r->head;
    if (n <= tail) {
        memcpy(r->buf + r->head, src, n * sizeof(float));
    } else {
        memcpy(r->buf + r->head, src,        tail * sizeof(float));
        memcpy(r->buf,           src + tail, (n - tail) * sizeof(float));
    }
    r->head = (r->head + n) % r->capacity;

    uint32_t cur = atomic_load_explicit(&r->count, memory_order_relaxed);
    uint32_t nc  = cur + n;
    if (nc > r->capacity) nc = r->capacity;
    atomic_store_explicit(&r->count, nc, memory_order_release);
}

/* Drain the last `n` (most recent) samples into dst.  Blocks briefly if
 * dst is full — acceptable because this runs once at the IDLE→RECORDING
 * transition, not in steady-state. */
static void preroll_drain_last_n(PreRollRing *r, uint32_t n, AudioRing *dst)
{
    if (r->capacity == 0 || n == 0) return;
    uint32_t avail = atomic_load_explicit(&r->count, memory_order_acquire);
    if (n > avail) n = avail;
    if (n == 0) { preroll_clear(r); return; }

    /* Range [start, start+n) with wrap: start = head − n (mod capacity) */
    uint32_t start = (r->head + r->capacity - n) % r->capacity;

    uint32_t written = 0;
    while (written < n) {
        uint32_t pos   = (start + written) % r->capacity;
        uint32_t chunk = n - written;
        if (pos + chunk > r->capacity) chunk = r->capacity - pos;

        uint32_t w = audio_ring_write(dst, r->buf + pos, chunk);
        if (w == 0) {
            /* dst ring full — let the disk writer drain it, try again. */
            platform_sleep_ms(1);
            continue;
        }
        written += w;
    }
    preroll_clear(r);
}

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

        /* Transition edge IDLE→RECORDING: prepend pre-roll to disk rings.
         * Only fires on the true start of a take — punch (RECORDING→RECORDING)
         * keeps was_recording=true so this block is skipped.
         *
         * The drain is INTERLEAVED across armed tracks in small chunks: the
         * disk_writer thread waits for the MIN sample count across every
         * channel of a poly folder before writing, so if we drained track 0
         * fully before starting track 1 the disk_writer would never catch up
         * and track 0's disk ring would deadlock on full. */
        if (is_recording && !rec->was_recording) {
            uint32_t drain_n = atomic_load_explicit(
                &rec->pre_roll_drain_frames, memory_order_acquire);
            if (drain_n > 0) {
                uint32_t pos   [WAVREC_MAX_CHANNELS] = {0};
                uint32_t remain[WAVREC_MAX_CHANNELS] = {0};

                /* Plan per-track drain start position + remaining count. */
                for (int i = 0; i < n_tracks; i++) {
                    const Track *t = (const Track *)engine_get_track(eng, i);
                    if (!t || !t->armed) continue;
                    PreRollRing *r = &rec->pre_roll_rings[i];
                    if (r->capacity == 0) continue;
                    uint32_t avail = atomic_load_explicit(&r->count,
                                                          memory_order_acquire);
                    uint32_t n     = (drain_n < avail) ? drain_n : avail;
                    if (n == 0) continue;
                    pos   [i] = (r->head + r->capacity - n) % r->capacity;
                    remain[i] = n;
                }

                /* Round-robin chunk writes; disk_writer keeps up as rings fill. */
                const uint32_t CHUNK_SZ = 2048;
                for (;;) {
                    bool any_left     = false;
                    bool any_progress = false;
                    for (int i = 0; i < n_tracks; i++) {
                        if (remain[i] == 0) continue;
                        any_left = true;
                        PreRollRing *r = &rec->pre_roll_rings[i];
                        uint32_t c = (CHUNK_SZ < remain[i]) ? CHUNK_SZ : remain[i];
                        if (pos[i] + c > r->capacity) c = r->capacity - pos[i];
                        uint32_t w = audio_ring_write(&rec->disk_rings[i],
                                                      r->buf + pos[i], c);
                        if (w > 0) {
                            pos[i]    = (pos[i] + w) % r->capacity;
                            remain[i] -= w;
                            any_progress = true;
                        }
                    }
                    if (!any_left) break;
                    if (!any_progress) platform_sleep_ms(1);
                }
            }
            /* Clear all pre-roll rings so the next take doesn't inherit
             * stale data, and so newly-armed tracks start with empty rings. */
            for (int i = 0; i < n_tracks; i++)
                preroll_clear(&rec->pre_roll_rings[i]);

            atomic_store_explicit(&rec->pre_roll_drain_frames, 0,
                                  memory_order_release);
        }
        rec->was_recording = is_recording;

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

                /* --- Pre-roll fill — when armed but not yet recording, so
                 * the user's "rewind on record" request has data to prepend. */
                if (!is_recording && t->armed) {
                    preroll_write(&rec->pre_roll_rings[i], buf, got);
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
        preroll_init(&rec->pre_roll_rings[i]);
    }
    atomic_init(&rec->pre_roll_drain_frames, 0);
    rec->was_recording = false;

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
    for (int i = 0; i < WAVREC_MAX_CHANNELS; i++)
        preroll_destroy(&rec->pre_roll_rings[i]);
    free(rec);
    engine_set_record_engine(eng, NULL);
}

/* -------------------------------------------------------------------------
 * Pre-roll configuration (caller must have stopped the record engine).
 * ---------------------------------------------------------------------- */

void record_engine_configure_preroll(struct WavRecEngine *eng)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return;
    const EngSession *sess = engine_session(eng);
    float    secs   = (sess->pre_roll_seconds > 0.0f) ? sess->pre_roll_seconds : 0.0f;
    uint32_t cap    = (uint32_t)(sess->sample_rate * secs);
    /* Safety clamp — 10s at 192kHz = ~1.92M samples per track. */
    if (cap > 2u * 1024u * 1024u) cap = 2u * 1024u * 1024u;

    for (int i = 0; i < WAVREC_MAX_CHANNELS; i++)
        preroll_resize(&rec->pre_roll_rings[i], cap);

    atomic_store_explicit(&rec->pre_roll_drain_frames, 0, memory_order_release);
    rec->was_recording = false;
}

uint32_t record_engine_preroll_min_count(struct WavRecEngine *eng)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return 0;
    int n_tracks = engine_n_tracks(eng);
    uint32_t mn = UINT32_MAX;
    for (int i = 0; i < n_tracks; i++) {
        const Track *t = (const Track *)engine_get_track(eng, i);
        if (!t || !t->armed) continue;
        PreRollRing *r = &rec->pre_roll_rings[i];
        if (r->capacity == 0) continue;
        uint32_t c = atomic_load_explicit(&r->count, memory_order_acquire);
        if (c < mn) mn = c;
    }
    return (mn == UINT32_MAX) ? 0u : mn;
}

void record_engine_set_preroll_drain(struct WavRecEngine *eng, uint32_t frames)
{
    RecordEngine *rec = get_rec(eng);
    if (!rec) return;
    atomic_store_explicit(&rec->pre_roll_drain_frames, frames,
                          memory_order_release);
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
