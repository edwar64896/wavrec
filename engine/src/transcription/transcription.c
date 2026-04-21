#include "transcription.h"
#include "../engine.h"
#include <string.h>

/* TODO: implement whisper.cpp integration.
 * Gate all whisper API calls behind WAVREC_TRANSCRIPTION_ENABLED so the
 * engine compiles and runs without the library present. */

bool transcription_init(struct WavRecEngine *eng)                    { (void)eng; return true; }
void transcription_configure(struct WavRecEngine *eng,
                             const TranscriptionConfig *cfg)         { (void)eng; (void)cfg; }
void transcription_start(struct WavRecEngine *eng)                   { (void)eng; }
void transcription_stop(struct WavRecEngine *eng)                    { (void)eng; }
void transcription_shutdown(struct WavRecEngine *eng)                { (void)eng; }
