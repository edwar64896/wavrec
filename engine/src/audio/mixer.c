#include "mixer.h"
#include "../engine.h"

/* TODO: implement channel strip (gain, pan) and mix bus summing. */

bool mixer_init(struct WavRecEngine *eng)                           { (void)eng; return true; }
void mixer_process(struct WavRecEngine *eng, uint32_t n_frames)     { (void)eng; (void)n_frames; }
void mixer_shutdown(struct WavRecEngine *eng)                       { (void)eng; }
