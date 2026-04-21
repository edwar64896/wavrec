#include "audio_io.h"
#include "miniaudio.h"
#ifdef WAVREC_HAVE_ASIO
#include "miniaudio_asio.h"
#endif
#include "../engine.h"
#include "../track/track.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Temporary diagnostic macro — remove once crash is identified */
#define TRACE(fmt, ...) do { fprintf(stderr, "[audio_io] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static AudioIO *get_aio(struct WavRecEngine *eng)
{
    return (AudioIO *)engine_audio_io(eng);
}

/* -------------------------------------------------------------------------
 * Audio device callback — runs on miniaudio's real-time thread.
 *
 * Contract:
 *   - No allocation, no syscalls, no locks.
 *   - Only touches AudioRings and atomic frame counter.
 *   - Reads eng->tracks[] without a lock; brief races during track
 *     reconfiguration are acceptable (reconfiguration only happens idle).
 * ---------------------------------------------------------------------- */

static void audio_callback(ma_device *device,
                            void       *output,
                            const void *input,
                            ma_uint32   frame_count)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)device->pUserData;
    AudioIO             *aio = get_aio(eng);

    static uint64_t cb_count = 0;
    if ((cb_count++ % 100) == 0) {
        int n_tracks = engine_n_tracks(eng);
        int n_mon = 0, n_arm = 0;
        for (int i = 0; i < n_tracks; i++) {
            const Track *t = (const Track *)engine_get_track(eng, i);
            if (!t) continue;
            if (t->monitor) n_mon++;
            if (t->armed)   n_arm++;
        }
        fprintf(stderr, "[audio/cb] #%llu in=%p out=%p fc=%u n_tracks=%d mon=%d arm=%d\n",
                (unsigned long long)cb_count, input, output, frame_count,
                n_tracks, n_mon, n_arm);
        fflush(stderr);
    }

    const float *in_buf  = (const float *)input;
    float       *out_buf = (float *)output;
    uint32_t     n_in_ch  = device->capture.channels;
    uint32_t     n_out_ch = device->playback.channels;

    /* Clamp to pre-allocated scratch size. */
    uint32_t fc = frame_count;
    if (fc > AUDIO_IO_MAX_PERIOD_FRAMES) fc = AUDIO_IO_MAX_PERIOD_FRAMES;

    /* Zero monitor scratch — accumulated per-track below. */
    memset(aio->monitor_l, 0, fc * sizeof(float));
    memset(aio->monitor_r, 0, fc * sizeof(float));

    /* --- INPUT: de-interleave each track that needs input -------------- *
     * Armed tracks → record ring (raw, no gain).                          *
     * Monitored tracks → monitor scratch (with gain).                     *
     * Monitoring is independent of arm state.                             */
    if (in_buf) {
        int n_tracks = engine_n_tracks(eng);
        for (int i = 0; i < n_tracks; i++) {
            const Track *t = (const Track *)engine_get_track(eng, i);
            if (!t) continue;
            if (!t->armed && !t->monitor) continue;  /* nothing to do */

            uint8_t src_ch = t->hw_input;
            if (src_ch >= (uint8_t)n_in_ch) continue;

            /* Stride-copy channel src_ch from interleaved input. */
            for (uint32_t f = 0; f < fc; f++)
                aio->tmp[f] = in_buf[f * n_in_ch + src_ch];

            /* Record ring — raw, no gain.  Always fed when the track is
             * armed OR monitored (we already skipped the track otherwise).
             * The record engine decides what to do with the data based on
             * the engine state: disk ring when recording, pre-roll ring
             * when armed+idle, always meter + waveform. */
            uint32_t written = audio_ring_write(&aio->input_rings[i],
                                                aio->tmp, fc);
            if (written < fc && t->armed)
                atomic_fetch_add(&aio->xrun_count, 1);

            /* Monitor mix — gain applied here (centre pan = equal L/R) */
            if (t->monitor) {
                float g = track_gain_linear(t);
                for (uint32_t f = 0; f < fc; f++) {
                    float v = aio->tmp[f] * g;
                    aio->monitor_l[f] += v;
                    aio->monitor_r[f] += v;
                }
            }
        }
    }

    /* --- OUTPUT: sum playback + monitor, write to hardware ------------- */
    if (out_buf && n_out_ch >= 2) {
        uint32_t avail_l = audio_ring_count(&aio->playback_ring_l);
        bool     has_pb  = engine_is_playing(eng) &&
                           avail_l >= fc &&
                           audio_ring_count(&aio->playback_ring_r) >= fc;

        if (has_pb) {
            audio_ring_read(&aio->playback_ring_l, aio->tmp, fc);
            for (uint32_t f = 0; f < fc; f++)
                out_buf[f * n_out_ch] = aio->tmp[f] + aio->monitor_l[f];

            audio_ring_read(&aio->playback_ring_r, aio->tmp, fc);
            for (uint32_t f = 0; f < fc; f++)
                out_buf[f * n_out_ch + 1] = aio->tmp[f] + aio->monitor_r[f];
        } else {
            /* No playback (or underrun) — monitor only */
            for (uint32_t f = 0; f < fc; f++) {
                out_buf[f * n_out_ch]     = aio->monitor_l[f];
                out_buf[f * n_out_ch + 1] = aio->monitor_r[f];
            }
            if (engine_is_playing(eng))
                atomic_fetch_add(&aio->xrun_count, 1); /* playback underrun */
        }

        /* Zero any channels beyond stereo */
        for (uint32_t ch = 2; ch < n_out_ch; ch++)
            for (uint32_t f = 0; f < fc; f++)
                out_buf[f * n_out_ch + ch] = 0.0f;
    } else if (out_buf) {
        memset(out_buf, 0, fc * n_out_ch * sizeof(float));
    }

    /* --- Advance engine frame counter ---------------------------------- */
    engine_advance_frames(eng, fc);
}

/* -------------------------------------------------------------------------
 * Device notification callback — called when device state changes.
 * ---------------------------------------------------------------------- */

static void device_notification_callback(const ma_device_notification *notification)
{
    struct WavRecEngine *eng =
        (struct WavRecEngine *)notification->pDevice->pUserData;

    if (notification->type == ma_device_notification_type_stopped) {
        /* Device stopped unexpectedly (e.g. disconnected or rerouted). */
        if (engine_is_recording(eng) || engine_is_playing(eng)) {
            engine_emit(eng, EVT_ERROR,
                        "{\"code\":1003,\"severity\":\"fatal\","
                        "\"message\":\"Audio device stopped unexpectedly\"}");
        }
    }
}

/* -------------------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------------- */

bool audio_io_init(struct WavRecEngine *eng)
{
    AudioIO *aio = (AudioIO *)calloc(1, sizeof(AudioIO));
    if (!aio) return false;

    aio->eng = eng;
    atomic_init(&aio->xrun_count, 0u);

    /* Initialise all ring buffers */
    for (int i = 0; i < 128; i++)
        audio_ring_init(&aio->input_rings[i]);
    audio_ring_init(&aio->playback_ring_l);
    audio_ring_init(&aio->playback_ring_r);

    /* Allocate and initialise the primary context */
    ma_context *ctx = (ma_context *)malloc(sizeof(ma_context));
    if (!ctx) { free(aio); return false; }

    ma_context_config ctx_cfg = ma_context_config_init();
    ma_asio_context_config_init(&ctx_cfg);
    ma_backend asio_only[] = { ma_backend_custom };
    ma_result rc = ma_context_init(asio_only, 1, &ctx_cfg, ctx);
    if (rc != MA_SUCCESS) {
        free(ctx);
        free(aio);
        return false;
    }

    ma_device *dev = (ma_device *)malloc(sizeof(ma_device));
    if (!dev) {
        ma_context_uninit(ctx);
        free(ctx);
        free(aio);
        return false;
    }

    aio->context = ctx;
    aio->device  = dev;

    engine_set_audio_io(eng, aio);
    return true;
}

/* -------------------------------------------------------------------------
 * Device finder — used to resolve a device name to a ma_device_id.
 * ---------------------------------------------------------------------- */

typedef struct {
    const char    *target_name;
    ma_device_type target_type;
    ma_device_id   found_id;
    bool           found;
} DeviceFinder;

static ma_bool32 find_device_callback(ma_context           *context,
                                      ma_device_type        device_type,
                                      const ma_device_info *info,
                                      void                 *user_data)
{
    (void)context;
    DeviceFinder *f = (DeviceFinder *)user_data;
    if (device_type == f->target_type && strcmp(info->name, f->target_name) == 0) {
        f->found_id = info->id;
        f->found    = true;
    }
    return MA_TRUE;
}

/* Find a device by name; returns true and fills *out_id on success.
 * Returns false if name is empty or not found (caller should use default). */
static bool find_device_by_name(ma_context *ctx,
                                 ma_device_type type,
                                 const char *name,
                                 ma_device_id *out_id)
{
    if (!name || name[0] == '\0') return false;
    DeviceFinder f;
    f.target_name = name;
    f.target_type = type;
    f.found       = false;
    ma_context_enumerate_devices(ctx, find_device_callback, &f);
    if (f.found) *out_id = f.found_id;
    return f.found;
}

bool audio_io_open_device(struct WavRecEngine *eng)
{
    AudioIO   *aio = get_aio(eng);
    if (!aio || aio->device_open) return false;

    ma_context *ctx = (ma_context *)aio->context;
    ma_device  *dev = (ma_device  *)aio->device;

    uint32_t sample_rate   = engine_sample_rate(eng);
    uint32_t buffer_frames = engine_buffer_frames(eng);

    const EngSession *sess = engine_session(eng);

    TRACE("open_device: device='%s'", sess->device);

    if (sess->device[0] == '\0') {
        TRACE("no device selected yet — skipping open");
        return false;
    }

    /* Single ASIO driver handles both input and output. */
    ma_device_id cap_id, pb_id;
    bool has_cap = find_device_by_name(ctx, ma_device_type_capture,
                                       sess->device, &cap_id);
    bool has_pb  = find_device_by_name(ctx, ma_device_type_playback,
                                       sess->device, &pb_id);

    TRACE("device lookup: has_cap=%d has_pb=%d", has_cap, has_pb);

    if (!has_cap && !has_pb) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"code\":1004,\"severity\":\"fatal\","
                 "\"message\":\"ASIO driver '%s' not found\"}", sess->device);
        engine_emit(eng, EVT_ERROR, buf);
        return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.capture.pDeviceID  = has_cap ? &cap_id : NULL;
    cfg.capture.format     = ma_format_f32;
    cfg.capture.channels   = 0;          /* native channel count */
    cfg.capture.shareMode  = ma_share_mode_shared;
    cfg.playback.pDeviceID = has_pb ? &pb_id : NULL;
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;          /* stereo mix bus */
    cfg.playback.shareMode = ma_share_mode_shared;
    cfg.sampleRate             = sample_rate;
    cfg.periodSizeInFrames     = buffer_frames;
    cfg.periods                = 2;
    cfg.dataCallback           = audio_callback;
    cfg.notificationCallback   = device_notification_callback;
    cfg.pUserData              = eng;

    TRACE("calling ma_device_init (ASIO)");
    ma_result rc = ma_device_init(ctx, &cfg, dev);
    TRACE("ma_device_init returned %d", (int)rc);
    if (rc != MA_SUCCESS) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"code\":1001,\"severity\":\"fatal\","
                 "\"message\":\"ASIO device init failed (%d)\"}", (int)rc);
        engine_emit(eng, EVT_ERROR, buf);
        return false;
    }

    TRACE("calling ma_device_start");
    rc = ma_device_start(dev);
    TRACE("ma_device_start returned %d", (int)rc);
    if (rc != MA_SUCCESS) {
        ma_device_uninit(dev);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"code\":1002,\"severity\":\"fatal\","
                 "\"message\":\"ma_device_start failed (%d)\"}",
                 (int)rc);
        engine_emit(eng, EVT_ERROR, buf);
        return false;
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"level\":\"info\",\"message\":"
                 "\"Device open: cap=%uch pb=%uch %uHz [ASIO]\"}",
                 dev->capture.channels, dev->playback.channels,
                 dev->sampleRate);
        engine_emit(eng, EVT_LOG, buf);
    }

    aio->device_open = true;
    return true;
}

void audio_io_close_device(struct WavRecEngine *eng)
{
    AudioIO *aio = get_aio(eng);
    if (!aio || !aio->device_open) return;
    ma_device *dev = (ma_device *)aio->device;
    ma_device_stop(dev);
    ma_device_uninit(dev);
    aio->device_open = false;
}

void audio_io_shutdown(struct WavRecEngine *eng)
{
    AudioIO *aio = get_aio(eng);
    if (!aio) return;
    audio_io_close_device(eng);
    if (aio->context) {
        ma_context_uninit((ma_context *)aio->context);
        free(aio->context);
    }
    free(aio->device);
    free(aio);
    engine_set_audio_io(eng, NULL);
}

/* -------------------------------------------------------------------------
 * Device enumeration
 * ---------------------------------------------------------------------- */

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    first;
} EnumCtx;

static void append(EnumCtx *ctx, const char *s)
{
    size_t slen = strlen(s);
    while (ctx->len + slen + 1 > ctx->cap) {
        ctx->cap *= 2;
        ctx->buf  = (char *)realloc(ctx->buf, ctx->cap);
    }
    memcpy(ctx->buf + ctx->len, s, slen);
    ctx->len += slen;
    ctx->buf[ctx->len] = '\0';
}

static ma_bool32 enum_callback(ma_context              *context,
                                ma_device_type           device_type,
                                const ma_device_info    *info,
                                void                    *user_data)
{
    (void)context;
    EnumCtx *ctx = (EnumCtx *)user_data;

    /* ASIO drivers are enumerated twice (capture + playback) with the same
     * name.  Emit each driver only once — skip the playback duplicate. */
    if (device_type == ma_device_type_playback) return MA_TRUE;

    if (!ctx->first) append(ctx, ",");
    ctx->first = false;

    char safe_name[256];
    const char *src = info->name;
    char       *dst = safe_name;
    while (*src && (dst - safe_name) < 250) {
        if (*src == '"' || *src == '\\') *dst++ = '\\';
        *dst++ = *src++;
    }
    *dst = '\0';

    char entry[512];
    snprintf(entry, sizeof(entry),
             "{\"name\":\"%s\",\"is_default\":%s}",
             safe_name, info->isDefault ? "true" : "false");
    append(ctx, entry);
    return MA_TRUE;
}

char *audio_io_list_devices(struct WavRecEngine *eng)
{
    AudioIO *aio = get_aio(eng);
    if (!aio || !aio->context) return NULL;

    EnumCtx ectx;
    ectx.cap   = 8192;
    ectx.len   = 0;
    ectx.first = true;
    ectx.buf   = (char *)malloc(ectx.cap);
    if (!ectx.buf) return NULL;

    append(&ectx, "[");
    ma_context_enumerate_devices((ma_context *)aio->context,
                                 enum_callback, &ectx);
    append(&ectx, "]");

    return ectx.buf; /* caller frees */
}

/* -------------------------------------------------------------------------
 * Accessor
 * ---------------------------------------------------------------------- */

AudioIO *audio_io_get(struct WavRecEngine *eng)
{
    return get_aio(eng);
}
