#pragma once
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * SMPTE frame rate descriptors
 * ---------------------------------------------------------------------- */

typedef enum {
    TC_RATE_23976 = 0,   /* 24000/1001 — film */
    TC_RATE_24,          /* 24 NDF */
    TC_RATE_25,          /* 25 NDF — PAL */
    TC_RATE_2997_NDF,    /* 30000/1001 NDF */
    TC_RATE_2997_DF,     /* 30000/1001 DF */
    TC_RATE_30_NDF,      /* 30 NDF */
    TC_RATE_30_DF,       /* 30 DF */
    TC_RATE_COUNT,
} TcRate;

typedef struct {
    uint32_t num;        /* numerator   e.g. 24000 */
    uint32_t den;        /* denominator e.g. 1001  */
    uint32_t nominal;    /* nominal integer fps, e.g. 24 */
    bool     drop_frame;
} TcRateInfo;

/* Returns the descriptor for a given rate enum. */
const TcRateInfo *tc_rate_info(TcRate rate);

/* -------------------------------------------------------------------------
 * TimecodeSource
 *
 * All consumers call tc_now() — the source type is transparent.
 * The active implementation is selected at runtime via tc_source_set_*().
 * ---------------------------------------------------------------------- */

typedef enum {
    TC_SOURCE_FREE_RUN = 0,   /* latched from RTC on record start */
    TC_SOURCE_LTC,            /* decoded from audio channel (deferred) */
    TC_SOURCE_MTC,            /* MIDI Timecode (deferred) */
} TcSourceType;

typedef struct {
    TcSourceType  type;
    TcRate        rate;
    bool          locked;           /* false = free-run or unlocked chase */

    /* Origin latch — set when TC is established. */
    uint64_t      frame_at_origin;  /* engine frame counter at latch moment */
    uint64_t      tc_frames_at_origin; /* SMPTE frame count at latch (frames since midnight) */
} WavRecTimecodeSource;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

void tc_init(WavRecTimecodeSource *src, TcRate rate);

/* Latch free-run TC from the real-time clock at the given engine frame.
 * Call this at the moment recording begins. */
void tc_latch_free_run(WavRecTimecodeSource *src,
                       uint64_t engine_frame,
                       uint32_t sample_rate);

/* Advance the origin to `new_engine_frame` while preserving TC continuity.
 * Used when a new file is started mid-recording (punch) so that the new
 * file's BEXT TimeReference picks up exactly where the previous file ended.
 * No wall-clock reads — purely deterministic re-basing. */
void tc_advance_origin_to(WavRecTimecodeSource *src,
                          uint64_t new_engine_frame,
                          uint32_t sample_rate);

/* Compute current SMPTE frame count (frames since midnight) given the
 * current engine frame counter.  Works for all source types. */
uint64_t tc_frames_now(const WavRecTimecodeSource *src,
                       uint64_t current_engine_frame,
                       uint32_t sample_rate);

/* Compute wall-clock SMPTE frame count (frames since midnight) without
 * requiring the source to be latched.  Used for free-run display before
 * recording starts. */
uint64_t tc_frames_wallclock(const WavRecTimecodeSource *src,
                             uint32_t sample_rate);

/* Format as "HH:MM:SS:FF" or "HH:MM:SS;FF" (DF).
 * buf must be at least 12 bytes. */
void tc_format(const WavRecTimecodeSource *src,
               uint64_t tc_frames,
               char *buf, int buf_len);

/* Parse "HH:MM:SS:FF" / "HH:MM:SS;FF" → total SMPTE frame count.
 * Returns -1 on parse error. */
int64_t tc_parse(const WavRecTimecodeSource *src, const char *str);
