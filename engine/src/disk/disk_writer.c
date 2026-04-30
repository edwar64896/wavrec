#include "disk_writer.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../audio/record_engine.h"
#include "../metadata/bwf.h"
#include "../metadata/ixml.h"
#include "../track/track.h"
#include "../timecode/timecode.h"
#include "../ipc/ipc_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef NDEBUG
#  define DBGLOG(...) ((void)0)
#else
#  define DBGLOG(...) fprintf(stderr, __VA_ARGS__)
#endif


/* Forward decls — rotation helpers live further down but are called by
 * writer_thread higher up. */
static void rotate_files_now(DiskWriter *dw, struct WavRecEngine *eng,
                             uint64_t target_frame);
static bool open_all_folder_files (DiskWriter *dw, struct WavRecEngine *eng);
static void close_all_folder_files(DiskWriter *dw, struct WavRecEngine *eng);

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
        memcpy(dst, src, n * sizeof(float));
        return n * sizeof(float);
    }
}

/* Sanitise a folder name for use in a filename. */
static void sanitise_name(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (*src && i + 1 < dst_len) {
        char c = *src++;
        dst[i++] = (c == '/' || c == '\\' || c == ':' || c == '*' ||
                    c == '?' || c == '"' || c == '<' || c == '>' ||
                    c == '|') ? '_' : c;
    }
    dst[i] = '\0';
}

/* folderName_Sxx_Tyyy.wav under `target` — e.g. Drums_01_003.wav */
static void build_folder_path(char *buf, size_t buf_len,
                              const char *target, const char *scene,
                              const char *take, const char *folder_name)
{
    char safe[WAVREC_MAX_FOLDER_NAME];
    sanitise_name(folder_name, safe, sizeof(safe));
    snprintf(buf, buf_len, "%s/%s_%s_%s.wav",
             target, safe[0] ? safe : "Main", scene, take);
}

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
 * Disk status emission (~1 Hz) — now reports per-folder target health.
 * For the UI compatibility window (still uses the flat targets list in
 * EVT_DISK_STATUS), we emit the UNION of all folder targets.
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
    int n_targets = sess->n_targets;

    char targets_json[2048] = "[";
    for (int j = 0; j < n_targets; j++) {
        const char *tp = sess->record_targets[j];
        int64_t free_b = platform_free_bytes(tp);

        /* Any folder still writing to this target path? */
        bool target_ok = false;
        for (int fi = 0; fi < WAVREC_MAX_FOLDERS && !target_ok; fi++) {
            const EngFolder *f = &sess->folders[fi];
            for (int k = 0; k < f->n_targets; k++) {
                if (strcmp(f->targets[k], tp) != 0) continue;
                /* Find the writer-side target slot for this folder by index. */
                if (dw->folders[fi].files[k].valid) {
                    target_ok = true; break;
                }
            }
        }

        char entry[384];
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

    char payload[3072];
    snprintf(payload, sizeof(payload),
             "{\"targets\":%s,\"estimated_remaining_seconds\":%llu}",
             targets_json, (unsigned long long)remaining_secs);
    engine_emit(eng, EVT_DISK_STATUS, payload);

    dw->bytes_written_interval = 0;
    dw->last_status_ms         = now_ms;
}

/* -------------------------------------------------------------------------
 * Periodic flush + commit event emission.
 *
 * Every FLUSH_INTERVAL_MS (or on close), for each open (folder × target)
 * file we:
 *   1. fflush(fp)         — stdio buffer → OS page cache
 *   2. platform_fsync(fp) — OS page cache → physical storage (data durable)
 *   3. bwf_update_sizes() — rewrite RIFF/data size fields; promote to RF64
 *                           and ds64 when data_bytes > 4 GB.  In-place
 *                           seek+write; does NOT touch any audio bytes.
 *   4. fflush + fsync     — size updates durable
 *   5. Record the cumulative bytes-committed high-water mark.
 *
 * Emits a single batched EVT_TARGET_COMMIT carrying all (folder, target,
 * frames, bytes) tuples so the UI can light per-target disk indicators.
 * ---------------------------------------------------------------------- */

#define FLUSH_INTERVAL_MS 2000

static void flush_all_files(DiskWriter *dw, struct WavRecEngine *eng)
{
    const EngSession *sess = engine_session(eng);
    uint8_t           bit_depth = wavrec_fmt_bit_depth(sess->sample_format);

    char commits[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(commits + pos, sizeof(commits) - pos, "[");
    int n_commits = 0;

    for (int fi = 0; fi < sess->n_folders; fi++) {
        FolderRecord *fr = &dw->folders[fi];
        if (!fr->any_valid) continue;

        for (int j = 0; j < WAVREC_MAX_TARGETS; j++) {
            FolderFile *ff = &fr->files[j];
            if (!ff->valid || !ff->fp) continue;

            /* Flush audio data before rewriting the header so the on-disk
             * size claim never exceeds the on-disk data length. */
            if (fflush(ff->fp) != 0 || !platform_fsync(ff->fp)) {
                ff->valid = false;
                warn_target(eng, sess->folders[fi].targets[j],
                            "Flush failed — target offline");
                continue;
            }

            /* Rewrite headers in place.  RF64 promotion happens transparently
             * inside bwf_update_sizes when data_bytes crosses 4 GB. */
            if (!bwf_update_sizes(ff->fp,
                                  ff->junk_offset,
                                  ff->data_size_offset,
                                  ff->frames_written,
                                  (uint8_t)fr->n_channels,
                                  bit_depth)) {
                ff->valid = false;
                warn_target(eng, sess->folders[fi].targets[j],
                            "Header update failed");
                continue;
            }
            fflush(ff->fp);
            platform_fsync(ff->fp);

            uint64_t data_bytes = ff->frames_written *
                                  (uint64_t)fr->n_channels *
                                  (uint64_t)(bit_depth / 8);
            ff->bytes_at_last_flush = data_bytes;

            /* Append to commits JSON. */
            int wrote = snprintf(commits + pos, sizeof(commits) - pos,
                                 "%s{\"folder_id\":%d,\"target_idx\":%d,"
                                 "\"frames\":%llu,\"bytes\":%llu}",
                                 n_commits > 0 ? "," : "",
                                 fi, j,
                                 (unsigned long long)ff->frames_written,
                                 (unsigned long long)data_bytes);
            if (wrote > 0 && (size_t)wrote < sizeof(commits) - pos) {
                pos += (size_t)wrote;
                n_commits++;
            }
        }
    }

    pos += (size_t)snprintf(commits + pos, sizeof(commits) - pos, "]");

    if (n_commits > 0) {
        char payload[5120];
        snprintf(payload, sizeof(payload), "{\"commits\":%s}", commits);
        engine_emit(eng, EVT_TARGET_COMMIT, payload);
    }
}

/* -------------------------------------------------------------------------
 * Writer thread
 *
 * For each active folder, drain its tracks' rings in lockstep.  We read
 * the minimum available across channels, interleave into one buffer, then
 * write that poly chunk to every valid (folder, target) file.
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
        WavSampleFormat   fmt  = sess->sample_format;

        /* --- Rotation: compute per-folder cap if a rotation is pending --- */
        bool     rotate_pending = atomic_load_explicit(
                                      &dw->rotate_pending, memory_order_acquire);
        uint64_t target_frame   = rotate_pending
                                  ? atomic_load_explicit(
                                      &dw->rotate_at_engine_frame,
                                      memory_order_acquire)
                                  : 0;
        uint64_t target_sample  = 0; /* in file-sample space (== target_frame - effective_origin) */
        if (rotate_pending && target_frame > dw->effective_origin)
            target_sample = target_frame - dw->effective_origin;

        for (int fi = 0; fi < sess->n_folders; fi++) {
            FolderRecord *fr = &dw->folders[fi];
            if (!fr->any_valid || fr->n_channels <= 0) continue;

            /* Find min available frames across this folder's tracks. */
            uint32_t min_avail = DISK_CHUNK_FRAMES;
            for (int c = 0; c < fr->n_channels; c++) {
                AudioRing *ring = record_engine_disk_ring(eng, fr->track_ids[c]);
                uint32_t avail = ring ? audio_ring_count(ring) : 0;
                if (avail < min_avail) min_avail = avail;
                if (min_avail == 0) break;
            }
            if (min_avail == 0) continue;

            /* Drain min_avail frames from each channel into the interleave
             * buffer.  interleave_buf layout: [f0c0 f0c1 ... f0cN f1c0 ...]. */
            uint32_t chunk = min_avail;
            const int N = fr->n_channels;

            /* --- Rotation cap: never overshoot the target_sample ------- *
             * Pick any valid file's frames_written as the folder's write
             * position (they're all equal — written in lockstep). */
            if (rotate_pending) {
                uint64_t fw = 0;
                for (int j = 0; j < WAVREC_MAX_TARGETS; j++) {
                    if (fr->files[j].valid) { fw = fr->files[j].frames_written; break; }
                }
                if (fw >= target_sample) {
                    /* Already at or past target — skip this folder until rotate. */
                    continue;
                }
                uint64_t remaining = target_sample - fw;
                if (chunk > remaining) chunk = (uint32_t)remaining;
            }

            for (int c = 0; c < N; c++) {
                AudioRing *ring = record_engine_disk_ring(eng, fr->track_ids[c]);
                uint32_t got = audio_ring_read(ring, dw->float_buf, chunk);
                /* Ring was guaranteed >= chunk by min_avail; if short, zero the rest. */
                for (uint32_t f = 0; f < chunk; f++)
                    dw->interleave_buf[f * N + c] = (f < got) ? dw->float_buf[f] : 0.f;
            }

            /* Convert chunk*N float samples to PCM once. */
            size_t byte_count = convert_samples(dw->interleave_buf,
                                                dw->pcm_buf,
                                                chunk * N, fmt);

            /* Write to every valid target for this folder. */
            const EngFolder *f_cfg = &sess->folders[fi];
            for (int j = 0; j < f_cfg->n_targets; j++) {
                FolderFile *ff = &fr->files[j];
                if (!ff->valid) continue;

                size_t written = fwrite(dw->pcm_buf, 1, byte_count, ff->fp);
                if (written != byte_count) {
                    ff->valid = false;
                    warn_target(eng, f_cfg->targets[j],
                                "Write error — target offline");
                } else {
                    ff->frames_written        += chunk;
                    dw->bytes_written_interval += written;
                }
            }
            did_work = true;
        }

        /* --- Rotation trigger: once every valid folder has written up to
         * target_sample, perform the rotation.  Runs after the write loop
         * so no folder is "half-written" when we close files. */
        if (rotate_pending) {
            bool all_at_target = true;
            for (int fi = 0; fi < sess->n_folders; fi++) {
                FolderRecord *fr = &dw->folders[fi];
                if (!fr->any_valid || fr->n_channels <= 0) continue;
                uint64_t fw = 0;
                for (int j = 0; j < WAVREC_MAX_TARGETS; j++) {
                    if (fr->files[j].valid) { fw = fr->files[j].frames_written; break; }
                }
                if (fw < target_sample) { all_at_target = false; break; }
            }
            if (all_at_target) {
                rotate_files_now(dw, eng, target_frame);
                atomic_store_explicit(&dw->rotate_pending, 0,
                                      memory_order_release);
                atomic_store_explicit(&dw->rotate_at_engine_frame, 0,
                                      memory_order_release);
                /* Emit EVT_TARGET_COMMIT for the new file's first frame (0).
                 * The UI resets the displayed bytes at this point. */
                did_work = true;
            }
        }

        emit_disk_status(eng, dw);

        /* Periodic RIFF-header fix-up + fsync, once every FLUSH_INTERVAL_MS. */
        uint64_t now_ms = platform_time_ms();
        if (now_ms - dw->last_flush_ms >= FLUSH_INTERVAL_MS) {
            flush_all_files(dw, eng);
            dw->last_flush_ms = now_ms;
        }

        if (stop && !did_work) break;
        if (!did_work) platform_sleep_ms(1);
    }

    /* Final flush of everything still buffered — makes the UI see a last
     * commit event with the true total before bwf_finalise runs on close. */
    flush_all_files(dw, eng);
}

/* -------------------------------------------------------------------------
 * File-open helper — opens all (folder × target) poly WAVs for the current
 * session state.  Called both from disk_writer_open() at record start and
 * from the rotation path inside writer_thread().
 *
 * Does NOT start/stop any threads.  Clears dw->folders and rebuilds
 * fr->track_ids from the current armed set, so a rotation that ran
 * engine_apply_pre_armed() just before will pick up newly-armed tracks
 * and drop newly-disarmed ones.
 * ---------------------------------------------------------------------- */
static bool open_all_folder_files(DiskWriter *dw, struct WavRecEngine *eng)
{
    memset(dw->folders, 0, sizeof(dw->folders));

    const EngSession *sess = engine_session(eng);
    if (sess->n_folders == 0) {
        engine_emit(eng, EVT_ERROR,
                    "{\"code\":2002,\"severity\":\"fatal\","
                    "\"message\":\"No folders configured\"}");
        return false;
    }

    BextChunk bext;
    bool any_open = false;

    for (int fi = 0; fi < sess->n_folders; fi++) {
        const EngFolder *f_cfg = &sess->folders[fi];
        FolderRecord    *fr    = &dw->folders[fi];
        fr->n_channels = 0;
        fr->any_valid  = false;

        /* Snapshot armed tracks in this folder, in the folder's declared
         * track_ids order.  Disarmed tracks are skipped entirely. */
        for (int ti = 0; ti < f_cfg->n_tracks; ti++) {
            int tid = f_cfg->track_ids[ti];
            if (tid < 0 || tid >= engine_n_tracks(eng)) continue;
            const Track *t = (const Track *)engine_get_track(eng, tid);
            if (!t || !t->armed) continue;
            fr->track_ids[fr->n_channels++] = tid;
        }
        if (fr->n_channels == 0) continue;   /* Nothing armed in this folder. */

        if (f_cfg->n_targets == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "{\"code\":2004,\"severity\":\"warning\","
                     "\"message\":\"Folder '%s' has no targets configured\"}",
                     f_cfg->name);
            engine_emit(eng, EVT_ERROR, msg);
            continue;
        }

        for (int j = 0; j < f_cfg->n_targets; j++) {
            FolderFile *ff = &fr->files[j];
            ff->valid = false;
            ff->fp    = NULL;
            ff->frames_written = 0;

            const char *target = f_cfg->targets[j];
            platform_mkdir_p(target);

            char path[WAVREC_MAX_TARGET_PATH + 128];
            build_folder_path(path, sizeof(path),
                              target, sess->scene, sess->take, f_cfg->name);

            ff->fp = fopen(path, "wb");
            if (!ff->fp) {
                warn_target(eng, target, "Cannot open file for writing");
                continue;
            }
            setvbuf(ff->fp, NULL, _IOFBF, DISK_WRITE_BUF_SIZE);

            char desc[280];
            snprintf(desc, sizeof(desc), "%s / %s / %s (poly)",
                     sess->scene, sess->take, f_cfg->name);

            bwf_build_bext(&bext, desc,
                           sess->sample_rate,
                           sess->sample_format,
                           (uint8_t)fr->n_channels,
                           engine_tc(eng),
                           engine_frame_counter(eng));
            /* One-off diagnostic so we can verify the time_reference being
             * written on each open (including punched files).  Emit only for
             * j == 0 to avoid N duplicate entries per open. */
            if (j == 0) {
                char bxlog[192];
                snprintf(bxlog, sizeof(bxlog),
                         "{\"level\":\"info\",\"message\":"
                         "\"BEXT folder='%s' take=%s time_ref=%llu\"}",
                         f_cfg->name, sess->take,
                         (unsigned long long)bext.time_reference);
                engine_emit(eng, EVT_LOG, bxlog);
            }

            /* Build iXML TRACK_LIST entries from the per-folder channel
             * snapshot so channel 1..N in the poly WAV matches names 1..N.
             * Memory is stack-allocated and only referenced during
             * ixml_render(), so it stays alive long enough. */
            const char *ixml_names[WAVREC_MAX_CHANNELS];
            uint8_t     ixml_inputs[WAVREC_MAX_CHANNELS];
            for (int c = 0; c < fr->n_channels; c++) {
                const Track *t = (const Track *)
                                 engine_get_track(eng, fr->track_ids[c]);
                ixml_names [c] = t ? t->label     : "";
                ixml_inputs[c] = t ? t->hw_input  : 0;
            }
            IxmlParams ip = {0};
            ip.project          = "";
            ip.scene            = sess->scene;
            ip.take             = sess->take;
            ip.tape             = sess->tape;
            ip.n_tracks         = fr->n_channels;
            ip.track_names      = ixml_names;
            ip.track_hw_inputs  = ixml_inputs;
            ip.tc               = engine_tc(eng);
            ip.sample_rate      = sess->sample_rate;
            ip.bit_depth        = wavrec_fmt_bit_depth(sess->sample_format);
            char *ixml_text = ixml_render(&ip);

            long junk_off = 0;
            long offset = bwf_write_header(ff->fp, &bext, ixml_text,
                                           sess->sample_rate,
                                           sess->sample_format,
                                           (uint8_t)fr->n_channels,
                                           &junk_off);
            free(ixml_text);
            if (offset < 0) {
                fclose(ff->fp);
                ff->fp = NULL;
                warn_target(eng, target, "Failed to write BWF header");
                continue;
            }

            ff->junk_offset         = junk_off;
            ff->data_size_offset    = offset;
            ff->frames_written      = 0;
            ff->bytes_at_last_flush = 0;
            ff->valid               = true;
            fr->any_valid           = true;
            any_open                = true;
        }

        if (!fr->any_valid) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "{\"code\":2003,\"severity\":\"fatal\","
                     "\"message\":\"No valid target for folder '%s'\"}",
                     f_cfg->name);
            engine_emit(eng, EVT_ERROR, msg);
        }
    }

    return any_open;
}

/* -------------------------------------------------------------------------
 * File-close helper — finalises BWF headers, closes FPs.  Does NOT stop
 * the writer thread (caller decides).  Used by both disk_writer_close()
 * and the rotation path inside writer_thread().
 * ---------------------------------------------------------------------- */
static void close_all_folder_files(DiskWriter *dw, struct WavRecEngine *eng)
{
    const EngSession *sess = engine_session(eng);
    const uint8_t bit_depth = wavrec_fmt_bit_depth(sess->sample_format);

    for (int fi = 0; fi < WAVREC_MAX_FOLDERS; fi++) {
        FolderRecord *fr = &dw->folders[fi];
        if (!fr->any_valid) continue;
        for (int j = 0; j < WAVREC_MAX_TARGETS; j++) {
            FolderFile *ff = &fr->files[j];
            if (!ff->fp) continue;
            if (ff->valid) {
                bwf_finalise(ff->fp,
                             ff->junk_offset,
                             ff->data_size_offset,
                             ff->frames_written,
                             (uint8_t)fr->n_channels,
                             bit_depth);
                platform_fsync(ff->fp);
            }
            fclose(ff->fp);
            ff->fp    = NULL;
            ff->valid = false;
        }
        fr->any_valid  = false;
        fr->n_channels = 0;
    }
}

/* -------------------------------------------------------------------------
 * Sample-contiguous rotation — runs on the writer thread once per folder
 * set has drained exactly up to the rotation target.  The caller
 * (writer_thread) guarantees frames_written == target for every valid
 * folder before we get here.
 * ---------------------------------------------------------------------- */
static void rotate_files_now(DiskWriter *dw, struct WavRecEngine *eng,
                             uint64_t target_frame)
{
    DBGLOG("[dw:rotate_now] target_frame=%llu effective_origin=%llu (delta_samples=%llu) tc_frame_at_orig=%llu\n",
            (unsigned long long)target_frame,
            (unsigned long long)dw->effective_origin,
            (unsigned long long)(target_frame - dw->effective_origin),
            (unsigned long long)(engine_tc(eng) ? engine_tc(eng)->frame_at_origin : 0));
    const WavRecTimecodeSource *tc_before = engine_tc(eng);
    char log[256];
    snprintf(log, sizeof(log),
             "{\"level\":\"info\",\"message\":"
             "\"Rotate: target=%llu eff_origin=%llu tc_frame_at_orig=%llu tc_at_orig=%llu locked=%d\"}",
             (unsigned long long)target_frame,
             (unsigned long long)dw->effective_origin,
             (unsigned long long)tc_before->frame_at_origin,
             (unsigned long long)tc_before->tc_frames_at_origin,
             tc_before->locked ? 1 : 0);
    engine_emit(eng, EVT_LOG, log);

    /* 1. Finalise and close all current files. */
    close_all_folder_files(dw, eng);

    /* 2. Apply any staged pre-arm changes so newly-armed/disarmed tracks
     *    take effect exactly at the rotation boundary. */
    engine_apply_pre_armed(eng);

    /* 3. Advance the TC origin forward by the samples covered in the take
     *    just closed.  New file's BEXT TimeReference picks up where the
     *    previous file ended. */
    const EngSession *sess = engine_session(eng);
    tc_advance_origin_to((WavRecTimecodeSource *)engine_tc(eng),
                         target_frame, sess->sample_rate);

    const WavRecTimecodeSource *tc_after = engine_tc(eng);
    snprintf(log, sizeof(log),
             "{\"level\":\"info\",\"message\":"
             "\"Rotate: post-advance frame_at_orig=%llu tc_at_orig=%llu\"}",
             (unsigned long long)tc_after->frame_at_origin,
             (unsigned long long)tc_after->tc_frames_at_origin);
    engine_emit(eng, EVT_LOG, log);

    /* 4. Update our effective origin so file sample 0 of the new file maps
     *    to engine frame target_frame. */
    dw->effective_origin = target_frame;

    /* 5. Reopen files with the (possibly updated) armed set and new session
     *    state (new scene/take from the UI's pre-punch CMD_SESSION_INIT). */
    open_all_folder_files(dw, eng);
}

/* -------------------------------------------------------------------------
 * Public open / close
 * ---------------------------------------------------------------------- */

bool disk_writer_open(struct WavRecEngine *eng)
{
    DiskWriter *dw = get_dw(eng);
    if (!dw) return false;

    /* Snapshot effective origin from the TC source the engine just latched;
     * rotations will advance this forward in lock-step with TC. */
    const WavRecTimecodeSource *tc = engine_tc(eng);
    dw->effective_origin = tc ? tc->frame_at_origin : engine_frame_counter(eng);
    atomic_store_explicit(&dw->rotate_at_engine_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&dw->rotate_pending, 0, memory_order_relaxed);

    if (!open_all_folder_files(dw, eng)) return false;

    dw->last_status_ms         = platform_time_ms();
    dw->last_flush_ms          = dw->last_status_ms;
    dw->bytes_written_interval = 0;
    atomic_store_explicit(&dw->running,        1, memory_order_release);
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

    atomic_store_explicit(&dw->running, 0, memory_order_release);
    platform_thread_join(dw->thread);
    platform_thread_destroy(dw->thread);
    dw->thread = NULL;

    close_all_folder_files(dw, eng);
}

/* -------------------------------------------------------------------------
 * Public rotation request — records the target engine frame and returns
 * immediately.  Writer thread picks it up and performs the transition
 * when it has drained disk_rings up to that frame.
 * ---------------------------------------------------------------------- */

void disk_writer_rotate_at(struct WavRecEngine *eng, uint64_t target_frame)
{
    DiskWriter *dw = get_dw(eng);
    if (!dw) return;
    atomic_store_explicit(&dw->rotate_at_engine_frame, target_frame,
                          memory_order_release);
    atomic_store_explicit(&dw->rotate_pending, 1, memory_order_release);
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
    disk_writer_close(eng);
    free(dw);
    engine_set_disk_writer(eng, NULL);
}

int64_t disk_writer_free_bytes(struct WavRecEngine *eng)
{
    const EngSession *sess = engine_session(eng);
    int64_t min_free = -1;
    for (int fi = 0; fi < sess->n_folders; fi++) {
        const EngFolder *f = &sess->folders[fi];
        for (int j = 0; j < f->n_targets; j++) {
            int64_t b = platform_free_bytes(f->targets[j]);
            if (b < 0) continue;
            if (min_free < 0 || b < min_free) min_free = b;
        }
    }
    return min_free;
}
