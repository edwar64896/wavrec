#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "timecode/timecode.h"
#include "ipc/ipc_protocol.h"

/* -------------------------------------------------------------------------
 * Engine state machine
 * ---------------------------------------------------------------------- */

typedef enum {
    ENG_STATE_IDLE        = 0,
    ENG_STATE_ARMED,
    ENG_STATE_RECORDING,
    ENG_STATE_PLAYING,
    ENG_STATE_RECORD_PLAY,
    ENG_STATE_STOPPING,
    ENG_STATE_ERROR,
    ENG_STATE_SHUTDOWN,
} EngState;

const char *eng_state_name(EngState s);

/* -------------------------------------------------------------------------
 * Session configuration (populated from CMD_SESSION_INIT payload)
 * ---------------------------------------------------------------------- */

#define WAVREC_MAX_TARGETS 8
#define WAVREC_MAX_TARGET_PATH 512

/* Sample format — drives disk conversion and WAV AudioFormat field. */
typedef enum {
    WAVREC_FMT_PCM16   = 0,  /* 16-bit signed integer PCM  */
    WAVREC_FMT_PCM24   = 1,  /* 24-bit signed integer PCM (default) */
    WAVREC_FMT_PCM32   = 2,  /* 32-bit signed integer PCM  */
    WAVREC_FMT_FLOAT32 = 3,  /* 32-bit IEEE 754 float       */
} WavSampleFormat;

/* Returns bit depth for a given format (16, 24, or 32). */
static inline uint8_t wavrec_fmt_bit_depth(WavSampleFormat f) {
    return (f == WAVREC_FMT_PCM16) ? 16u : (f == WAVREC_FMT_PCM24) ? 24u : 32u;
}

/* Returns bytes per sample for a given format. */
static inline uint8_t wavrec_fmt_bytes(WavSampleFormat f) {
    return (f == WAVREC_FMT_PCM16) ? 2u : (f == WAVREC_FMT_PCM24) ? 3u : 4u;
}

typedef struct {
    uint32_t       sample_rate;
    WavSampleFormat sample_format; /* replaces bare bit_depth */
    uint32_t       buffer_frames;
    char     device[128];
    TcRate   tc_rate;
    bool     tc_drop_frame;
    char     scene[32];
    char     take[8];
    char     tape[16];
    char     record_targets[WAVREC_MAX_TARGETS][WAVREC_MAX_TARGET_PATH];
    int      n_targets;
} EngSession;

/* -------------------------------------------------------------------------
 * Engine context — single global instance
 * ---------------------------------------------------------------------- */

typedef struct WavRecEngine WavRecEngine;

WavRecEngine *engine_create(void);
void          engine_destroy(WavRecEngine *eng);

/* Start all worker threads and IPC server. */
bool          engine_start(WavRecEngine *eng);

/* Block until CMD_SHUTDOWN is received or a fatal error occurs. */
void          engine_run(WavRecEngine *eng);

/* Thread-safe state accessors. */
EngState      engine_state(const WavRecEngine *eng);
bool          engine_is_recording(const WavRecEngine *eng);
bool          engine_is_playing(const WavRecEngine *eng);

/* Frame counter — monotonically increasing, driven by Audio I/O callback. */
uint64_t      engine_frame_counter(const WavRecEngine *eng);

/* Called by the audio I/O callback to advance the frame counter.
 * Only the callback writes this; everyone else reads. */
void          engine_advance_frames(WavRecEngine *eng, uint32_t n);

/* Session and timecode — read-only access for subsystem modules. */
const EngSession            *engine_session(const WavRecEngine *eng);
const WavRecTimecodeSource  *engine_tc(const WavRecEngine *eng);

/* Read-only track access for the audio callback and subsystem threads.
 * Returns NULL if idx is out of range.  No lock — callers must tolerate
 * a brief race window during track reconfiguration. */
const struct Track *engine_get_track(const WavRecEngine *eng, int idx);
int           engine_n_tracks(const WavRecEngine *eng);
uint32_t         engine_sample_rate(const WavRecEngine *eng);
uint32_t         engine_buffer_frames(const WavRecEngine *eng);
WavSampleFormat  engine_sample_format(const WavRecEngine *eng);

/* PlaybackEngine subsystem accessor/mutator. */
struct PlaybackEngine *engine_playback(WavRecEngine *eng);
void                   engine_set_playback(WavRecEngine *eng,
                                           struct PlaybackEngine *pe);

/* RecordEngine subsystem accessor/mutator. */
struct RecordEngine   *engine_record_engine(WavRecEngine *eng);
void                   engine_set_record_engine(WavRecEngine *eng,
                                                struct RecordEngine *re);

/* MeteringEngine subsystem accessor/mutator. */
struct MeteringEngine *engine_metering(WavRecEngine *eng);
void                   engine_set_metering(WavRecEngine *eng,
                                           struct MeteringEngine *me);

/* WaveformEngine subsystem accessor/mutator. */
struct WaveformEngine *engine_waveform(WavRecEngine *eng);
void                   engine_set_waveform(WavRecEngine *eng,
                                           struct WaveformEngine *we);

/* DiskWriter subsystem accessor/mutator. */
struct DiskWriter *engine_disk_writer(WavRecEngine *eng);
void               engine_set_disk_writer(WavRecEngine *eng,
                                          struct DiskWriter *dw);

/* IpcContext accessor/mutator. */
struct IpcContext *engine_ipc(WavRecEngine *eng);
void               engine_set_ipc(WavRecEngine *eng, struct IpcContext *ctx);

/* IpcShm accessor/mutator. */
struct IpcShm *engine_ipc_shm(WavRecEngine *eng);
void           engine_set_ipc_shm(WavRecEngine *eng, struct IpcShm *shm);

/* AudioIO subsystem accessor/mutator.
 * engine_audio_io: used by Record/Playback engine threads.
 * engine_set_audio_io: called only by audio_io_init/shutdown. */
struct AudioIO *engine_audio_io(WavRecEngine *eng);
void            engine_set_audio_io(WavRecEngine *eng, struct AudioIO *aio);

/* Called by IPC Rx thread to dispatch a parsed command. */
void          engine_dispatch(WavRecEngine *eng, WavRecMsgType type,
                              uint32_t sequence,
                              const char *payload, uint16_t payload_len);

/* Called by subsystems to emit an event to the UI. */
void          engine_emit(WavRecEngine *eng, WavRecMsgType type,
                          const char *payload_json);
