#pragma once
/*
 * WavRec IPC protocol — shared struct definitions.
 * All multi-byte integers are little-endian (native on x86/ARM).
 * See docs/IPC_PROTOCOL.md for the full specification.
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define WAVREC_MAX_CHANNELS      128
#define WAVREC_IPC_HOST          "127.0.0.1"
#define WAVREC_IPC_PORT          27182      /* e × 10000, unlikely to clash */
/* Legacy names kept for reference but no longer used for transport */
#define WAVREC_PIPE_NAME_WIN     "\\\\.\\pipe\\wavrec_cmd"
#define WAVREC_PIPE_NAME_POSIX   "/tmp/wavrec_cmd.sock"
#define WAVREC_SHM_METERS        "wavrec_meters"
#define WAVREC_SHM_WAVEFORMS     "wavrec_waveforms"

#define WAVREC_MSG_MAGIC         UINT32_C(0x57415652)  /* "WAVR" */
#define WAVREC_METER_MAGIC       UINT32_C(0x4D455452)  /* "METR" */
#define WAVREC_WFM_MAGIC         UINT32_C(0x5746524D)  /* "WFRM" */
#define WAVREC_PROTOCOL_VERSION  1

/* -------------------------------------------------------------------------
 * Message types
 * ---------------------------------------------------------------------- */

typedef enum {
    /* Commands (UI → Engine) */
    CMD_PING                 = 0x01,
    CMD_SESSION_INIT         = 0x02,
    CMD_TRACK_CONFIG         = 0x03,
    CMD_ROUTE_CONFIG         = 0x04,
    CMD_ARM                  = 0x10,
    CMD_DISARM               = 0x11,
    CMD_RECORD               = 0x12,
    CMD_PLAY                 = 0x13,
    CMD_STOP                 = 0x14,
    CMD_LOCATE               = 0x15,
    CMD_METER_RESET          = 0x16,
    CMD_SET_CUE              = 0x17,
    CMD_DELETE_CUE           = 0x18,
    CMD_TRANSCRIPTION_CONFIG = 0x19,
    CMD_TIMECODE_SOURCE      = 0x20,  /* deferred */
    CMD_SHUTDOWN             = 0x1F,

    /* Events (Engine → UI) */
    EVT_PONG                 = 0x81,
    EVT_READY                = 0x82,
    EVT_STATE_CHANGE         = 0x83,
    EVT_TRANSPORT            = 0x84,
    EVT_TRACK_STATE          = 0x85,
    EVT_SESSION_INFO         = 0x86,
    EVT_DISK_STATUS          = 0x87,
    EVT_CUE_UPDATE           = 0x88,
    EVT_TRANSCRIPTION        = 0x89,
    EVT_TC_STATUS            = 0x8A,  /* deferred */
    EVT_ERROR                = 0xE0,
    EVT_LOG                  = 0xE1,
} WavRecMsgType;

/* -------------------------------------------------------------------------
 * Command/event bus frame envelope (16 bytes)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t magic;           /* WAVREC_MSG_MAGIC */
    uint8_t  msg_type;        /* WavRecMsgType */
    uint8_t  flags;           /* reserved = 0 */
    uint16_t payload_length;  /* bytes of JSON payload following header */
    uint32_t sequence;        /* monotonically increasing */
    uint32_t crc32;           /* CRC-32 of payload bytes; 0 if payload_length==0 */
} WavRecMsgHeader;            /* 16 bytes */

/* -------------------------------------------------------------------------
 * Shared memory — metering region
 * ---------------------------------------------------------------------- */

typedef struct {
    float    true_peak;   /* linear (not dBFS) — UI converts */
    float    rms;         /* linear */
    float    peak_hold;   /* linear, decays in engine */
    uint8_t  clip;        /* 1 if clipped since last CMD_METER_RESET */
    uint8_t  active;      /* 1 if track is armed / recording / playing */
    uint8_t  _pad[2];
} WavRecMeterChannel;     /* 16 bytes */

typedef struct {
    uint32_t magic;      /* WAVREC_METER_MAGIC */
    uint8_t  version;    /* WAVREC_PROTOCOL_VERSION */
    /* write_index selects which channel array (0 or 1) is current.
     * Writer completes a frame into the inactive slot then flips this
     * atomically with release semantics.  Reader loads with acquire. */
    _Atomic uint8_t  write_index;
    uint16_t         n_channels;
    uint8_t          _pad[8];
} WavRecMeterHeader;     /* 16 bytes */

typedef struct {
    WavRecMeterHeader  header;
    WavRecMeterChannel ch[WAVREC_MAX_CHANNELS];    /* slot 0 */
    WavRecMeterChannel ch_b[WAVREC_MAX_CHANNELS];  /* slot 1 */
} WavRecMeterRegion;
/* Total: 16 + 128*16*2 = 4112 bytes */

/* -------------------------------------------------------------------------
 * Shared memory — waveform ring buffer
 * ---------------------------------------------------------------------- */

#define WAVREC_WFM_RING_SLOTS 4096  /* must be power of two */

/* Field order chosen so natural C alignment gives exactly 24 bytes with
 * no implicit padding:
 *   0..0   channel_id (u8)
 *   1..3   _pad
 *   4..7   n_samples  (u32, 4-aligned)
 *   8..15  timeline_frame (u64, 8-aligned)
 *   16..19 min (f32)
 *   20..23 max (f32)
 * Kotlin WfmReader depends on this layout. */
typedef struct {
    uint8_t  channel_id;       /* track index 0-127 */
    uint8_t  _pad[3];
    uint32_t n_samples;        /* input samples this block represents */
    uint64_t timeline_frame;   /* engine frame counter at block start */
    float    min;
    float    max;
} WavRecWfmBlock;              /* 24 bytes */

typedef struct {
    uint32_t          magic;       /* WAVREC_WFM_MAGIC */
    uint8_t           version;
    uint8_t           _pad[3];
    uint32_t          decimation;  /* samples per display block, e.g. 512 */
    uint32_t          sample_rate;
    _Atomic uint32_t  write_pos;   /* producer advances */
    _Atomic uint32_t  read_pos;    /* consumer advances */
    uint8_t           _pad2[36];   /* pad header to 64 bytes */
    WavRecWfmBlock    blocks[WAVREC_WFM_RING_SLOTS];
} WavRecWfmRegion;
/* Total: 64 + 4096*24 = 98,368 bytes (~96 KB) */
