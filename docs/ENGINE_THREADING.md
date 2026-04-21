# WavRec — Engine Threading Model

## Design Principles

- The audio I/O callback thread does the absolute minimum (ring buffer copy only)
- All inter-thread communication uses lock-free ring buffers or atomics — no mutexes on the real-time path
- Disk I/O is fully decoupled from the audio path
- Engine state changes are driven exclusively by the IPC command thread via a command queue
- The timecode subsystem is source-agnostic: all consumers (BEXT stamping, EVT_TRANSPORT, iXML, cue points) read from a single `TimecodeSource` handle; the active source implementation is swapped at runtime

---

## Thread Inventory

```
┌─────────────────────────────────────────────────────────────────┐
│  REAL-TIME THREADS                     Priority                  │
│                                                                  │
│  [Audio I/O]  ──────────────────────  TIME_CRITICAL             │
│      │  input ring buf (per track)                              │
│      │  output mix bus ring buf                                 │
│      ▼                                                           │
│  [Record Engine]  ──────────────────  HIGHEST                   │
│      │  gain/trim → meter feed → wfm feed → disk write queue   │
│      ▼                                                           │
│  [Playback Engine]  ────────────────  HIGHEST                   │
│      │  disk read → decode → mix → output ring buf             │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│  SECONDARY THREADS                                               │
│                                                                  │
│  [Metering]  ───────────────────────  ABOVE_NORMAL             │
│      Drains meter feed buffers, computes peak/RMS,             │
│      writes to shared memory metering region                    │
│                                                                  │
│  [Waveform]  ───────────────────────  NORMAL                   │
│      Drains wfm feed queue, computes min/max decimation,       │
│      writes blocks to shared memory waveform ring              │
│                                                                  │
│  [Disk Writer]  ────────────────────  BELOW_NORMAL             │
│      Drains per-track disk write queues, writes BWF/WAV        │
│      Writes to all configured redundant targets in parallel    │
│      Separate pool of 1–N threads, scales with track count     │
│                                                                  │
│  [Transcription]  ──────────────────  BELOW_NORMAL             │
│      Drains transcription feed ring buffer (per armed track)   │
│      Accumulates audio segments, submits to whisper.cpp        │
│      Sends EVT_TRANSCRIPTION results via IPC event queue       │
│                                                                  │
│  [IPC Rx]  ─────────────────────────  NORMAL                   │
│      Reads command messages from named pipe/socket,            │
│      deserialises and pushes to command queue                  │
│                                                                  │
│  [IPC Tx]  ─────────────────────────  NORMAL                   │
│      Drains internal event queue, serialises and sends         │
│      event messages to UI over command channel                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Thread Detail

### 1. Audio I/O Thread
Driven by miniaudio's device callback (miniaudio manages thread creation and priority).

**Inputs:** miniaudio capture buffer (up to 128 channels from hardware)
**Outputs:** miniaudio playback buffer

**Work done:**
- Copy N frames of interleaved input samples into per-track lock-free ring buffers (de-interleave)
- Copy N frames from the mix bus ring buffer into the miniaudio playback buffer
- Increment a 64-bit frame counter (atomic) for timeline tracking
- Nothing else. No allocation, no syscalls, no locks.

**Ring buffer sizing:** 4× the miniaudio period size (e.g., 512-frame period → 2048-frame ring buffer per track). Gives Record Engine headroom without starvation.

---

### 2. Record Engine Thread
**Inputs:** per-track input ring buffers
**Outputs:** per-track meter feed buffers, waveform feed queue, disk write queue

**Work done per track per iteration:**
1. Drain available frames from input ring buffer
2. Apply input gain/trim (single multiply pass)
3. Push frames to meter feed buffer (lock-free SPSC)
4. Push frames to waveform feed queue (lock-free SPSC, lower duty cycle — every N frames)
5. Push frames to disk write queue (lock-free SPSC per track)

**Timing:** Runs in a tight loop, wakes on a condition variable signalled by the Audio I/O thread after each callback completion. Falls back to timed sleep (1ms) to avoid busy-waiting.

---

### 3. Playback Engine Thread
**Inputs:** per-track playback file handles, timeline position, track volume/pan state
**Outputs:** mix bus ring buffer fed to Audio I/O thread

**Work done:**
1. Determine how many frames the mix bus ring buffer needs
2. For each active playback track: read frames from disk (via read-ahead buffer), apply gain/pan
3. Mix all playback tracks into a stereo or N-channel output bus
4. Write mix result to output ring buffer
5. Advance playback head position

**Read-ahead buffer:** Each playback track maintains a per-track read-ahead buffer (e.g., 100ms of audio) filled by the Disk Writer thread's read side, ensuring the Playback Engine thread never blocks on disk.

**Key:** Record head and playback head are independent 64-bit frame counters. The engine supports any combination:
- Playback-only (record head stationary)
- Record-only (playback head stationary)  
- Simultaneous record + playback at different timeline positions

---

### 4. Metering Thread
**Inputs:** per-track meter feed buffers (SPSC ring buffers)
**Outputs:** shared memory metering region (see IPC spec)

**Work done:**
- Runs at ~60Hz (16.7ms sleep between iterations)
- For each active track: drain meter feed buffer, compute true-peak and RMS over the available window
- Write results atomically to the shared memory double-buffer (flip index on completion)

---

### 5. Waveform Thread
**Inputs:** per-track waveform feed queues
**Outputs:** shared memory waveform ring buffer (see IPC spec)

**Work done:**
- Drains waveform feed queue
- Computes min/max over a configurable decimation block size (e.g., 512 samples → 1 display pixel)
- Writes WFM blocks (channel_id, timeline_frame, min, max) into the shared memory waveform ring

---

### 6. Disk Writer Thread(s)
**Inputs:** per-track disk write queues (record writes), per-track read-ahead fill requests (playback reads)
**Outputs:** WAV/BWF files on disk; read-ahead buffers

A small thread pool (default: 4 threads, configurable). Each iteration:
- Service pending write requests (record path) — write to open file handles
- Service pending read-ahead requests (playback path) — read from file handles into per-track prefetch buffers

File handles are opened when a track is armed and closed on stop. Uses buffered I/O with large write buffers (e.g., 1MB per track) to minimise syscall overhead.

---

### 7. Transcription Thread
**Inputs:** per-track transcription feed ring buffers (SPSC, filled by Record Engine at BELOW_NORMAL duty cycle)
**Outputs:** IPC event queue (EVT_TRANSCRIPTION messages)

The Record Engine pushes decoded float32 frames into a transcription feed buffer only for tracks configured for transcription (`CMD_TRANSCRIPTION_CONFIG`). The Transcription thread accumulates audio into segments of configurable length (default: 5–10s, balancing latency vs. accuracy), then submits each segment synchronously to **whisper.cpp**.

whisper.cpp is a blocking call — the thread sleeps between submissions. Since it runs at `BELOW_NORMAL` priority and is entirely off the real-time path, long inference times (e.g., on CPU) cannot affect recording.

Results are pushed to the IPC event queue as `EVT_TRANSCRIPTION` with track_id, timeline position, duration, text, and confidence score.

**Feed buffer sizing:** Large enough to absorb whisper inference time without overflow. At 48kHz float32 mono, 30s of audio = ~5.5MB per track. Size to 2× the configured segment length.

---

### 8 & 9. IPC Rx / Tx Threads
See `docs/IPC_PROTOCOL.md`. These threads are the only ones that touch the IPC channels. All communication with the rest of the engine passes through:
- **Rx → Engine:** lock-free SPSC command queue
- **Engine → Tx:** lock-free SPSC event queue

---

## Engine State Machine

```
           CMD_ARM          CMD_RECORD
  IDLE ──────────► ARMED ──────────► RECORDING ─────┐
   ▲                │                    │           │ CMD_PLAY
   │                │ CMD_DISARM         │           ▼
   │                ▼                   │      RECORD+PLAY
   │              IDLE                  │           │
   │                           CMD_STOP │           │ CMD_STOP
   │                                    ▼           ▼
   └───────────────────────────────── STOPPING ────►┘
                                          │
                                          ▼
                                        IDLE
                                        
  Any state ──── CMD_SHUTDOWN ──► SHUTDOWN
  Any state ──── internal fault ──► ERROR (recoverable via CMD_RESET)
```

State is a single atomic enum. Threads read state to decide whether to process data. Only the IPC Rx thread writes state (via the command queue dispatch).

---

## Timecode Subsystem

### TimecodeSource Abstraction

All timecode consumers reference a single `WavRecTimecodeSource *` handle. The implementation behind it is hot-swappable at runtime via `CMD_TIMECODE_SOURCE`.

```c
typedef enum {
    TC_SOURCE_FREE_RUN = 0,   // latched from RTC on record start — implement first
    TC_SOURCE_LTC      = 1,   // decoded from audio channel via libltc — deferred
    TC_SOURCE_MTC      = 2,   // MIDI Timecode quarter-frames — deferred
} WavRecTcSourceType;

typedef struct {
    WavRecTcSourceType type;
    bool               locked;           // false = free-running or unlocked chase
    uint64_t           frame_at_origin;  // engine frame counter when TC was latched/locked
    uint32_t           tc_frames_at_origin; // SMPTE frame count at latch point (frames since midnight)
    uint8_t            fps_enum;         // index into rate table
    bool               drop_frame;
    // source-specific state follows in a union
} WavRecTimecodeSource;
```

Consumers call a single inline function `wavrec_tc_now(source, current_engine_frame)` which computes current timecode from the origin latch + elapsed frames, regardless of source type. This means Free-Run, LTC, and MTC all produce the same output type and require no changes in BEXT stamping, EVT_TRANSPORT, or iXML.

### Free-Run (active)
On `CMD_RECORD`:
1. Read wall-clock time (platform `clock_gettime` / `GetSystemTimeAsFileTime`)
2. Convert to SMPTE frame count at the configured rate (samples since midnight)
3. Store as `tc_frames_at_origin`, store current engine frame counter as `frame_at_origin`
4. Set `locked = true`, `type = TC_SOURCE_FREE_RUN`

Timecode then advances sample-accurately via the engine frame counter — no clock drift.

### LTC Chase (deferred — REQ-META-003b)
- Requires a designated audio input channel (configured via `CMD_TIMECODE_SOURCE`)
- Record Engine feeds that channel's ring buffer to a libltc decoder within its processing loop
- On lock, updates `tc_frames_at_origin` and `frame_at_origin`; sets `locked = true`
- On loss of lock, sets `locked = false`; engine continues free-running from last locked position
- Lock status reported in `EVT_TRANSPORT` (`"tc_locked": true/false`)

### MTC Chase (deferred — REQ-META-003c)
- Requires a dedicated MIDI Input thread (added to thread inventory when implemented)
- Thread reads quarter-frame messages from RtMidi callback, assembles full-frame timecode
- On lock, updates `TimecodeSource` via atomic write; same lock/unlock reporting as LTC

### IPC additions (when timecode sources are implemented)
```
CMD_TIMECODE_SOURCE  = 0x20   // select source type + parameters (channel for LTC, port for MTC)
EVT_TC_STATUS        = 0x8A   // lock state change, current source, current timecode
```

---

## Cross-platform Notes

| Concern | Windows | macOS | Linux |
|---|---|---|---|
| Thread priority | `SetThreadPriority` | `pthread` + `sched_setscheduler` | `sched_setscheduler(SCHED_FIFO)` |
| Audio backend | WASAPI (default), ASIO | CoreAudio | ALSA / JACK / PipeWire |
| Shared memory | `CreateFileMapping` | `shm_open` | `shm_open` |
| Named pipe | `\\.\pipe\wavrec_cmd` | Unix domain socket `/tmp/wavrec_cmd.sock` | Unix domain socket |
| Lock-free ring | Platform-agnostic C (memory barriers via `stdatomic.h`) | same | same |

All platform-specific code isolated behind a `platform/` abstraction layer.
