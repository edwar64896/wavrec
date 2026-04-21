#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Per-file handle state — one per (folder, target).  Each file is a
 * polyphonic BWF/WAV containing all armed tracks in the folder, ordered
 * by `track_ids` at the moment of record start.
 * ---------------------------------------------------------------------- */

#define DISK_CHUNK_FRAMES    4096   /* frames drained per iteration */
#define DISK_WRITE_BUF_SIZE  65536  /* setvbuf buffer per file */
#define WAVREC_MAX_CHANNELS  128
#define WAVREC_MAX_TARGETS     8
#define DW_MAX_FOLDERS        16    /* must match WAVREC_MAX_FOLDERS in engine.h */

typedef struct {
    FILE    *fp;
    bool     valid;              /* false = open failed or write error */
    long     junk_offset;        /* offset of JUNK/ds64 chunk header */
    long     data_size_offset;   /* file offset for patching data chunk size */
    uint64_t frames_written;     /* frames-per-channel (not samples) */
    uint64_t bytes_at_last_flush;/* cumulative bytes durable on disk at last flush */
} FolderFile;

typedef struct {
    /* Channel layout snapshot taken at disk_writer_open() — stays stable
     * for the lifetime of this take even if the user rearranges tracks. */
    int      track_ids[WAVREC_MAX_CHANNELS];
    int      n_channels;
    /* Per-target file for this folder. */
    FolderFile files[WAVREC_MAX_TARGETS];
    bool       any_valid;       /* at least one target open and writing */
} FolderRecord;

typedef struct DiskWriter {
    FolderRecord folders[DW_MAX_FOLDERS];

    /* Scratch buffers — owned exclusively by the writer thread.
     *   float_buf      : mono float samples from a single track's ring
     *   interleave_buf : interleaved poly float32 (channels × frames)
     *   pcm_buf        : bytes for one poly chunk after format conversion */
    float    float_buf     [DISK_CHUNK_FRAMES];
    float    interleave_buf[DISK_CHUNK_FRAMES * WAVREC_MAX_CHANNELS];
    uint8_t  pcm_buf       [DISK_CHUNK_FRAMES * WAVREC_MAX_CHANNELS * 4];

    /* Throughput tracking for EVT_DISK_STATUS (emitted ~1 Hz). */
    uint64_t  bytes_written_interval;
    uint64_t  last_status_ms;

    /* Periodic flush + RIFF-header fix-up (for crash safety and RF64
     * promotion).  See disk_writer.c::flush_all_files. */
    uint64_t  last_flush_ms;

    /* Sample-contiguous punch rotation.
     *   effective_origin  — engine frame of file sample 0 (shared across
     *                       all folders; set on open and advanced on
     *                       rotate).  New file's BEXT TimeReference is
     *                       derived from this via tc_advance_origin_to.
     *   rotate_at_engine_frame
     *                     — target engine frame at which to finalise the
     *                       current file set and open a new one.  Set
     *                       atomically by disk_writer_rotate_at().
     *   rotate_pending    — 1 when a rotation has been requested. */
    uint64_t          effective_origin;
    _Atomic uint64_t  rotate_at_engine_frame;
    _Atomic int       rotate_pending;

    /* Thread control */
    void          *thread;
    _Atomic int    running;
    _Atomic int    flush_and_stop;

    struct WavRecEngine *eng;
} DiskWriter;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool    disk_writer_init(struct WavRecEngine *eng);

/* Open files for every folder that has at least one armed track; one poly
 * WAV per (folder, target) on the folder's target list.  Called after
 * CMD_RECORD is accepted. */
bool    disk_writer_open(struct WavRecEngine *eng);

/* Drain rings, finalise headers, close files, stop the writer thread. */
void    disk_writer_close(struct WavRecEngine *eng);

void    disk_writer_shutdown(struct WavRecEngine *eng);

/* Returns the free bytes on the most-constrained target across all folders,
 * or -1 if none configured. */
int64_t disk_writer_free_bytes(struct WavRecEngine *eng);

/* Request a sample-contiguous file rotation at the given engine frame.
 * The disk writer will finalise the current file set when it has written
 * exactly (target_frame - effective_origin) samples, then open a new
 * file set using the current session state (new scene/take, armed set
 * with pre-arm applied, TC origin advanced).  Used for punch. */
void    disk_writer_rotate_at(struct WavRecEngine *eng, uint64_t target_frame);
