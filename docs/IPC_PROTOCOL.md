# WavRec — IPC Protocol Specification

## Overview

Two independent IPC channels:

| Channel | Mechanism | Direction | Purpose |
|---|---|---|---|
| **Metering Bus** | Shared memory | Engine → UI | Per-channel peak/RMS, clip flags at ~60fps |
| **Waveform Bus** | Shared memory | Engine → UI | Real-time decimated waveform blocks |
| **Command/Event Bus** | Named pipe / Unix domain socket | Bidirectional | Transport commands, track config, status events |

---

## 1. Metering Bus (Shared Memory)

### Shared Memory Object
- **Windows:** `Global\wavrec_meters`
- **macOS/Linux:** `/wavrec_meters` (shm_open)
- **Size:** Fixed at compile time. See layout below.

### Layout

```c
#define WAVREC_MAX_CHANNELS  128
#define WAVREC_METER_MAGIC   0x4D455452  // "METR"

typedef struct {
    float  true_peak;     // dBFS, linear float (not log — UI converts)
    float  rms;           // dBFS, linear float
    float  peak_hold;     // dBFS, decays in engine or UI (TBD)
    uint8_t clip;         // 1 if clipped since last reset
    uint8_t active;       // 1 if track is armed/recording/playing
    uint8_t pad[2];
} WavRecMeterChannel;     // 14 bytes → padded to 16

typedef struct {
    uint32_t          magic;                          // WAVREC_METER_MAGIC
    uint8_t           version;                        // protocol version = 1
    uint8_t           write_index;                    // which frame slot is current (0 or 1)
    uint16_t          n_channels;                     // active channel count (≤128)
    uint8_t           pad[8];
} WavRecMeterHeader;      // 16 bytes

typedef struct {
    WavRecMeterHeader  header;
    WavRecMeterChannel channels[WAVREC_MAX_CHANNELS]; // slot 0
    WavRecMeterChannel channels_b[WAVREC_MAX_CHANNELS]; // slot 1 (double buffer)
} WavRecMeterRegion;
```

Total size: 16 + (128 × 16 × 2) = **4112 bytes** (~4KB)

### Double-buffer Protocol

**Engine (writer):**
1. Determine `next_slot = 1 - header.write_index`
2. Write all channel data into `channels` array of `next_slot`
3. `atomic_store(&header.write_index, next_slot)` with release semantics

**UI (reader):**
1. `slot = atomic_load(&header.write_index)` with acquire semantics
2. Read from `channels` array of `slot`
3. No locking required. May read a frame that is 1 update old at worst — acceptable for metering.

---

## 2. Waveform Bus (Shared Memory)

### Shared Memory Object
- **Windows:** `Global\wavrec_waveforms`
- **macOS/Linux:** `/wavrec_waveforms`

### Layout

A fixed-size lock-free SPSC ring buffer of waveform blocks.

```c
#define WAVREC_WFM_MAGIC        0x5746524D  // "WFRM"
#define WAVREC_WFM_RING_SLOTS   4096        // number of block slots in ring

typedef struct {
    uint8_t  channel_id;    // track index (0–127)
    uint8_t  pad[3];
    uint64_t timeline_frame; // engine frame counter at start of block
    uint32_t n_samples;     // samples this block represents (before decimation)
    float    min;           // minimum sample value in block
    float    max;           // maximum sample value in block
} WavRecWfmBlock;           // 24 bytes

typedef struct {
    uint32_t      magic;                          // WAVREC_WFM_MAGIC
    uint8_t       version;
    uint8_t       pad[3];
    uint32_t      decimation;                     // samples per block (e.g. 512)
    uint32_t      sample_rate;                    // engine sample rate
    _Atomic uint32_t write_pos;                   // writer advances this
    _Atomic uint32_t read_pos;                    // reader advances this
    uint8_t       pad2[44];                       // pad header to 64 bytes
    WavRecWfmBlock blocks[WAVREC_WFM_RING_SLOTS];
} WavRecWfmRegion;
```

Total size: 64 + (4096 × 24) = **98,368 bytes** (~96KB)

### Ring Buffer Protocol

**Engine (writer):**
1. Check `write_pos - read_pos < WAVREC_WFM_RING_SLOTS` (not full)
2. Write block to `blocks[write_pos % WAVREC_WFM_RING_SLOTS]`
3. `atomic_fetch_add(&write_pos, 1)` with release semantics

**UI (reader):**
1. While `read_pos != write_pos` (load acquire): consume block at `blocks[read_pos % WAVREC_WFM_RING_SLOTS]`
2. `atomic_fetch_add(&read_pos, 1)` with release semantics
3. UI accumulates blocks into its own per-track waveform cache for display

If the ring fills (UI too slow), the engine drops blocks. This is non-fatal — only affects waveform display resolution, not audio.

---

## 3. Command/Event Bus (Named Pipe / Unix Socket)

### Transport

| Platform | Path |
|---|---|
| Windows | `\\.\pipe\wavrec_cmd` |
| macOS / Linux | `/tmp/wavrec_cmd.sock` (Unix domain socket) |

Engine creates the server endpoint on startup. UI connects as client. Single persistent connection for the session lifetime. Engine sends an EVT_READY event on connection to signal readiness.

### Frame Envelope

All messages (commands and events) are framed identically:

```c
#define WAVREC_CMD_MAGIC  0x57415652  // "WAVR"

typedef struct {
    uint32_t magic;           // 0x57415652
    uint8_t  msg_type;        // see MessageType enum
    uint8_t  flags;           // reserved = 0
    uint16_t payload_length;  // bytes of JSON payload following this header
    uint32_t sequence;        // monotonically increasing, wraps at UINT32_MAX
    uint32_t crc32;           // CRC32 of payload bytes (0 if payload_length == 0)
} WavRecMsgHeader;            // 16 bytes
// followed immediately by payload_length bytes of UTF-8 JSON
```

Max payload: 65535 bytes (sufficient for any command or event — largest expected is a full session config ~10KB).

### Message Types

```c
typedef enum {
    // Commands (UI → Engine)
    CMD_PING           = 0x01,
    CMD_SESSION_INIT   = 0x02,  // configure session: sample rate, buffer size, project path
    CMD_TRACK_CONFIG   = 0x03,  // add/modify/remove track(s)
    CMD_ROUTE_CONFIG   = 0x04,  // update routing matrix
    CMD_ARM            = 0x10,  // arm one or more tracks for record
    CMD_DISARM         = 0x11,
    CMD_RECORD         = 0x12,  // begin recording
    CMD_PLAY           = 0x13,  // begin playback (with position)
    CMD_STOP           = 0x14,  // stop record and/or playback
    CMD_LOCATE         = 0x15,  // set playback head position
    CMD_METER_RESET          = 0x16,  // reset clip flags / peak hold
    CMD_SET_CUE              = 0x17,  // create/update a cue point at current or specified position
    CMD_DELETE_CUE           = 0x18,  // remove a cue point by id
    CMD_TRANSCRIPTION_CONFIG = 0x19,  // enable/disable transcription, set model/language/tracks
    CMD_TIMECODE_SOURCE      = 0x20,  // select TC source: free-run | LTC (channel) | MTC (port) — deferred
    CMD_SHUTDOWN             = 0x1F,

    // Events (Engine → UI)
    EVT_PONG           = 0x81,
    EVT_READY          = 0x82,  // engine ready, includes capability info
    EVT_STATE_CHANGE   = 0x83,  // engine state machine transition
    EVT_TRANSPORT      = 0x84,  // record/play head positions (sent at ~10Hz)
    EVT_TRACK_STATE    = 0x85,  // per-track status update
    EVT_SESSION_INFO   = 0x86,  // echo of accepted session config
    EVT_DISK_STATUS    = 0x87,  // available disk space, write rate per target
    EVT_CUE_UPDATE     = 0x88,  // cue point created/updated/deleted
    EVT_TRANSCRIPTION  = 0x89,  // transcription segment result
    EVT_TC_STATUS      = 0x8A,  // timecode source lock state change — deferred
    EVT_ERROR          = 0xE0,  // error with code and description
    EVT_LOG            = 0xE1,  // log message (level + text)
} WavRecMsgType;
```

---

### Payload Schemas (JSON)

#### CMD_SESSION_INIT
```json
{
  "project_path": "/path/to/project",
  "sample_rate": 48000,
  "bit_depth": 24,
  "buffer_frames": 512,
  "device_in": "Focusrite USB Audio",
  "device_out": "Focusrite USB Audio"
}
```

#### CMD_TRACK_CONFIG
```json
{
  "tracks": [
    {
      "id": 0,
      "label": "Lav 1",
      "hw_input": 0,
      "gain_db": 0.0,
      "armed": true,
      "monitor": false
    }
  ]
}
```

#### CMD_PLAY
```json
{
  "position_frames": 0,
  "tracks": [0, 1, 2]
}
```

#### CMD_LOCATE
```json
{
  "position_frames": 230400
}
```

#### EVT_READY
```json
{
  "engine_version": "0.1.0",
  "max_channels": 128,
  "shared_mem_meters": "Global\\wavrec_meters",
  "shared_mem_waveforms": "Global\\wavrec_waveforms",
  "sample_rate": 48000
}
```
Note: `EVT_READY` payload includes the shared memory object names so the UI knows exactly what to open — no hardcoded names on the UI side.

#### EVT_STATE_CHANGE
```json
{
  "prev_state": "IDLE",
  "new_state": "RECORDING",
  "timestamp_frames": 0
}
```

#### EVT_TRANSPORT
```json
{
  "record_head_frames": 115200,
  "play_head_frames": 48000,
  "recording": true,
  "playing": true,
  "timecode": "00:00:02:10"
}
```

#### EVT_DISK_STATUS
```json
{
  "free_bytes": 107374182400,
  "write_rate_bps": 18874368,
  "estimated_remaining_seconds": 5700
}
```

#### CMD_SESSION_INIT (extended)
```json
{
  "project_path": "/path/to/project",
  "sample_rate": 48000,
  "bit_depth": 24,
  "buffer_frames": 512,
  "device_in": "Focusrite USB Audio",
  "device_out": "Focusrite USB Audio",
  "timecode_rate": "25",
  "timecode_df": false,
  "scene": "001",
  "take": "01",
  "tape": "A001",
  "record_targets": [
    "/Volumes/SSD/project",
    "/Volumes/Backup/project"
  ]
}
```

#### CMD_SET_CUE
```json
{
  "id": "cue-001",
  "position_frames": 115200,
  "name": "Section A",
  "color": "#FF5500"
}
```

#### CMD_TRANSCRIPTION_CONFIG
```json
{
  "enabled": true,
  "model": "base.en",
  "language": "en",
  "tracks": [0, 1]
}
```

#### EVT_CUE_UPDATE
```json
{
  "action": "created",
  "id": "cue-001",
  "position_frames": 115200,
  "name": "Section A",
  "color": "#FF5500"
}
```

#### EVT_TRANSCRIPTION
```json
{
  "track_id": 0,
  "position_frames": 96000,
  "duration_frames": 24000,
  "text": "Take one, scene four.",
  "confidence": 0.94
}
```

#### EVT_DISK_STATUS (extended for redundant targets)
```json
{
  "targets": [
    { "path": "/Volumes/SSD/project",    "free_bytes": 107374182400, "write_rate_bps": 9437184, "ok": true },
    { "path": "/Volumes/Backup/project", "free_bytes": 214748364800, "write_rate_bps": 9437184, "ok": true }
  ],
  "estimated_remaining_seconds": 5700
}
```

#### EVT_ERROR
```json
{
  "code": 1003,
  "severity": "fatal",
  "message": "Audio device lost"
}
```

#### EVT_ERROR (redundant target warning example)
```json
{
  "code": 2001,
  "severity": "warning",
  "message": "Record target offline: /Volumes/Backup/project — continuing on remaining targets"
}
```

---

## Startup Sequence

```
UI                              Engine
│                                  │
│── (spawn engine process) ───────►│ starts up, opens shared memory, creates pipe server
│                                  │
│── (connect to pipe) ────────────►│ accepts connection
│                                  │
│◄── EVT_READY ───────────────────│ sends capabilities + shared memory names
│                                  │
│── CMD_SESSION_INIT ─────────────►│ configures session parameters
│◄── EVT_SESSION_INFO ────────────│
│                                  │
│── CMD_TRACK_CONFIG ─────────────►│ instantiates track objects
│◄── EVT_TRACK_STATE (×N) ────────│
│                                  │
│── (open shared memory) ─────────►│ both sides now sharing metering + waveform regions
│                                  │
│── CMD_ARM ──────────────────────►│
│── CMD_RECORD ───────────────────►│
│◄── EVT_STATE_CHANGE ────────────│ IDLE → RECORDING
│                                  │
│  [metering + waveform via shm]   │  (no pipe traffic for high-frequency data)
│◄── EVT_TRANSPORT (10Hz) ────────│
│                                  │
│── CMD_PLAY (position=0) ────────►│
│◄── EVT_STATE_CHANGE ────────────│ RECORDING → RECORD+PLAY
│                                  │
│── CMD_STOP ─────────────────────►│
│◄── EVT_STATE_CHANGE ────────────│ → STOPPING → IDLE
```

---

## Error Handling

- Engine sends `EVT_ERROR` with severity `warning` (recoverable) or `fatal` (requires restart)
- On `fatal`, engine transitions to `ERROR` state and closes the pipe. UI should offer recovery.
- CRC32 mismatch on any received message: log and discard. Do not attempt recovery — the pipe is a stream and a short read indicates a logic bug, not a transient error.
- If shared memory cannot be created/opened, engine sends `EVT_ERROR` (fatal) before closing. Metering and waveform are degraded-mode optional — the session can record without them.
