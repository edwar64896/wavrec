#pragma once
/*
 * Transcription thread — real-time whisper.cpp integration.
 * Enabled at build time only when WAVREC_TRANSCRIPTION_ENABLED is defined
 * (set in CMakeLists when whisper.cpp is linked).
 */
#include <stdbool.h>
#include <stdint.h>

struct WavRecEngine;

typedef struct {
    char     model[64];   /* e.g. "base.en" */
    char     language[8]; /* e.g. "en" */
    uint8_t  tracks[128]; /* track indices to transcribe */
    uint8_t  n_tracks;
    bool     enabled;
} TranscriptionConfig;

bool transcription_init(struct WavRecEngine *eng);
void transcription_configure(struct WavRecEngine *eng,
                             const TranscriptionConfig *cfg);
void transcription_start(struct WavRecEngine *eng);
void transcription_stop(struct WavRecEngine *eng);
void transcription_shutdown(struct WavRecEngine *eng);
