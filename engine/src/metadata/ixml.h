#pragma once
/*
 * iXML chunk builder.
 * Produces the XML string to be embedded in the WAV iXML chunk.
 */
#include <stdint.h>
#include "../timecode/timecode.h"

typedef struct {
    const char *project;
    const char *scene;
    const char *take;
    const char *tape;
    /* Track list — parallel arrays, n_tracks entries each */
    int         n_tracks;
    const char **track_names;
    const uint8_t *track_hw_inputs;
    /* Timecode */
    const WavRecTimecodeSource *tc;
    uint32_t    sample_rate;
    uint8_t     bit_depth;
} IxmlParams;

/* Render iXML to a malloc'd null-terminated string. Caller frees. */
char *ixml_render(const IxmlParams *p);
