#pragma once
#ifdef WAVREC_HAVE_ASIO
/*
 * miniaudio_asio.h — ASIO custom backend for miniaudio.
 *
 * Usage:
 *   1. Call ma_asio_context_config_init() to get a pre-wired ma_context_config.
 *   2. Pass it to ma_context_init() — the ASIO backend registers itself as
 *      ma_backend_custom.  WASAPI devices still appear alongside ASIO ones.
 *   3. Use the context exactly as you would a normal miniaudio context.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "miniaudio.h"

/*
 * Wire ASIO callbacks into a context config.
 * pConfig->custom.onContextInit will be set; caller may set other fields freely.
 */
void ma_asio_context_config_init(ma_context_config *pConfig);

/*
 * Enumerate ASIO drivers from the Windows registry.
 * Calls back in the same format as ma_context_enumerate_devices.
 * Returns MA_SUCCESS even if no drivers are found.
 */
ma_result ma_asio_enumerate_devices(ma_context *pContext,
                                    ma_enum_devices_callback_proc callback,
                                    void *pUserData);

#ifdef __cplusplus
}
#endif

#endif /* WAVREC_HAVE_ASIO */
