#include "timecode.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Rate table
 * ---------------------------------------------------------------------- */

static const TcRateInfo k_rates[TC_RATE_COUNT] = {
    [TC_RATE_23976]   = { 24000, 1001, 24, false },
    [TC_RATE_24]      = {    24,    1, 24, false },
    [TC_RATE_25]      = {    25,    1, 25, false },
    [TC_RATE_2997_NDF]= { 30000, 1001, 30, false },
    [TC_RATE_2997_DF] = { 30000, 1001, 30, true  },
    [TC_RATE_30_NDF]  = {    30,    1, 30, false },
    [TC_RATE_30_DF]   = {    30,    1, 30, true  },
};

const TcRateInfo *tc_rate_info(TcRate rate)
{
    if (rate < 0 || rate >= TC_RATE_COUNT) return NULL;
    return &k_rates[rate];
}

/* -------------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------------- */

void tc_init(WavRecTimecodeSource *src, TcRate rate)
{
    memset(src, 0, sizeof(*src));
    src->type   = TC_SOURCE_FREE_RUN;
    src->rate   = rate;
    src->locked = false;
}

/* -------------------------------------------------------------------------
 * Free-run latch
 * ---------------------------------------------------------------------- */

void tc_latch_free_run(WavRecTimecodeSource *src,
                       uint64_t engine_frame,
                       uint32_t sample_rate)
{
    /* Convert wall-clock position to SMPTE frame count at the nominal fps.
     * We use samples-since-midnight for sub-second accuracy. */
    uint64_t samples_since_mid = platform_samples_since_midnight(sample_rate);

    const TcRateInfo *ri = tc_rate_info(src->rate);

    /* tc_frames = samples_since_midnight * nominal_fps / sample_rate
     * Use 64-bit arithmetic to avoid overflow at high sample rates. */
    uint64_t tc_frames = (samples_since_mid * ri->nominal) / sample_rate;

    src->frame_at_origin    = engine_frame;
    src->tc_frames_at_origin = tc_frames;
    src->locked             = true;
    src->type               = TC_SOURCE_FREE_RUN;
}

/* -------------------------------------------------------------------------
 * Current timecode
 * ---------------------------------------------------------------------- */

uint64_t tc_frames_now(const WavRecTimecodeSource *src,
                       uint64_t current_engine_frame,
                       uint32_t sample_rate)
{
    if (!src->locked)
        return 0;

    uint64_t elapsed_samples = current_engine_frame - src->frame_at_origin;
    const TcRateInfo *ri = tc_rate_info(src->rate);

    /* elapsed_tc_frames = elapsed_samples * nominal_fps / sample_rate */
    uint64_t elapsed_tc = (elapsed_samples * ri->nominal) / sample_rate;

    return src->tc_frames_at_origin + elapsed_tc;
}

uint64_t tc_frames_wallclock(const WavRecTimecodeSource *src,
                             uint32_t sample_rate)
{
    uint64_t samples_since_mid = platform_samples_since_midnight(sample_rate);
    const TcRateInfo *ri = tc_rate_info(src->rate);
    return (samples_since_mid * ri->nominal) / sample_rate;
}

/* -------------------------------------------------------------------------
 * Formatting
 * ---------------------------------------------------------------------- */

void tc_format(const WavRecTimecodeSource *src,
               uint64_t tc_frames,
               char *buf, int buf_len)
{
    const TcRateInfo *ri = tc_rate_info(src->rate);
    uint32_t fps = ri->nominal;

    uint32_t ff = (uint32_t)(tc_frames % fps);
    uint64_t total_secs = tc_frames / fps;
    uint32_t ss = (uint32_t)(total_secs % 60);
    uint64_t total_mins = total_secs / 60;
    uint32_t mm = (uint32_t)(total_mins % 60);
    uint32_t hh = (uint32_t)(total_mins / 60);

    char sep = ri->drop_frame ? ';' : ':';
    snprintf(buf, (size_t)buf_len, "%02u:%02u:%02u%c%02u", hh, mm, ss, sep, ff);
}

/* -------------------------------------------------------------------------
 * Parsing
 * ---------------------------------------------------------------------- */

int64_t tc_parse(const WavRecTimecodeSource *src, const char *str)
{
    unsigned hh, mm, ss, ff;
    char sep;
    if (sscanf(str, "%u:%u:%u%c%u", &hh, &mm, &ss, &sep, &ff) != 5)
        return -1;

    const TcRateInfo *ri = tc_rate_info(src->rate);
    uint32_t fps = ri->nominal;

    uint64_t total = ((uint64_t)hh * 3600 + (uint64_t)mm * 60 + ss) * fps + ff;

    /* Drop-frame correction (29.97 DF / 30 DF).
     * Standard Nilsson formula: drop 2 (or 4) frame numbers at the start
     * of every minute except multiples of 10. */
    if (ri->drop_frame) {
        uint32_t drop_per_min = (fps == 30 && ri->den == 1001) ? 2 : 2;
        uint64_t total_mins = (uint64_t)hh * 60 + mm;
        uint64_t drop_frames = drop_per_min * (total_mins - total_mins / 10);
        total -= drop_frames;
    }

    return (int64_t)total;
}
