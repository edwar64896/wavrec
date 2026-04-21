#include "playback_engine.h"
#include "audio_io.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../track/track.h"
#include "../metadata/wav_reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static PlaybackEngine *get_pe(struct WavRecEngine *eng)
{
    return (PlaybackEngine *)engine_playback(eng);
}

/* -------------------------------------------------------------------------
 * Format decode — read n frames from fp into dst (float32 mono output).
 * For multi-channel files, only channel 0 is extracted.
 * Returns frames actually decoded.
 * ---------------------------------------------------------------------- */

static uint32_t decode_frames(PlaybackFile *pf, float *dst,
                               uint8_t *raw, uint32_t n)
{
    uint8_t  bps   = wavrec_fmt_bytes(pf->fmt);     /* bytes per sample */
    uint16_t ch    = pf->channels ? pf->channels : 1;
    size_t   frame_bytes = (size_t)bps * ch;
    size_t   to_read     = (size_t)n * frame_bytes;

    size_t got = fread(raw, 1, to_read, pf->fp);
    uint32_t frames = (uint32_t)(got / frame_bytes);

    for (uint32_t f = 0; f < frames; f++) {
        /* Always decode channel 0 only (mono file in our case) */
        const uint8_t *p = raw + f * frame_bytes;

        switch (pf->fmt) {
        case WAVREC_FMT_PCM16: {
            int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            dst[f] = (float)s * (1.0f / 32768.0f);
            break;
        }
        case WAVREC_FMT_PCM24: {
            int32_t s = (int32_t)((uint32_t)p[0]
                      | ((uint32_t)p[1] << 8)
                      | ((uint32_t)p[2] << 16));
            if (s & 0x800000) s |= (int32_t)0xFF000000; /* sign extend */
            dst[f] = (float)s * (1.0f / 8388608.0f);
            break;
        }
        case WAVREC_FMT_PCM32: {
            int32_t s = (int32_t)((uint32_t)p[0]
                      | ((uint32_t)p[1] << 8)
                      | ((uint32_t)p[2] << 16)
                      | ((uint32_t)p[3] << 24));
            dst[f] = (float)s * (1.0f / 2147483648.0f);
            break;
        }
        case WAVREC_FMT_FLOAT32: {
            float v;
            memcpy(&v, p, 4);
            dst[f] = v;
            break;
        }
        default:
            dst[f] = 0.0f;
        }
    }
    return frames;
}

/* -------------------------------------------------------------------------
 * Playback thread
 *
 * Design:
 *   - Reads PLAYBACK_CHUNK_FRAMES from each active track file.
 *   - Applies gain from Track.gain_db (playback path — gain lives here,
 *     not in the record path).
 *   - Simple pan law: equal power, pan ∈ [-1, +1].
 *   - Mixes into L/R scratch buffers, writes to AudioIO output rings.
 *   - Backs off (1ms sleep) if the output ring is >75% full to avoid
 *     overrunning the audio callback.
 * ---------------------------------------------------------------------- */

#define OUTPUT_RING_HIGHWATER  (AUDIO_RING_FRAMES * 3 / 4)

static void pb_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    PlaybackEngine      *pe  = get_pe(eng);
    AudioIO             *aio = audio_io_get(eng);

    while (atomic_load_explicit(&pe->running, memory_order_acquire)) {

        if (!aio) { platform_sleep_ms(5); continue; }

        /* Back off if output rings are nearly full */
        uint32_t fill = audio_ring_count(&aio->playback_ring_l);
        if (fill >= OUTPUT_RING_HIGHWATER) {
            platform_sleep_ms(1);
            continue;
        }

        uint32_t n = PLAYBACK_CHUNK_FRAMES;
        bool any_active = false;

        /* Zero mix buffers */
        memset(pe->mix_l, 0, n * sizeof(float));
        memset(pe->mix_r, 0, n * sizeof(float));

        int n_tracks = engine_n_tracks(eng);
        for (int i = 0; i < n_tracks; i++) {
            if (!pe->track_active[i]) continue;
            PlaybackFile *pf = &pe->files[i];
            if (!pf->valid) continue;

            uint32_t got = decode_frames(pf, pe->decoded, pe->raw_buf, n);
            if (got == 0) {
                /* End of file */
                pf->valid = false;
                pe->track_active[i] = false;
                continue;
            }
            pf->read_frame += got;
            any_active = true;

            /* Gain and pan from track config */
            const Track *t = (const Track *)engine_get_track(eng, i);
            float gain = t ? track_gain_linear(t) : 1.0f;
            float pan  = 0.0f; /* TODO: expose pan per track */

            /* Equal-power pan: L = cos((pan+1)*π/4), R = sin((pan+1)*π/4) */
            float pan_l = cosf((pan + 1.0f) * 0.7853982f) * gain;
            float pan_r = sinf((pan + 1.0f) * 0.7853982f) * gain;

            for (uint32_t f = 0; f < got; f++) {
                pe->mix_l[f] += pe->decoded[f] * pan_l;
                pe->mix_r[f] += pe->decoded[f] * pan_r;
            }
        }

        if (!any_active) {
            /* All files exhausted — stop playback */
            atomic_store_explicit(&pe->running, 0, memory_order_release);
            break;
        }

        /* Write mix to output rings */
        audio_ring_write(&aio->playback_ring_l, pe->mix_l, n);
        audio_ring_write(&aio->playback_ring_r, pe->mix_r, n);

        /* Advance play head */
        uint64_t ph = atomic_load_explicit(&pe->play_head, memory_order_relaxed);
        atomic_store_explicit(&pe->play_head, ph + n, memory_order_release);
    }
}

/* -------------------------------------------------------------------------
 * File management
 * ---------------------------------------------------------------------- */

static void build_playback_path(char *buf, size_t len,
                                 const char *target, const char *scene,
                                 const char *take,   const char *label)
{
    /* Reuse the same sanitise-and-format logic as the disk writer */
    char safe[64];
    const char *s = label;
    char *d = safe;
    while (*s && (d - safe) < 63) {
        char c = *s++;
        *d++ = (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||
                c=='"'||c=='<'||c=='>'||c=='|') ? '_' : c;
    }
    *d = '\0';
    /* Matches disk_writer's build_folder_path: {folder}_{scene}_{take}.wav */
    snprintf(buf, len, "%s/%s_%s_%s.wav", target, safe, scene, take);
}

/* -------------------------------------------------------------------------
 * Init / start / stop / locate / shutdown
 * ---------------------------------------------------------------------- */

bool playback_engine_init(struct WavRecEngine *eng)
{
    PlaybackEngine *pe = (PlaybackEngine *)calloc(1, sizeof(PlaybackEngine));
    if (!pe) return false;
    pe->eng = eng;
    atomic_init(&pe->running,   0);
    atomic_init(&pe->play_head, 0u);
    engine_set_playback(eng, pe);
    return true;
}

void playback_engine_start(struct WavRecEngine *eng, uint64_t position_frames)
{
    PlaybackEngine   *pe   = get_pe(eng);
    if (!pe) return;
    if (atomic_load_explicit(&pe->running, memory_order_relaxed)) return;

    const EngSession *sess = engine_session(eng);
    int n_tracks = engine_n_tracks(eng);

    /* Use the first configured recording target as the playback source */
    const char *target = (sess->n_targets > 0) ? sess->record_targets[0] : ".";

    for (int i = 0; i < n_tracks; i++) {
        PlaybackFile     *pf = &pe->files[i];
        const Track      *t  = (const Track *)engine_get_track(eng, i);
        pf->valid = false;
        pe->track_active[i] = false;

        if (!t) continue;

        char path[PLAYBACK_MAX_PATH];
        build_playback_path(path, sizeof(path),
                            target, sess->scene, sess->take, t->label);

        pf->fp = fopen(path, "rb");
        if (!pf->fp) continue;

        WavInfo info;
        if (!wav_read_info(pf->fp, &info)) {
            fclose(pf->fp); pf->fp = NULL; continue;
        }

        pf->fmt          = info.fmt;
        pf->channels     = info.channels;
        pf->sample_rate  = info.sample_rate;
        pf->data_offset  = info.data_offset;
        pf->total_frames = info.data_frames;
        pf->read_frame   = 0;

        /* Seek to the requested start position */
        if (position_frames > 0 && position_frames < pf->total_frames) {
            uint8_t bps   = wavrec_fmt_bytes(pf->fmt);
            long seek_off = pf->data_offset
                          + (long)(position_frames * pf->channels * bps);
            fseek(pf->fp, seek_off, SEEK_SET);
            pf->read_frame = position_frames;
        }

        pf->valid          = true;
        pe->track_active[i] = true;
    }

    atomic_store_explicit(&pe->play_head, position_frames, memory_order_release);
    atomic_store_explicit(&pe->running,   1,               memory_order_release);

    pe->thread = platform_thread_create(pb_thread, eng,
                                        PLATFORM_PRIO_HIGH, "playback");
}

void playback_engine_stop(struct WavRecEngine *eng)
{
    PlaybackEngine *pe = get_pe(eng);
    if (!pe || !atomic_load_explicit(&pe->running, memory_order_relaxed)) return;

    atomic_store_explicit(&pe->running, 0, memory_order_release);
    platform_thread_join(pe->thread);
    platform_thread_destroy(pe->thread);
    pe->thread = NULL;

    /* Close all open file handles */
    for (int i = 0; i < 128; i++) {
        if (pe->files[i].fp) {
            fclose(pe->files[i].fp);
            pe->files[i].fp    = NULL;
            pe->files[i].valid = false;
        }
        pe->track_active[i] = false;
    }
}

void playback_engine_locate(struct WavRecEngine *eng, uint64_t position_frames)
{
    PlaybackEngine *pe = get_pe(eng);
    if (!pe) return;
    bool was_playing = atomic_load_explicit(&pe->running, memory_order_relaxed);
    if (was_playing) playback_engine_stop(eng);
    if (was_playing) playback_engine_start(eng, position_frames);
    else atomic_store_explicit(&pe->play_head, position_frames, memory_order_release);
}

uint64_t playback_engine_play_head(const struct WavRecEngine *eng)
{
    const PlaybackEngine *pe = (const PlaybackEngine *)engine_playback(
                                    (struct WavRecEngine *)eng);
    return pe ? atomic_load_explicit(&pe->play_head, memory_order_acquire) : 0;
}

void playback_engine_shutdown(struct WavRecEngine *eng)
{
    PlaybackEngine *pe = get_pe(eng);
    if (!pe) return;
    playback_engine_stop(eng);
    free(pe);
    engine_set_playback(eng, NULL);
}
