#pragma once
#include <stdint.h>
#include <stdbool.h>
struct WavRecEngine;
bool mixer_init(struct WavRecEngine *eng);
void mixer_process(struct WavRecEngine *eng, uint32_t n_frames);
void mixer_shutdown(struct WavRecEngine *eng);
