#include "disk_writer.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../audio/record_engine.h"
#include "../metadata/bwf.h"
#include "../track/track.h"
#include "../ipc/ipc_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static DiskWriter *get_dw(struct WavRecEngine *eng)
{
    return (DiskWriter *)engine_disk_writer(eng);
}

/* Convert n float32 samples to the target WavSampleFormat.
 * Returns bytes written to dst.  dst must have capacity n * 4 bytes. */
static size_t convert_samples(const float *src, uint8_t *dst,
                               uint32_t n, WavSampleFormat fmt)
{
    uint32_t i;
    switch (fmt) {

    case WAVREC_FMT_PCM16:
        for (i = 0; i < n; i++) {
            float f = src[i] > 1.f ? 1.f : src[i] < -1.f ? -1.f : src[i];
            int16_t s = (int16_t)(f * 32767.f);
            dst[i*2+0] = (uint8_t)( s       & 0xFF);
            dst[i*2+1] = (uint8_t)((s >> 8) & 0xFF);
        }
        return n * 2;

    default:
    case WAVREC_FMT_PCM24:
        for (i = 0; i < n; i++) {
            float f = src[i] > 1.f ? 1.f : src[i] < -1.f ? -1.f : src[i];
            int32_t s = (int32_t)(f * 8388607.f);
            dst[i*3+0] = (uint8_t)( s        & 0xFF);
            dst[i*3+1] = (uint8_t)((s >>  8) & 0xFF);
            dst[i*3+2] = (uint8_t)((s >> 16) & 0xFF);
        }
        return n * 3;

    case WAVREC_FMT_PCM32:
        for (i = 0; i < n; i++) {
            float f = src[i] > 1.f ? 1.f : src[i] < -1.f ? -1.f : src[i];
            int32_t s = (int32_t)(f * 2147483647.f);
            dst[i*4+0] = (uint8_t)( s        & 0xFF);
            dst[i*4+1] = (uint8_t)((s >>  8) & 0xFF);
            dst[i*4+2] = (uint8_t)((s >> 16) & 0xFF);
            dst[i*4+3] = (uint8_t)((s >> 24) & 0xFF);
        }
        return n * 4;

    case WAVREC_FMT_FLOAT32:
        /* Passthrough — raw IEEE 754 bytes, no clamping or scaling */
        memcpy(dst, src, n * sizeof(float));
        return n * sizeof(float);
    }
}

/* Build the file path for a track/target combination. */
static void build_path(char *buf, size_t buf_len,
                       const char *target, const char *scene,
                       const char *take,   const char *label)
{
    /* Sanitise label — replace characters unsafe for filenames. */
    char safe_label[TRACK_LABEL_MAX];
    const char *src = label;
    char       *dst = safe_label;
    while (*src && (dst - safe_label) < TRACK_LABEL_MAX - 1) {
        char c = *src++;
        *dst++ = (c == '/' || c == '\\' || c == ':' || c == '*' ||
                  c == '?' || c == '"' || c == '<' || c == '>' ||
                  c == '|') ? '_' : c;
    }
    *dst = '\0';

    snprintf(buf, buf_len, "%s/%s_%s_%s.wav",
             target, scene, take, safe_label);
}

/* Emit EVT_ERROR warning (non-fatal — recording continues on other targets). */
static void warn_target(struct WavRecEngine *eng, const char *target,
                        const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"code\":2001,\"severity\":\"warning\","
             "\"message\":\"%s: %s\"}",
             msg, target);
    engine_emit(eng, EVT_ERROR, buf);
}

/* -------------------------------------------------------------------------
 * Disk status emission (~1 Hz)
 * ---------------------------------------------------------------------- */

static void emit_disk_status(struct WavRecEngine *eng, DiskWriter *dw)
{
    uint64_t now_ms = platform_time_ms();
    uint64_t elapsed = now_ms - dw->last_status_ms;
    if (elapsed < 1000) return;

    uint64_t write_rate = (elapsed > 0)
        ? (dw->bytes_written_interval * 1000) / elapsed
        : 0;

    const EngSession *sess = engine_session(eng);

    /* Build targets JSON array */
    char targets_json[1024] = "[";
    int  n_targets = sess->n_targets;

    for (int j = 0; j < n_targets; j++) {
        const char *tp = sess->record_targets[j];
        int64_t free_b = platform_free_bytes(tp);

        /* Check if any track still has this target valid */
        bool target_ok = false;
        for (int i = 0; i < WAVREC_MAX_CHANNELS; i++) {
            if (dw->track_active[i] && dw->files[i][j].valid) {
                target_ok = true;
                break;
            }
        }

        char entry[256];
        snprintf(entry, sizeof(entry),
                 "%s{\"path\":\"%s\",\"free_bytes\":%lld"
                 ",\"write_rate_bps\":%llu,\"ok\":%s}",
                 j > 0 ? "," : "",
                 tp, (long long)free_b, (unsigned long long)write_rate,
                 target_ok ? "true" : "false");
        strncat(targets_json, entry,
                sizeof(targets_json) - strlen(targets_json) - 1);
    }
    strncat(targets_json, "]",
            sizeof(targets_json) - strlen(targets_json) - 1);

    /* Estimate remaining recording time from write rate and free space */
    uint64_t remaining_secs = 0;
    if (write_rate > 0 && n_targets > 0) {
        int64_t min_free = (int64_t)0x7FFFFFFFFFFFFFFF;
        for (int j = 0; j < n_targets; j++) {
            int64_t f = platform_free_bytes(sess->record_targets[j]);
            if (f >= 0 && f < min_free) min_free = f;
        }
        if (min_free != (int64_t)0x7FFFFFFFFFFFFFFF)
            remaining_secs = (uint64_t)min_free / write_rate;
    }

    char payload[2048];
    snprintf(payload, sizeof(payload),
             "{\"targets\":%s,\"estimated_remaining_seconds\":%llu}",
             targets_json, (unsigned long long)remaining_secs);
    engine_emit(eng, EVT_DISK_STATUS, payload);

    dw->bytes_written_interval = 0;
    dw->last_status_ms         = now_ms;
}

/* -------------------------------------------------------------------------
 * Writer thread
 * ---------------------------------------------------------------------- */

static void writer_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    DiskWriter          *dw  = get_dw(eng);

    for (;;) {
        bool stop     = !atomic_load_explicit(&dw->running,
                                              memory_order_acquire);
        bool did_work = false;

        const EngSession *sess = engine_session(eng);
        int n_tracks         = engine_n_tracks(eng);
        int n_targets        = sess->n_targets;
        WavSampleFormat fmt  = sess->sample_format;

        for (int i = 0; i < n_tracks; i++) {
            if (!dw->track_active[i]) continue;

            AudioRing *ring = record_engine_disk_ring(eng, i);
            if (!ring) continue;

            /* Drain ring in chunks */
            for (;;) {
                uint32_t got = audio_ring_read(ring, dw->float_buf,
                                               DISK_CHUNK_FRAMES);
                if (got == 0) break;
                did_work = true;

                /* Convert float32 to target format once, write to all targets */
                size_t byte_count = convert_samples(dw->float_buf,
                                                    dw->pcm_buf, got, fmt);

                for (int j = 0; j < n_targets; j++) {
                    TrackFile *tf = &dw->files[i][j];
                    if (!tf->valid) continue;

                    size_t written = fwrite(dw->pcm_buf, 1, byte_count, tf->fp);
                    if (written != byte_count) {
                        /* Write error — mark target invalid for this track */
                        tf->valid = false;
                        warn_target(eng, sess->record_targets[j],
                                    "Write error — target offline");
                    } else {
                        tf->frames_written       += got;
                        dw->bytes_written_interval += written;
                    }
                }
            }
        }

        emit_disk_status(eng, dw);

        if (stop && !did_work) break; /* fully drained, safe to exit */
        if (!did_work) platform_sleep_ms(1);
    }

    /* Final flush of stdio buffers */
    int n_tracks  = engine_n_tracks(eng);
    int n_targets = engine_session(eng)->n_targets;
    for (int i = 0; i < n_tracks; i++)
        for (int j = 0; j < n_targets; j++)
            if (dw->files[i][j].valid && dw->files[i][j].fp)
                fflush(dw->files[i][j].fp);
}

/* -------------------------------------------------------------------------
 * Open / close
 * ---------------------------------------------------------------------- */

bool disk_writer_open(struct WavRecEngine *eng)
{
    DiskWriter *dw = get_dw(eng);
    if (!dw) return false;

    const EngSession *sess = engine_session(eng);
    int n_tracks  = engine_n_tracks(eng);
    int n_targets = sess->n_targets;

    /* Need at least one target */
    if (n_targets == 0) {
        engine_emit(eng, EVT_ERROR,
                    "{\"code\":2002,\"severity\":\"fatal\","
                    "\"message\":\"No recording targets configured\"}");
        return false;
    }

    BextChunk bext;
    bool any_open = false;

    for (int i = 0; i < n_tracks; i++) {
        const Track *t = (const Track *)engine_get_track(eng, i);
        if (!t || !t->armed) continue;

        dw->track_active[i] = false;

        for (int j = 0; j < n_targets; j++) {
            TrackFile *tf = &dw->files[i][j];
            tf->valid          = false;
            tf->fp             = NULL;
            tf->frames_written = 0;

            const char *target = sess->record_targets[j];

            /* Ensure target directory exists */
            platform_mkdir_p(target);

            char path[WAVREC_MAX_TARGET_PATH + 128];
            build_path(path, sizeof(path),
                       target, sess->scene, sess->take, t->label);

            tf->fp = fopen(path, "wb");
            if (!tf->fp) {
                warn_target(eng, target, "Cannot open file for writing");
                continue;
            }

            /* 64 KB stdio buffer — reduces write syscall frequency */
            setvbuf(tf->fp, NULL, _IOFBF, DISK_WRITE_BUF_SIZE);

            /* Build and write BWF header */
            char desc[280];
            snprintf(desc, sizeof(desc), "%s / %s / %s",
                     sess->scene, sess->take, t->label);

            bwf_build_bext(&bext, desc,
                           sess->sample_rate,
                           sess->sample_format,
                           1, /* mono per track */
                           engine_tc(eng),
                           engine_frame_counter(eng));

            long offset = bwf_write_header(tf->fp, &bext,
                                           sess->sample_rate,
                                           sess->sample_format,
                                           1 /* mono */);
            if (offset < 0) {
                fclose(tf->fp);
                tf->fp = NULL;
                warn_target(eng, target, "Failed to write BWF header");
                continue;
            }

            tf->data_size_offset = offset;
            tf->valid            = true;
            dw->track_active[i]  = true;
            any_open             = true;
        }

        if (!dw->track_active[i]) {
            /* All targets failed for this track */
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"code\":2003,\"severity\":\"fatal\","
                     "\"message\":\"No valid target for track %d\"}",
                     i);
            engine_emit(eng, EVT_ERROR, msg);
        }
    }

    if (!any_open) return false;

    /* Start writer thread */
    dw->last_status_ms         = platform_time_ms();
    dw->bytes_written_interval = 0;
    atomic_store_explicit(&dw->running,       1, memory_order_release);
    atomic_store_explicit(&dw->flush_and_stop, 0, memory_order_release);

    dw->thread = platform_thread_create(writer_thread, eng,
                                        PLATFORM_PRIO_LOW,
                                        "disk_writer");
    return true;
}

void disk_writer_close(struct WavRecEngine *eng)
{
    DiskWriter *dw = get_dw(eng);
    if (!dw) return;
    if (!atomic_load_explicit(&dw->running, memory_order_relaxed)) return;

    /* Signal thread: drain remaining ring data then stop */
    atomic_store_explicit(&dw->running, 0, memory_order_release);
    platform_thread_join(dw->thread);
    platform_thread_destroy(dw->thread);
    dw->thread = NULL;

    /* Finalise all files: patch RIFF/data sizes, close */
    const EngSession *sess = engine_session(eng);
    int n_tracks  = engine_n_tracks(eng);
    int n_targets = sess->n_targets;

    for (int i = 0; i < n_tracks; i++) {
        if (!dw->track_active[i]) continue;
        for (int j = 0; j < n_targets; j++) {
            TrackFile *tf = &dw->files[i][j];
            if (!tf->fp) continue;

            if (tf->valid) {
                bwf_finalise(tf->fp,
                             tf->data_size_offset,
                             tf->frames_written,
                             1, /* mono */
                             wavrec_fmt_bit_depth(sess->sample_format));
            }
            fclose(tf->fp);
            tf->fp    = NULL;
            tf->valid = false;
        }
        dw->track_active[i] = false;
    }
}

/* -------------------------------------------------------------------------
 * Init / shutdown / free_bytes
 * ---------------------------------------------------------------------- */

bool disk_writer_init(struct WavRecEngine *eng)
{
    DiskWriter *dw = (DiskWriter *)calloc(1, sizeof(DiskWriter));
    if (!dw) return false;

    dw->eng = eng;
    atomic_init(&dw->running,        0);
    atomic_init(&dw->flush_and_stop, 0);

    engine_set_disk_writer(eng, dw);
    return true;
}

void disk_writer_shutdown(struct WavRecEngine *eng)
{
    DiskWriter *dw = get_dw(eng);
    if (!dw) return;
    disk_writer_close(eng); /* no-op if already closed */
    free(dw);
    engine_set_disk_writer(eng, NULL);
}

int64_t disk_writer_free_bytes(struct WavRecEngine *eng)
{
    const EngSession *sess = engine_session(eng);
    int64_t min_free = -1;
    int n_targets = sess->n_targets;
    for (int j = 0; j < n_targets; j++) {
        int64_t f = platform_free_bytes(sess->record_targets[j]);
        if (f < 0) continue;
        if (min_free < 0 || f < min_free) min_free = f;
    }
    return min_free;
}
