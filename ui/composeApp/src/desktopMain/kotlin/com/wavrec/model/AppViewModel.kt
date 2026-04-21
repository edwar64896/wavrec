package com.wavrec.model

import com.wavrec.engine.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import java.io.File

class AppViewModel(
    private val engineExePath: String,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    private val runner         = EngineRunner(engineExePath, scope)
    private val meteringReader = MeteringReader()
    private val wfmReader      = WfmReader()
    val waveforms              = WaveformBuffers()

    /* Session config — updated when user changes settings */
    private var currentDevice = ""
    private var currentTargets   = listOf<String>()

    private val _state = MutableStateFlow(EngineState())
    val state: StateFlow<EngineState> = _state.asStateFlow()

    init { collectEvents() }

    fun start() {
        _state.update { it.copy(connection = EngineConnectionState.CONNECTING) }
        runner.launch()
    }

    fun shutdown() {
        meteringReader.close()
        wfmReader.close()
        runner.shutdown()
        scope.cancel()
    }

    /* ------------------------------------------------------------------
     * Event collection
     * ------------------------------------------------------------------ */

    private fun collectEvents() {
        scope.launch {
            runner.events.collect { event ->
                when (event) {
                    is EngineEvent.Ready -> {
                        /* Don't auto-select any device — let the user pick explicitly.
                         * Auto-opening the first ASIO driver can crash buggy drivers
                         * before the user can switch to a working one. */
                        _state.update { it.copy(
                            connection     = EngineConnectionState.CONNECTED,
                            sampleRate     = event.sampleRate,
                            devices        = event.devices.map { d ->
                                AudioDevice(d.name, d.isDefault)
                            },
                            selectedDevice = "",
                        )}
                        meteringReader.open(event.sharedMemMeters)
                        wfmReader.open(event.sharedMemWaveforms)
                        startMeteringLoop()
                        startWaveformLoop()
                        sendDefaultSession()
                    }
                    is EngineEvent.StateChange ->
                        _state.update { it.copy(engineState = event.newState) }
                    is EngineEvent.Transport ->
                        _state.update { s -> s.copy(transport = TransportState(
                            recording        = event.recording,
                            playing          = event.playing,
                            recordHeadFrames = event.recordHeadFrames,
                            playHeadFrames   = event.playHeadFrames,
                            timecode         = event.timecode,
                        ))}
                    is EngineEvent.DiskStatus ->
                        _state.update { s -> s.copy(
                            diskTargets   = event.targets.map { t ->
                                DiskTarget(t.path, t.freeBytes, t.writeRateBps, t.ok)
                            },
                            remainingSecs = event.remainingSecs,
                        )}
                    is EngineEvent.EngineError ->
                        _state.update { it.copy(
                            lastError = "[${event.severity}] ${event.message}"
                        )}
                    is EngineEvent.Log ->
                        _state.update { it.copy(lastError = "[${event.level}] ${event.message}") }
                    else -> {}
                }
            }
        }
    }

    private fun startWaveformLoop() {
        scope.launch(Dispatchers.IO) {
            /* Clear any stale state so display starts fresh. */
            waveforms.reset()
            /* Track recording transitions so we can wipe history on new take. */
            var wasRecording = false
            while (isActive) {
                val recording = _state.value.transport.recording
                if (recording && !wasRecording) waveforms.reset()
                wasRecording = recording
                wfmReader.drain { b ->
                    waveforms.append(b.channelId, b.min, b.max)
                }
                delay(20)   /* ~50Hz — enough headroom for 64 tracks at 93Hz/track */
            }
        }
    }

    private fun startMeteringLoop() {
        scope.launch {
            while (isActive) {
                val n = _state.value.tracks.size.coerceAtLeast(1)
                val meters = meteringReader.read(n)
                val now    = System.currentTimeMillis()
                _state.update { s ->
                    s.copy(tracks = s.tracks.mapIndexed { i, t ->
                        val m = if (i < meters.size) meters[i] else t.meter
                        computeAlertState(t, m, now)
                    })
                }
                delay(16)
            }
        }
    }

    /** Alert thresholds (user-configurable later). */
    companion object {
        /* -100 dBFS linear = 10^-5 */
        private const val LOW_SIGNAL_LINEAR   = 0.00001f
        private const val LOW_SIGNAL_HOLD_MS  = 5_000L
        /* -3 dBFS linear ≈ 0.708 — below clip but getting close. */
        private const val NEAR_CLIP_LINEAR    = 0.708f
        /* Keep near-clip icon lit for 500ms after the last hot sample so
         * transients don't flicker the indicator. */
        private const val NEAR_CLIP_HOLD_MS   = 500L
    }

    private fun computeAlertState(t: TrackState, m: ChannelMeter, now: Long): TrackState {
        val watching = t.armed || t.monitor

        /* Silent detection — only runs when the channel is expected to have signal. */
        val silentSince = when {
            !watching                         -> null
            m.truePeak >= LOW_SIGNAL_LINEAR   -> null
            t.silentSinceMs == null           -> now
            else                              -> t.silentSinceMs
        }
        val lowSignal = silentSince != null &&
                        (now - silentSince) >= LOW_SIGNAL_HOLD_MS

        /* Near-clip detection with hold. */
        val nearClipUntil = if (m.truePeak >= NEAR_CLIP_LINEAR)
            now + NEAR_CLIP_HOLD_MS
        else t.nearClipUntilMs

        val nearClip = now < nearClipUntil

        /* Clip is latched via meter.clip; stays set until Clear Alerts. */
        val clipLatched = t.clipLatched || m.clip

        val alert = when {
            clipLatched -> TrackAlert.CLIPPING
            nearClip    -> TrackAlert.NEAR_CLIP
            lowSignal   -> TrackAlert.LOW_SIGNAL
            else        -> TrackAlert.NONE
        }

        return t.copy(
            meter           = m,
            alert           = alert,
            silentSinceMs   = silentSince,
            nearClipUntilMs = nearClipUntil,
            clipLatched     = clipLatched,
        )
    }

    /* ------------------------------------------------------------------
     * Default startup
     * ------------------------------------------------------------------ */

    private fun sendDefaultSession() {
        val defaultTarget = "${System.getProperty("user.home")}${File.separator}WavRec"
        File(defaultTarget).mkdirs()
        currentTargets = listOf(defaultTarget)
        pushSessionInit()
        repeat(8) { i -> addTrack("Track ${i + 1}", hwInput = i) }
    }

    /* ------------------------------------------------------------------
     * Session / device
     * ------------------------------------------------------------------ */

    fun setDevice(name: String) {
        val logFile = java.io.File(System.getProperty("user.home"), "WavRec/ui.log")
        logFile.appendText("[${System.currentTimeMillis()}] setDevice('$name') called\n")
        currentDevice = name
        _state.update { it.copy(selectedDevice = name) }
        pushSessionInit()
    }

    fun setSampleRate(sr: Int) {
        _state.update { it.copy(sampleRate = sr) }
        pushSessionInit(sampleRate = sr)
    }

    fun setSampleFormat(fmt: String) {
        _state.update { it.copy(sampleFormat = fmt) }
        pushSessionInit(sampleFormat = fmt)
    }

    /** `rate` is the wire format: "23.976" / "24" / "25" / "29.97" / "30".
     *  `drop` only applies to 29.97 and 30. */
    fun setTimecodeRate(rate: String, drop: Boolean) {
        _state.update { it.copy(timecodeRate = rate, timecodeDrop = drop) }
        pushSessionInit(tcRate = rate, tcDrop = drop)
    }

    private fun pushSessionInit(
        sampleRate  : Int    = _state.value.sampleRate,
        sampleFormat: String = _state.value.sampleFormat,
        bufferFrames: Int    = 512,
        scene       : String = "001",
        take        : String = "01",
        tape        : String = "A001",
        tcRate      : String = _state.value.timecodeRate,
        tcDrop      : Boolean = _state.value.timecodeDrop,
    ) {
        val targetsJson = currentTargets.joinToString(",") { "\"${it.replace("\\", "\\\\")}\"" }
        val dev = currentDevice.replace("\"", "\\\"")
        val payload = """{"sample_rate":$sampleRate,"sample_format":"$sampleFormat","buffer_frames":$bufferFrames,"device":"$dev","timecode_rate":"$tcRate","timecode_df":$tcDrop,"scene":"$scene","take":"$take","tape":"$tape","record_targets":[$targetsJson]}"""
        runner.sendCommand(MsgType.CMD_SESSION_INIT, payload)
    }

    /* ------------------------------------------------------------------
     * Track management
     * ------------------------------------------------------------------ */

    fun addTrack(label: String = "Track ${_state.value.tracks.size + 1}",
                 hwInput: Int  = _state.value.tracks.size) {
        val id = _state.value.tracks.size
        _state.update { s -> s.copy(tracks = s.tracks + TrackState(id = id, label = label, hwInput = hwInput)) }
        pushTrack(id)
    }

    fun removeTrack(trackId: Int) {
        /* Remove from UI — renumber remaining tracks and re-push all */
        val updated = _state.value.tracks
            .filter  { it.id != trackId }
            .mapIndexed { i, t -> t.copy(id = i) }
        _state.update { it.copy(tracks = updated) }
        pushAllTracks(updated)
    }

    fun setTrackLabel(trackId: Int, label: String) {
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            if (t.id == trackId) t.copy(label = label) else t
        })}
        pushTrack(trackId)
    }

    fun setTrackInput(trackId: Int, hwInput: Int) {
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            if (t.id == trackId) t.copy(hwInput = hwInput.coerceIn(0, 127)) else t
        })}
        pushTrack(trackId)
    }

    fun setMonitor(trackId: Int, enabled: Boolean) {
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            if (t.id == trackId) t.copy(monitor = enabled) else t
        })}
        pushTrack(trackId)
    }

    fun armTrack(trackId: Int, armed: Boolean) {
        val cmd = if (armed) MsgType.CMD_ARM else MsgType.CMD_DISARM
        runner.sendCommand(cmd, """{"tracks":[$trackId]}""")
        val isRecording = _state.value.transport.recording
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            if (t.id == trackId) {
                /* UI mirrors engine-side auto-monitor on arm. */
                val newMonitor = if (armed) true else t.monitor
                if (isRecording)
                    t.copy(preArmed = armed, monitor = newMonitor)
                else
                    t.copy(armed = armed, preArmed = null, monitor = newMonitor)
            } else t
        })}
    }

    /* ------------------------------------------------------------------
     * Transport
     * ------------------------------------------------------------------ */

    fun record() {
        val isRecording = _state.value.transport.recording
        if (isRecording) {
            /* Punch: apply pending pre-arm changes to local state, engine handles the restart. */
            _state.update { s -> s.copy(tracks = s.tracks.map { t ->
                val newArmed = t.preArmed ?: t.armed
                t.copy(armed = newArmed, preArmed = null)
            })}
        }
        runner.sendCommand(MsgType.CMD_RECORD)
    }

    fun play()       = runner.sendCommand(MsgType.CMD_PLAY, """{"position_frames":0}""")

    fun stop() {
        runner.sendCommand(MsgType.CMD_STOP)
        /* Clear all pending pre-arm state on stop. */
        _state.update { s -> s.copy(tracks = s.tracks.map { t -> t.copy(preArmed = null) }) }
    }

    fun resetClips() = runner.sendCommand(MsgType.CMD_METER_RESET)

    /* ------------------------------------------------------------------
     * Bulk track actions (header row)
     * ------------------------------------------------------------------ */

    /** Arm (or disarm) every configured track in one IPC round-trip. */
    fun armAllTracks(armed: Boolean) {
        val cmd = if (armed) MsgType.CMD_ARM else MsgType.CMD_DISARM
        /* Engine treats an empty tracks array as "all tracks". */
        runner.sendCommand(cmd, """{"tracks":[]}""")
        val isRecording = _state.value.transport.recording
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            val newMonitor = if (armed) true else t.monitor
            if (isRecording)
                t.copy(preArmed = armed, monitor = newMonitor)
            else
                t.copy(armed = armed, preArmed = null, monitor = newMonitor)
        })}
    }

    /** Enable (or disable) monitor on every track. */
    fun monitorAllTracks(enabled: Boolean) {
        val updated = _state.value.tracks.map { it.copy(monitor = enabled) }
        _state.update { it.copy(tracks = updated) }
        pushAllTracks(updated)
    }

    /** Clear latched clip bits (engine side) and reset UI alert state. */
    fun clearAllAlerts() {
        runner.sendCommand(MsgType.CMD_METER_RESET)
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            t.copy(
                alert           = TrackAlert.NONE,
                silentSinceMs   = null,
                nearClipUntilMs = 0L,
                clipLatched     = false,
            )
        })}
    }

    /* ------------------------------------------------------------------
     * Internal helpers
     * ------------------------------------------------------------------ */

    private fun pushTrack(trackId: Int) {
        val t = _state.value.tracks.firstOrNull { it.id == trackId } ?: return
        val payload = """{"tracks":[{"id":${t.id},"label":"${t.label.replace("\"","\\\"")}", "hw_input":${t.hwInput},"gain_db":${t.gainDb},"armed":${t.armed},"monitor":${t.monitor}}]}"""
        runner.sendCommand(MsgType.CMD_TRACK_CONFIG, payload)
    }

    private fun pushAllTracks(tracks: List<TrackState>) {
        if (tracks.isEmpty()) return
        val arr = tracks.joinToString(",") { t ->
            """{"id":${t.id},"label":"${t.label.replace("\"","\\\"")}", "hw_input":${t.hwInput},"gain_db":${t.gainDb},"armed":${t.armed},"monitor":${t.monitor}}"""
        }
        runner.sendCommand(MsgType.CMD_TRACK_CONFIG, """{"tracks":[$arr]}""")
    }
}
