#pragma once
#ifdef WAVREC_HAVE_ASIO

#include <stdint.h>
#include <stdbool.h>

struct WavRecEngine;

/* One-time init — call after engine is created, before any open. */
bool  asio_engine_init(struct WavRecEngine *eng);

/* Enumerate available ASIO drivers as a JSON array (malloc'd, caller frees).
 * Each driver appears twice: one "input" entry, one "output" entry.
 * Format: [{"name":"...","type":"input","is_default":false,"backend":"asio"},...] */
char *asio_engine_list_devices(void);

/* Return true if a driver with this exact name exists in the ASIO registry. */
bool  asio_engine_has_driver(const char *name);

/* Open the named ASIO driver and start streaming.
 * On success fills *ch_in_out, *sr_out, *buf_frames_out with actual values. */
bool  asio_engine_open(struct WavRecEngine *eng,
                       const char *driver_name,
                       uint32_t preferred_sample_rate,
                       uint32_t preferred_buffer_frames,
                       uint32_t *ch_in_out,
                       uint32_t *sr_out,
                       uint32_t *buf_frames_out);

/* Stop and release the ASIO driver. */
void  asio_engine_close(void);

/* Full teardown — call once on shutdown. */
void  asio_engine_shutdown(void);

#endif /* WAVREC_HAVE_ASIO */
