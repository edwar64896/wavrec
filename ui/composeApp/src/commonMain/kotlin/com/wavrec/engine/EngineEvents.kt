package com.wavrec.engine

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.*

/* -------------------------------------------------------------------------
 * Wire message types (mirrors WavRecMsgType in ipc_protocol.h)
 * ---------------------------------------------------------------------- */

object MsgType {
    /* Commands (UI → Engine) */
    const val CMD_PING                 = 0x01
    const val CMD_SESSION_INIT         = 0x02
    const val CMD_TRACK_CONFIG         = 0x03
    const val CMD_ARM                  = 0x10
    const val CMD_DISARM               = 0x11
    const val CMD_RECORD               = 0x12
    const val CMD_PLAY                 = 0x13
    const val CMD_STOP                 = 0x14
    const val CMD_LOCATE               = 0x15
    const val CMD_METER_RESET          = 0x16
    const val CMD_SET_CUE              = 0x17
    const val CMD_TRANSCRIPTION_CONFIG = 0x19
    const val CMD_SHUTDOWN             = 0x1F

    /* Events (Engine → UI) */
    const val EVT_PONG                 = 0x81
    const val EVT_READY                = 0x82
    const val EVT_STATE_CHANGE         = 0x83
    const val EVT_TRANSPORT            = 0x84
    const val EVT_TRACK_STATE          = 0x85
    const val EVT_SESSION_INFO         = 0x86
    const val EVT_DISK_STATUS          = 0x87
    const val EVT_CUE_UPDATE           = 0x88
    const val EVT_TRANSCRIPTION        = 0x89
    const val EVT_TARGET_COMMIT        = 0x8A
    const val EVT_ERROR                = 0xE0
    const val EVT_LOG                  = 0xE1
}

/* -------------------------------------------------------------------------
 * Parsed event sealed hierarchy
 * ---------------------------------------------------------------------- */

sealed class EngineEvent {
    data class DeviceInfo(
        val name     : String,
        val isDefault: Boolean,
    )

    data class Ready(
        val engineVersion     : String,
        val maxChannels       : Int,
        val sharedMemMeters   : String,
        val sharedMemWaveforms: String,
        val sampleRate        : Int,
        val devices           : List<DeviceInfo> = emptyList(),
    ) : EngineEvent()

    data class StateChange(val prevState: String, val newState: String) : EngineEvent()

    data class Transport(
        val recordHeadFrames: Long,
        val playHeadFrames  : Long,
        val recording       : Boolean,
        val playing         : Boolean,
        val timecode        : String,
    ) : EngineEvent()

    data class DiskStatus(
        val targets          : List<DiskTargetInfo>,
        val remainingSecs    : Long,
    ) : EngineEvent()

    data class DiskTargetInfo(
        val path        : String,
        val freeBytes   : Long,
        val writeRateBps: Long,
        val ok          : Boolean,
    )

    data class TargetCommit(val commits: List<Entry>) : EngineEvent() {
        data class Entry(
            val folderId : Int,
            val targetIdx: Int,
            val frames   : Long,
            val bytes    : Long,
        )
    }

    data class EngineError(val code: Int, val severity: String, val message: String) : EngineEvent()
    data class Log(val level: String, val message: String) : EngineEvent()
    data class Pong(val seq: Int) : EngineEvent()
    data class Unknown(val msgType: Int, val json: String) : EngineEvent()
}

/* -------------------------------------------------------------------------
 * Parser — converts raw (msgType, json) into typed EngineEvent
 * ---------------------------------------------------------------------- */

object EngineEventParser {
    private val json = Json { ignoreUnknownKeys = true }

    fun parse(msgType: Int, payload: String): EngineEvent = try {
        val obj = if (payload.isBlank()) JsonObject(emptyMap())
                  else json.parseToJsonElement(payload).jsonObject

        when (msgType) {
            MsgType.EVT_READY -> EngineEvent.Ready(
                engineVersion      = obj["engine_version"]?.jsonPrimitive?.content ?: "",
                maxChannels        = obj["max_channels"]?.jsonPrimitive?.int ?: 128,
                sharedMemMeters    = obj["shared_mem_meters"]?.jsonPrimitive?.content ?: "",
                sharedMemWaveforms = obj["shared_mem_waveforms"]?.jsonPrimitive?.content ?: "",
                sampleRate         = obj["sample_rate"]?.jsonPrimitive?.int ?: 48000,
                devices            = obj["devices"]?.jsonArray?.mapNotNull { d ->
                    val o = d.jsonObject
                    EngineEvent.DeviceInfo(
                        name      = o["name"]?.jsonPrimitive?.content ?: return@mapNotNull null,
                        isDefault = o["is_default"]?.jsonPrimitive?.boolean ?: false,
                    )
                } ?: emptyList(),
            )

            MsgType.EVT_STATE_CHANGE -> EngineEvent.StateChange(
                prevState = obj["prev_state"]?.jsonPrimitive?.content ?: "",
                newState  = obj["new_state"]?.jsonPrimitive?.content ?: "",
            )

            MsgType.EVT_TRANSPORT -> EngineEvent.Transport(
                recordHeadFrames = obj["record_head_frames"]?.jsonPrimitive?.long ?: 0L,
                playHeadFrames   = obj["play_head_frames"]?.jsonPrimitive?.long ?: 0L,
                recording        = obj["recording"]?.jsonPrimitive?.boolean ?: false,
                playing          = obj["playing"]?.jsonPrimitive?.boolean ?: false,
                timecode         = obj["timecode"]?.jsonPrimitive?.content ?: "00:00:00:00",
            )

            MsgType.EVT_DISK_STATUS -> EngineEvent.DiskStatus(
                targets = obj["targets"]?.jsonArray?.map { t ->
                    val o = t.jsonObject
                    EngineEvent.DiskTargetInfo(
                        path         = o["path"]?.jsonPrimitive?.content ?: "",
                        freeBytes    = o["free_bytes"]?.jsonPrimitive?.long ?: 0L,
                        writeRateBps = o["write_rate_bps"]?.jsonPrimitive?.long ?: 0L,
                        ok           = o["ok"]?.jsonPrimitive?.boolean ?: false,
                    )
                } ?: emptyList(),
                remainingSecs = obj["estimated_remaining_seconds"]?.jsonPrimitive?.long ?: 0L,
            )

            MsgType.EVT_TARGET_COMMIT -> EngineEvent.TargetCommit(
                commits = obj["commits"]?.jsonArray?.map { c ->
                    val o = c.jsonObject
                    EngineEvent.TargetCommit.Entry(
                        folderId  = o["folder_id"]?.jsonPrimitive?.int ?: 0,
                        targetIdx = o["target_idx"]?.jsonPrimitive?.int ?: 0,
                        frames    = o["frames"]?.jsonPrimitive?.long ?: 0L,
                        bytes     = o["bytes"]?.jsonPrimitive?.long ?: 0L,
                    )
                } ?: emptyList()
            )

            MsgType.EVT_ERROR -> EngineEvent.EngineError(
                code     = obj["code"]?.jsonPrimitive?.int ?: 0,
                severity = obj["severity"]?.jsonPrimitive?.content ?: "",
                message  = obj["message"]?.jsonPrimitive?.content ?: "",
            )

            MsgType.EVT_LOG -> EngineEvent.Log(
                level   = obj["level"]?.jsonPrimitive?.content ?: "info",
                message = obj["message"]?.jsonPrimitive?.content ?: payload,
            )

            MsgType.EVT_PONG -> EngineEvent.Pong(seq = 0)

            else -> EngineEvent.Unknown(msgType, payload)
        }
    } catch (e: Exception) {
        EngineEvent.Unknown(msgType, payload)
    }
}
