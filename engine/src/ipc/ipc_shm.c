#include "ipc_shm.h"
#include "../engine.h"
#include "../platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * IpcShm context
 * ---------------------------------------------------------------------- */

struct IpcShm {
    PlatformShm        meter_handle;
    PlatformShm        wfm_handle;
    WavRecMeterRegion *meters;
    WavRecWfmRegion   *waveforms;
};

/* -------------------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------------- */

bool ipc_shm_init(struct WavRecEngine *eng)
{
    struct IpcShm *shm = (struct IpcShm *)calloc(1, sizeof(struct IpcShm));
    if (!shm) return false;

    /* --- Metering region ------------------------------------------------ */
    shm->meter_handle = platform_shm_create(WAVREC_SHM_METERS,
                                             sizeof(WavRecMeterRegion));
    if (!shm->meter_handle) {
        free(shm);
        return false;
    }
    shm->meters = (WavRecMeterRegion *)platform_shm_ptr(shm->meter_handle);
    memset(shm->meters, 0, sizeof(WavRecMeterRegion));

    /* Init header */
    shm->meters->header.magic      = WAVREC_METER_MAGIC;
    shm->meters->header.version    = WAVREC_PROTOCOL_VERSION;
    shm->meters->header.n_channels = WAVREC_MAX_CHANNELS;
    atomic_init(&shm->meters->header.write_index, 0u);

    /* --- Waveform region ------------------------------------------------ */
    shm->wfm_handle = platform_shm_create(WAVREC_SHM_WAVEFORMS,
                                           sizeof(WavRecWfmRegion));
    if (!shm->wfm_handle) {
        platform_shm_close(shm->meter_handle);
        platform_shm_unlink(WAVREC_SHM_METERS);
        free(shm);
        return false;
    }
    shm->waveforms = (WavRecWfmRegion *)platform_shm_ptr(shm->wfm_handle);
    memset(shm->waveforms, 0, sizeof(WavRecWfmRegion));

    /* Init header — sample_rate left 0 until CMD_SESSION_INIT is parsed */
    shm->waveforms->magic       = WAVREC_WFM_MAGIC;
    shm->waveforms->version     = WAVREC_PROTOCOL_VERSION;
    shm->waveforms->decimation  = 512;
    shm->waveforms->sample_rate = 0;
    atomic_init(&shm->waveforms->write_pos, 0u);
    atomic_init(&shm->waveforms->read_pos,  0u);

    engine_set_ipc_shm(eng, shm);
    return true;
}

void ipc_shm_shutdown(struct WavRecEngine *eng)
{
    struct IpcShm *shm = engine_ipc_shm(eng);
    if (!shm) return;

    if (shm->wfm_handle) {
        platform_shm_close(shm->wfm_handle);
        platform_shm_unlink(WAVREC_SHM_WAVEFORMS);
    }
    if (shm->meter_handle) {
        platform_shm_close(shm->meter_handle);
        platform_shm_unlink(WAVREC_SHM_METERS);
    }
    free(shm);
    engine_set_ipc_shm(eng, NULL);
}

/* -------------------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------------- */

void ipc_shm_update_sample_rate(struct WavRecEngine *eng)
{
    struct IpcShm *shm = engine_ipc_shm(eng);
    if (!shm || !shm->waveforms) return;
    shm->waveforms->sample_rate = engine_sample_rate(eng);
}

WavRecMeterRegion *ipc_shm_meters(struct WavRecEngine *eng)
{
    struct IpcShm *shm = engine_ipc_shm(eng);
    return shm ? shm->meters : NULL;
}

WavRecWfmRegion *ipc_shm_waveforms(struct WavRecEngine *eng)
{
    struct IpcShm *shm = engine_ipc_shm(eng);
    return shm ? shm->waveforms : NULL;
}
