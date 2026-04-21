#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Per-file handle state (one per track per recording target)
 * ---------------------------------------------------------------------- */

typedef struct {
    FILE    *fp;
    bool     valid;            /* false = open failed or write error */
    long     data_size_offset; /* file offset for patching data chunk size */
    uint64_t frames_written;
} TrackFile;

/* -------------------------------------------------------------------------
 * DiskWriter context
 * ---------------------------------------------------------------------- */

#define DISK_CHUNK_FRAMES    4096   /* samples read from ring per iteration */
#define DISK_WRITE_BUF_SIZE  65536  /* setvbuf buffer per file (64 KB) */
#define WAVREC_MAX_CHANNELS  128
#define WAVREC_MAX_TARGETS     8

typedef struct DiskWriter {
    /* File handles: [track_id][target_idx] */
    TrackFile files[WAVREC_MAX_CHANNELS][WAVREC_MAX_TARGETS];

    /* True if at least one valid file is open for this track. */
    bool      track_active[WAVREC_MAX_CHANNELS];

    /* Conversion scratch — owned by the writer thread (single-threaded).
     * pcm_buf sized for the widest format: 4 bytes/sample (32-bit or float). */
    float     float_buf[DISK_CHUNK_FRAMES];
    uint8_t   pcm_buf  [DISK_CHUNK_FRAMES * 4];

    /* Throughput tracking for EVT_DISK_STATUS (emitted ~1 Hz). */
    uint64_t  bytes_written_interval;
    uint64_t  last_status_ms;

    /* Thread control */
    void          *thread;       /* PlatformThread */
    _Atomic int    running;
    _Atomic int    flush_and_stop; /* set by close: drain rings then exit */

    struct WavRecEngine *eng;
} DiskWriter;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool    disk_writer_init(struct WavRecEngine *eng);

/* Open files for all armed tracks across all configured targets and
 * start the writer thread. Call after CMD_RECORD is accepted. */
bool    disk_writer_open(struct WavRecEngine *eng);

/* Signal the writer thread to drain remaining data, flush and patch all
 * open files, then stop. Blocks until the thread exits. */
void    disk_writer_close(struct WavRecEngine *eng);

void    disk_writer_shutdown(struct WavRecEngine *eng);

/* Returns the free bytes on the most-constrained target, or -1. */
int64_t disk_writer_free_bytes(struct WavRecEngine *eng);
