# WavRec — Requirements

## Overview
A multi-channel audio recording and annotation application. The core audio processing engine is written in C; a separate UI layer communicates with the engine via IPC.

---

## Architecture

### REQ-ARCH-001 — C Audio Engine
The core recording, transcription, and processing engine shall be implemented in C using standard audio libraries. Candidate: **miniaudio** (single-header, cross-platform, supports WASAPI/ASIO/CoreAudio/ALSA/JACK).

### REQ-ARCH-002 — Multi-threading
The engine shall be multi-threaded with the following dedicated threads: Audio I/O (miniaudio callback), Record Engine, Playback Engine, Metering, Waveform, Disk Writer (pool), Transcription, IPC Rx, IPC Tx. See `docs/ENGINE_THREADING.md`.

### REQ-ARCH-003 — IPC Dual-channel Design
IPC shall use two independent channels:
- **High-frequency bus:** Shared memory (metering + waveform) — no pipe traffic for per-frame data
- **Command/event bus:** Named pipe (Windows) / Unix domain socket (macOS/Linux) — framed binary messages with JSON payloads

### REQ-ARCH-004 — UI Layer Separation
The UI layer shall be a separate process that communicates exclusively via the IPC interface. The UI receives shared memory object names from the engine at startup via `EVT_READY` — no hardcoded names on the UI side.

### REQ-ARCH-005 — Platform Abstraction Layer
All platform-specific code (thread priority, shared memory, named pipe vs Unix socket) shall be isolated behind a `platform/` abstraction layer in the C engine.

### REQ-ARCH-006 — Real-time Safety
No memory allocation, no syscalls, and no locks shall occur on the Audio I/O callback thread. All inter-thread communication on the real-time path shall use lock-free ring buffers and atomics (`stdatomic.h`).

### REQ-ARCH-007 — UI Framework
The UI shall be implemented in Kotlin Compose Multiplatform (desktop/JVM target), rendering via Skia/Skiko. Maximum 128 tracks supported in the UI.

---

## Track / Channel Model

### REQ-TRACK-001 — Track Objects
The engine shall support instantiation of one or more track (channel) objects. Each track represents an independent audio stream for recording or playback.

### REQ-TRACK-002 — Multi-channel Recording
The engine shall support simultaneous recording across multiple tracks, up to 128 channels.

### REQ-TRACK-003 — Track Naming
Each track shall carry a user-defined label. Track names shall be stored in file metadata (iXML `TRACK_LIST/TRACK/NAME`) and persisted in the session.

---

## Recording & Playback

### REQ-REC-001 — Record Command
The UI shall be able to arm tracks, then command the engine to begin and stop recording. Arming and recording are distinct states.

### REQ-REC-002 — Simultaneous Record and Playback
Recording and playback shall operate independently and simultaneously. The record head and playback head are independent 64-bit frame counters. The engine supports all combinations: record-only, playback-only, and simultaneous record+playback at different timeline positions.

### REQ-REC-003 — Timeline Model
The engine shall maintain a timeline model with independent record and playback heads. Playback position shall be settable via `CMD_LOCATE` without affecting recording.

### REQ-REC-004 — Transport Position Reporting
The engine shall broadcast current record and playback head positions to the UI at ~10Hz via `EVT_TRANSPORT` on the command/event bus. Positions are expressed in samples (frames) and as timecode string.

### REQ-REC-005 — Read-ahead Buffering
The Playback Engine shall maintain a per-track read-ahead buffer (target: 100ms) to ensure playback never blocks on disk I/O.

### REQ-REC-006 — Redundant Recording Targets
Each recording session shall support multiple simultaneous write destinations per track (e.g., local SSD + NAS + USB). The Disk Writer thread pool shall write identical audio data to all configured targets in parallel. Loss of one target shall generate a warning (`EVT_ERROR` severity `warning`) but shall not interrupt recording to remaining targets.

---

## Metadata & Standards

### REQ-META-001 — BWF / BEXT Chunk
All recorded files shall be written as Broadcast Wave Format (BWF) with a valid BEXT chunk containing:
- `Description` (user-provided or auto-generated)
- `Originator` (application name + version)
- `OriginatorReference` (unique ID per file)
- `OriginationDate` / `OriginationTime` (wall-clock at record start)
- `TimeReference` (sample offset from midnight, aligned to SMPTE timecode)
- `Version` = 1
- `UMID` (optional, zero-padded if not used)
- `CodingHistory`

### REQ-META-002 — iXML Chunk
All recorded files shall include an iXML chunk containing at minimum:
- `PROJECT`, `SCENE`, `TAKE`, `TAPE` fields
- `SPEED/TIMECODE_RATE`, `SPEED/TIMECODE_FLAG` (DF/NDF)
- `SPEED/FILE_SAMPLE_RATE`, `SPEED/AUDIO_BIT_DEPTH`
- `TRACK_LIST` with per-track `NAME`, `FUNCTION`, `INDEX`
- `BWFXML` compatibility wrapper

### REQ-META-003 — SMPTE Timecode
The engine shall support SMPTE timecode at the following rates: 23.976, 24, 25, 29.97 DF, 29.97 NDF, 30 DF, 30 NDF.
- Timecode shall be stamped into the BEXT `TimeReference` field on record start
- Timecode shall be reported in all `EVT_TRANSPORT` events as a formatted string
- The timecode subsystem shall be built around a `TimecodeSource` abstraction layer so the active source can be switched without changing downstream consumers (BEXT stamping, transport reporting, iXML, cue points)

#### REQ-META-003a — Timecode Source: Free-Run (implement first)
On record start, the engine shall latch wall-clock time, convert to SMPTE frames at the configured rate, and increment the timecode counter sample-accurately from that point. No external sync required.

#### REQ-META-003b — Timecode Source: LTC Chase (deferred)
The engine shall be able to decode SMPTE LTC from a designated audio input channel using **libltc**. When locked, the internal timecode shall chase the incoming LTC. The `TimecodeSource` abstraction shall route LTC-derived timecode to all downstream consumers transparently. A dedicated LTC decode step shall run within the Record Engine thread (or a dedicated LTC thread TBD) consuming the designated channel's ring buffer.

#### REQ-META-003c — Timecode Source: MTC Chase (deferred)
The engine shall accept MIDI Timecode quarter-frame messages from a configured MIDI input port. A dedicated MIDI input thread shall parse MTC and feed the `TimecodeSource`. When locked, MTC-derived timecode shall feed all downstream consumers via the same abstraction. MIDI I/O library TBD (candidate: **RtMidi** — C++ but clean C-callable API).

### REQ-META-004 — Cue Points
The engine shall support named cue points on the timeline:
- Cue points created via `CMD_SET_CUE` during or after recording
- Stored in the WAV `cue ` chunk and mirrored in iXML `BWFXML/MARKERS`
- Each cue point carries: position (frames), name, colour (for UI display)
- Cue points reported to UI via `EVT_CUE_UPDATE`

---

## Audio Format

### REQ-FMT-001 — Sample Rate Support
The engine shall support recording and playback at the following sample rates:

| Rate | Use |
|---|---|
| 44,100 Hz | CD / consumer |
| 48,000 Hz | Broadcast / film (default) |
| 88,200 Hz | High-res 2× |
| 96,000 Hz | High-res 2× (broadcast) |
| 176,400 Hz | High-res 4× |
| 192,000 Hz | High-res 4× (maximum) |

- Sample rate is configured per session via `CMD_SESSION_INIT` (`"sample_rate"` field).
- All internal processing (ring buffers, metering, waveform decimation, timecode) is sample-rate agnostic — parameters are passed as arguments, not hardcoded.
- Ring buffer headroom at 192kHz: 8192 frames ≈ 43ms — adequate for all thread scheduling jitter.
- The waveform shared memory region header (`WavRecWfmRegion.sample_rate`) shall be updated from the session config at session init, not hardcoded to 48kHz.
- BWF BEXT `TimeReference`, `CodingHistory`, and the `fmt` chunk byte-rate/block-align fields shall all reflect the actual configured sample rate.

### REQ-FMT-002 — Bit Depth Support
The engine shall support the following bit depths for recording:

| Depth | Format | WAV `AudioFormat` |
|---|---|---|
| 16-bit integer | Signed PCM, 2 bytes/sample LE | `1` (PCM) |
| 24-bit integer | Signed PCM, 3 bytes/sample LE | `1` (PCM) — **default** |
| 32-bit integer | Signed PCM, 4 bytes/sample LE | `1` (PCM) |
| 32-bit float   | IEEE 754, 4 bytes/sample LE   | `3` (WAVE_FORMAT_IEEE_FLOAT) |

- All internal engine processing uses `float32` throughout (ring buffers, metering, waveform). Conversion to the target bit depth occurs only in the Disk Writer immediately before writing to file.
- Conversion table:
  - **16-bit:** `clamp(f, -1, 1) × 32767` → 2-byte LE signed integer
  - **24-bit:** `clamp(f, -1, 1) × 8388607` → 3-byte LE signed integer
  - **32-bit int:** `clamp(f, -1, 1) × 2147483647` → 4-byte LE signed integer
  - **32-bit float:** passthrough — write raw `float` bytes (no clamping or scaling)
- The `bwf_write_header` function shall write `AudioFormat = 3` when bit depth is 32-bit float, `1` otherwise.
- The BEXT `CodingHistory` and iXML `SPEED/AUDIO_BIT_DEPTH` shall reflect the actual configured bit depth.
- Playback and monitoring paths shall support all four bit depths for reading recorded files.

---

## Mixer & Routing

### REQ-MIX-001 — Audio Mixer
The engine shall include a basic audio mixer capable of combining multiple track signals.

### REQ-MIX-002 — Audio Routing Engine
The engine shall include a routing engine that allows flexible assignment of input sources to tracks and track outputs to hardware outputs or the mixer.

---

## Transcription

### REQ-TRX-001 — On-the-fly Transcription
The engine shall support real-time transcription of one or more tracks during an active recording operation. Transcription shall run in a dedicated Transcription thread, fed by a ring buffer off the Record Engine, so that it cannot impede audio I/O or disk writing.

### REQ-TRX-002 — Transcription Library
Candidate library: **whisper.cpp** (C/C++ API, runs locally, no network dependency). Model size and language shall be configurable via `CMD_TRANSCRIPTION_CONFIG`.

### REQ-TRX-003 — Transcription Results to UI
Transcription results (word-level or segment-level) shall be sent to the UI via `EVT_TRANSCRIPTION` on the command/event bus, carrying: track_id, timeline position (frames), and text.

### REQ-TRX-004 — Transcription Storage
Transcription results shall be persisted alongside the session (format TBD: sidecar JSON or embedded in iXML markers).

---

## Annotation

### REQ-ANN-001 — Annotation Support
The application shall support user-created text annotations anchored to timeline positions. Annotations are distinct from auto-generated transcription and cue points.

---

## Status

### Completed
- [x] Architecture design (`docs/ENGINE_THREADING.md`)
- [x] IPC protocol definition (`docs/IPC_PROTOCOL.md`)
- [x] C engine scaffold (CMake, VS2019, MSVC stdatomic shim, platform abstraction)
- [x] Lock-free SPSC ring buffer (`util/ringbuf.h`) + AudioRing (`audio/audio_io.h`)
- [x] IPC shared memory regions — metering (4KB double-buffer) + waveform (96KB SPSC ring)
- [x] Audio I/O thread — miniaudio WASAPI duplex, de-interleave to per-track rings, mix-bus output
- [x] Record Engine thread — drains input rings, distributes raw float32 to disk/meter/wfm/txcr feeds (no gain)
- [x] Metering thread — 60Hz peak/RMS/peak-hold/clip, writes to shm double-buffer
- [x] Waveform thread — 100Hz min/max decimation blocks, writes to shm SPSC ring
- [x] Disk Writer thread — float32 → PCM24 conversion, BWF/BEXT header, redundant targets, EVT_DISK_STATUS
- [x] BWF/BEXT writer — full RIFF/WAVE header, BEXT v1 (EBU Tech 3285), CodingHistory
- [x] SMPTE timecode — free-run latch from RTC, all 7 rates, DF/NDF, formatter, parser
- [x] TimecodeSource abstraction — LTC/MTC deferred but architecture in place
- [x] IPC command/event bus — Rx/Tx threads, MPSC event queue, CRC32 framing, EVT_TRANSPORT
- [x] Engine state machine — IDLE/ARMED/RECORDING/PLAYING/RECORD_PLAY/STOPPING/ERROR
- [x] Track model + track naming
- [x] Platform layer — Win32 + POSIX (threads, shm, named pipe/socket, mkdir_p, free_bytes)
- [x] Kotlin CMP UI scaffold (gradle, version catalog, JNA metering reader)
- [x] UI metering display — JNA shm reader, 60Hz poll, VU meter canvas (peak/RMS/hold/clip/colour zones)
- [x] UI transport controls — Record/Play/Stop buttons, timecode display, engine state chip
- [x] UI track list — scrollable, arm button, track name, HW input badge, per-track VU meter
- [x] UI status bar — disk free space, write rate, remaining time estimate, error display
- [x] `EngineRunner` — process launch, pipe connect with retry, binary framing, CRC validation

### In Progress / Next
- [ ] **REQ-FMT-001/002** — multi-rate + multi-depth: update `ipc_shm.c` wfm sample_rate init; add 16/32-bit int + float32 disk write paths; update `bwf_write_header` AudioFormat field
- [ ] Playback engine thread + read-ahead buffer
- [ ] Mixer / routing engine + monitoring gain (gain intentionally excluded from record path)
- [ ] BWF/BEXT — iXML chunk writer (ixml.c stub → full implementation)
- [ ] SMPTE Timecode sources — LTC (libltc) + MTC (RtMidi)
- [ ] Cue point subsystem (CMD_SET_CUE / EVT_CUE_UPDATE)
- [ ] Transcription thread (whisper.cpp integration)
- [ ] Annotation subsystem
- [ ] UI waveform display (shared memory waveform ring reader)
- [ ] UI cue point display + creation
- [ ] UI transcription display
- [ ] CMD_SESSION_INIT JSON parsing in engine (currently stub)
- [ ] CMD_TRACK_CONFIG JSON parsing in engine (currently stub)
- [ ] Device enumeration exposed to UI (EVT_DEVICE_LIST)
