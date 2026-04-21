#pragma once
#include <stdbool.h>
#include "ipc_protocol.h"

struct WavRecEngine;

/* Opaque shared memory context. */
typedef struct IpcShm IpcShm;

bool               ipc_shm_init(struct WavRecEngine *eng);
WavRecMeterRegion *ipc_shm_meters(struct WavRecEngine *eng);
WavRecWfmRegion   *ipc_shm_waveforms(struct WavRecEngine *eng);
/* Call after CMD_SESSION_INIT is applied so the wfm region reflects the
 * actual sample rate (not the 48kHz default). */
void               ipc_shm_update_sample_rate(struct WavRecEngine *eng);
void               ipc_shm_shutdown(struct WavRecEngine *eng);
