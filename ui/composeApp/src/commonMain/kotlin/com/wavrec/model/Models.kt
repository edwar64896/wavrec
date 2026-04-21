package com.wavrec.model

/* -------------------------------------------------------------------------
 * Audio device
 * ---------------------------------------------------------------------- */

data class AudioDevice(
    val name     : String,
    val isDefault: Boolean,
)

/* -------------------------------------------------------------------------
 * Per-channel metering snapshot (read from shared memory each frame)
 * ---------------------------------------------------------------------- */

data class ChannelMeter(
    val truePeak : Float  = 0f,   /* linear 0–1 */
    val rms      : Float  = 0f,
    val peakHold : Float  = 0f,
    val clip     : Boolean = false,
    val active   : Boolean = false,
)

/* -------------------------------------------------------------------------
 * Waveform block (from shared memory waveform ring)
 * ---------------------------------------------------------------------- */

data class WfmBlock(
    val channelId    : Int,
    val timelineFrame: Long,
    val nSamples     : Int,
    val min          : Float,
    val max          : Float,
)

/* -------------------------------------------------------------------------
 * Track configuration state
 * ---------------------------------------------------------------------- */

enum class TrackAlert { NONE, LOW_SIGNAL, NEAR_CLIP, CLIPPING }

/* -------------------------------------------------------------------------
 * Folder — groups tracks into a single poly WAV output, with its own set
 * of target disks.  Track membership is ordered: position in trackIds
 * defines channel order in the written file.
 * ---------------------------------------------------------------------- */

data class Folder(
    val id       : Int,
    val name     : String        = "Main",
    val trackIds : List<Int>     = emptyList(),
    val targets  : List<String>  = emptyList(),
    val collapsed: Boolean       = false,
)

data class TrackState(
    val id       : Int,
    val label    : String   = "Track ${id + 1}",
    val hwInput  : Int      = id,
    val armed    : Boolean  = false,
    val preArmed : Boolean? = null,   /* null = no pending change; true/false = pending during recording */
    val monitor  : Boolean  = false,
    val gainDb   : Float    = 0f,

    /* Updated from shared memory each display frame */
    val meter   : ChannelMeter = ChannelMeter(),

    /* Alert state — computed in the metering loop.
     *   alert          : most-severe current alert (or NONE)
     *   silentSinceMs  : first timestamp the channel went below -100 dBFS
     *                    while armed or monitored (null = has signal)
     *   nearClipUntilMs: hold near-clip state visible until this timestamp
     *   clipLatched    : latched clip bit (cleared only by Clear Alerts) */
    val alert           : TrackAlert = TrackAlert.NONE,
    val silentSinceMs   : Long?      = null,
    val nearClipUntilMs : Long       = 0L,
    val clipLatched     : Boolean    = false,
)

/* -------------------------------------------------------------------------
 * Transport / playhead state
 * ---------------------------------------------------------------------- */

data class TransportState(
    val recording        : Boolean = false,
    val playing          : Boolean = false,
    val recordHeadFrames : Long    = 0L,
    val playHeadFrames   : Long    = 0L,
    val timecode         : String  = "00:00:00:00",
)

/* -------------------------------------------------------------------------
 * Recording target health
 * ---------------------------------------------------------------------- */

data class DiskTarget(
    val path        : String,
    val freeBytes   : Long,
    val writeRateBps: Long,
    val ok          : Boolean,
)

/* -------------------------------------------------------------------------
 * Top-level engine state
 * ---------------------------------------------------------------------- */

enum class EngineConnectionState { DISCONNECTED, CONNECTING, CONNECTED }

data class EngineState(
    val connection      : EngineConnectionState = EngineConnectionState.DISCONNECTED,
    val engineState     : String                = "IDLE",
    val sampleRate      : Int                   = 48000,
    val sampleFormat    : String                = "pcm24",
    val timecodeRate    : String                = "25",    /* wire format: 23.976/24/25/29.97/30 */
    val timecodeDrop    : Boolean               = false,
    val preRollSeconds  : Float                 = 0f,     /* 0 = disabled */
    /* Scene/Take: integers, displayed as %02d / %03d, used in filenames.
     * Scene change auto-resets Take to 1.  Punch (record while recording)
     * auto-advances Take and starts a fresh file set. */
    val sceneNum        : Int                   = 1,
    val takeNum         : Int                   = 1,
    val tracks          : List<TrackState>      = emptyList(),
    val folders         : List<Folder>          = emptyList(),
    val transport       : TransportState        = TransportState(),
    val diskTargets     : List<DiskTarget>      = emptyList(),
    /* Last commit timestamp (ms since epoch) per (folderId, targetIdx).
     * Updated by EVT_TARGET_COMMIT; drives the per-target disk-chip flash. */
    val targetCommits   : Map<Pair<Int, Int>, Long> = emptyMap(),
    val remainingSecs   : Long                  = 0L,
    val lastError       : String?               = null,
    /* Device config */
    val devices         : List<AudioDevice>     = emptyList(),
    val selectedDevice  : String                = "",
)
