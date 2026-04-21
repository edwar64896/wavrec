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
                    is EngineEvent.TargetCommit -> {
                        val now = System.currentTimeMillis()
                        _state.update { s ->
                            val stamped = s.targetCommits.toMutableMap()
                            event.commits.forEach { c ->
                                stamped[c.folderId to c.targetIdx] = now
                            }
                            s.copy(targetCommits = stamped)
                        }
                    }
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
        /* Default layout: one "Main" folder that every track lands in. */
        _state.update { it.copy(folders = listOf(
            Folder(id = 0, name = "Main", trackIds = emptyList(), targets = listOf(defaultTarget))
        )) }
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

    fun setPreRoll(seconds: Float) {
        _state.update { it.copy(preRollSeconds = seconds) }
        pushSessionInit(preRoll = seconds)
    }

    /** Scene 01–99.  Changing scene resets take to 1 (industry convention). */
    fun setScene(n: Int) {
        val clamped = n.coerceIn(1, 99)
        _state.update { it.copy(sceneNum = clamped, takeNum = 1) }
        pushSessionInit()
    }

    /** Take 001–999. */
    fun setTake(n: Int) {
        val clamped = n.coerceIn(1, 999)
        _state.update { it.copy(takeNum = clamped) }
        pushSessionInit()
    }

    private fun pushSessionInit(
        sampleRate  : Int    = _state.value.sampleRate,
        sampleFormat: String = _state.value.sampleFormat,
        bufferFrames: Int    = 512,
        scene       : String = "%02d".format(_state.value.sceneNum),
        take        : String = "%03d".format(_state.value.takeNum),
        tape        : String = "A001",
        tcRate      : String = _state.value.timecodeRate,
        tcDrop      : Boolean = _state.value.timecodeDrop,
        preRoll     : Float  = _state.value.preRollSeconds,
    ) {
        val dev = currentDevice.replace("\"", "\\\"")

        /* Serialize folders with tracks + per-folder targets.  If no folders
         * have been set up yet we fall back to a flat record_targets payload
         * so the engine's legacy path still opens a default "Main" folder. */
        val folders = _state.value.folders
        val folderJson = if (folders.isEmpty()) "" else folders.joinToString(",") { f ->
            val name    = f.name.replace("\"", "\\\"")
            val trackIds = f.trackIds.joinToString(",")
            val tgts    = f.targets.joinToString(",") { "\"${it.replace("\\", "\\\\")}\"" }
            """{"id":${f.id},"name":"$name","track_ids":[$trackIds],"targets":[$tgts]}"""
        }

        val payload = if (folderJson.isNotEmpty()) {
            """{"sample_rate":$sampleRate,"sample_format":"$sampleFormat","buffer_frames":$bufferFrames,"device":"$dev","timecode_rate":"$tcRate","timecode_df":$tcDrop,"pre_roll_seconds":$preRoll,"scene":"$scene","take":"$take","tape":"$tape","folders":[$folderJson]}"""
        } else {
            val targetsJson = currentTargets.joinToString(",") { "\"${it.replace("\\", "\\\\")}\"" }
            """{"sample_rate":$sampleRate,"sample_format":"$sampleFormat","buffer_frames":$bufferFrames,"device":"$dev","timecode_rate":"$tcRate","timecode_df":$tcDrop,"pre_roll_seconds":$preRoll,"scene":"$scene","take":"$take","tape":"$tape","record_targets":[$targetsJson]}"""
        }
        runner.sendCommand(MsgType.CMD_SESSION_INIT, payload)
    }

    /* ------------------------------------------------------------------
     * Track management
     * ------------------------------------------------------------------ */

    fun addTrack(label: String = "Track ${_state.value.tracks.size + 1}",
                 hwInput: Int  = _state.value.tracks.size,
                 folderId: Int = 0) {
        val id = _state.value.tracks.size
        _state.update { s ->
            val withTrack = s.copy(tracks = s.tracks + TrackState(id = id, label = label, hwInput = hwInput))
            /* Add to the requested folder (default: first folder). */
            val folders = withTrack.folders.ifEmpty { listOf(Folder(id = 0, name = "Main")) }
            val updated = folders.map { f ->
                if (f.id == folderId) f.copy(trackIds = f.trackIds + id) else f
            }
            withTrack.copy(folders = updated)
        }
        pushTrack(id)
        /* Push the new session so the engine knows this track belongs to the folder. */
        pushSessionInit()
    }

    fun removeTrack(trackId: Int) {
        /* Remove from UI — renumber remaining tracks and re-push all. Folder
         * track_ids are remapped to the new ids (drop removed, shift higher). */
        val oldTracks = _state.value.tracks
        val keptIds   = oldTracks.filter { it.id != trackId }.map { it.id }
        val idRemap   = keptIds.mapIndexed { new, old -> old to new }.toMap()

        val updatedTracks = keptIds.map { old -> oldTracks.first { it.id == old }.copy(id = idRemap.getValue(old)) }
        val updatedFolders = _state.value.folders.map { f ->
            f.copy(trackIds = f.trackIds.mapNotNull { idRemap[it] })
        }
        _state.update { it.copy(tracks = updatedTracks, folders = updatedFolders) }
        pushAllTracks(updatedTracks)
        pushSessionInit()
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
            /* Punch: finalise current take, auto-advance take number, start a
             * new take.  Apply pending pre-arm changes to local state — the
             * engine closes the file set, reopens with the new take embedded
             * in the filename, and we already plumbed scene/take through
             * CMD_SESSION_INIT. */
            val nextTake = (_state.value.takeNum + 1).coerceAtMost(999)
            _state.update { s -> s.copy(
                takeNum = nextTake,
                tracks  = s.tracks.map { t ->
                    val newArmed = t.preArmed ?: t.armed
                    t.copy(armed = newArmed, preArmed = null)
                },
            )}
            /* Push the new take BEFORE CMD_RECORD so the engine's disk writer
             * sees it when it opens the fresh file set. */
            pushSessionInit()
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
     * Folder management
     * ------------------------------------------------------------------ */

    fun addFolder(name: String = "Folder ${_state.value.folders.size + 1}") {
        val newId = (_state.value.folders.maxOfOrNull { it.id } ?: -1) + 1
        _state.update { it.copy(folders = it.folders + Folder(
            id = newId, name = name, trackIds = emptyList(),
            /* Seed the new folder with the first existing folder's targets. */
            targets = it.folders.firstOrNull()?.targets ?: currentTargets,
        )) }
        pushSessionInit()
    }

    fun removeFolder(folderId: Int) {
        val folders = _state.value.folders
        if (folders.size <= 1) return               /* keep at least one */
        val victim = folders.firstOrNull { it.id == folderId } ?: return
        val destination = folders.first { it.id != folderId }
        val updated = folders.mapNotNull { f ->
            when (f.id) {
                folderId          -> null
                destination.id    -> f.copy(trackIds = f.trackIds + victim.trackIds)
                else              -> f
            }
        }
        _state.update { it.copy(folders = updated) }
        pushSessionInit()
    }

    fun renameFolder(folderId: Int, name: String) {
        _state.update { s -> s.copy(folders = s.folders.map { f ->
            if (f.id == folderId) f.copy(name = name) else f
        })}
        pushSessionInit()
    }

    fun setFolderCollapsed(folderId: Int, collapsed: Boolean) {
        _state.update { s -> s.copy(folders = s.folders.map { f ->
            if (f.id == folderId) f.copy(collapsed = collapsed) else f
        })}
        /* Collapse state is UI-only — don't push session. */
    }

    fun setFolderTargets(folderId: Int, targets: List<String>) {
        _state.update { s -> s.copy(folders = s.folders.map { f ->
            if (f.id == folderId) f.copy(targets = targets) else f
        })}
        pushSessionInit()
    }

    /** Move [trackId] up (-1) or down (+1) within its current folder.
     *  No-op if the track is already at the boundary. */
    fun reorderTrackInFolder(trackId: Int, dir: Int) {
        var changed = false
        _state.update { s ->
            val updated = s.folders.map { f ->
                val idx = f.trackIds.indexOf(trackId)
                if (idx < 0) return@map f
                val newIdx = idx + dir
                if (newIdx < 0 || newIdx >= f.trackIds.size) return@map f
                changed = true
                val ids = f.trackIds.toMutableList()
                ids.removeAt(idx)
                ids.add(newIdx, trackId)
                f.copy(trackIds = ids)
            }
            s.copy(folders = updated)
        }
        if (changed) pushSessionInit()
    }

    /** Move [trackId] into [destFolderId], appending at the end. */
    fun moveTrackToFolder(trackId: Int, destFolderId: Int) {
        _state.update { s ->
            val cleaned = s.folders.map { f -> f.copy(trackIds = f.trackIds - trackId) }
            val updated = cleaned.map { f ->
                if (f.id == destFolderId) f.copy(trackIds = f.trackIds + trackId) else f
            }
            s.copy(folders = updated)
        }
        pushSessionInit()
    }

    /** Arm or disarm every track in the given folder. */
    fun armFolder(folderId: Int, armed: Boolean) {
        val ids = _state.value.folders.firstOrNull { it.id == folderId }?.trackIds ?: return
        if (ids.isEmpty()) return
        val cmd = if (armed) MsgType.CMD_ARM else MsgType.CMD_DISARM
        runner.sendCommand(cmd, """{"tracks":[${ids.joinToString(",")}]}""")
        val isRecording = _state.value.transport.recording
        _state.update { s -> s.copy(tracks = s.tracks.map { t ->
            if (t.id !in ids) t else {
                val newMonitor = if (armed) true else t.monitor
                if (isRecording)
                    t.copy(preArmed = armed, monitor = newMonitor)
                else
                    t.copy(armed = armed, preArmed = null, monitor = newMonitor)
            }
        })}
    }

    /** Enable or disable monitor on every track in the folder. */
    fun monitorFolder(folderId: Int, enabled: Boolean) {
        val ids = _state.value.folders.firstOrNull { it.id == folderId }?.trackIds ?: return
        val updated = _state.value.tracks.map { t ->
            if (t.id in ids) t.copy(monitor = enabled) else t
        }
        _state.update { it.copy(tracks = updated) }
        pushAllTracks(updated.filter { it.id in ids })
    }

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
