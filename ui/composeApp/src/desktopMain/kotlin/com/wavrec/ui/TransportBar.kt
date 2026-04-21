package com.wavrec.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.*
import com.wavrec.model.EngineConnectionState
import com.wavrec.model.EngineState

private val RECORD_RED  = Color(0xFFE53935)
private val PLAY_GREEN  = Color(0xFF43A047)
private val STOP_GRAY   = Color(0xFF9E9E9E)
private val TC_TEXT     = Color(0xFF00FF88)

@Composable
fun TransportBar(
    state   : EngineState,
    onRecord: () -> Unit,
    onPlay  : () -> Unit,
    onStop  : () -> Unit,
    modifier: Modifier = Modifier,
) {
    val transport  = state.transport
    val connected  = state.connection == EngineConnectionState.CONNECTED
    /* Record is enabled whenever at least one track is armed OR has a pending
     * pre-arm.  During recording, pressing it punches: finalises the current
     * take, advances the take number, and rolls to a new file set at the
     * next wall-second.  We intentionally allow the click while recording
     * so the user can trigger a punch. */
    val anyArmed   = state.tracks.any { it.armed || it.preArmed == true }

    Surface(
        modifier  = modifier.fillMaxWidth().height(56.dp),
        color     = Color(0xFF1A1A1A),
        tonalElevation = 4.dp,
    ) {
        Row(
            modifier            = Modifier.fillMaxSize().padding(horizontal = 16.dp),
            verticalAlignment   = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            /* --- Timecode display --- */
            Text(
                text       = transport.timecode,
                color      = if (transport.recording) TC_TEXT else TC_TEXT.copy(alpha = 0.5f),
                fontSize   = 22.sp,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                modifier   = Modifier.width(160.dp),
            )

            Spacer(Modifier.width(16.dp))

            /* --- Transport buttons --- */
            IconButton(
                onClick  = onRecord,
                enabled  = connected && anyArmed,
            ) {
                Icon(Icons.Default.FiberManualRecord, "Record",
                     /* Solid red while actively recording; dim when idle-armed. */
                     tint = if (transport.recording) RECORD_RED else RECORD_RED.copy(alpha = 0.5f),
                     modifier = Modifier.size(32.dp))
            }

            IconButton(
                onClick = onPlay,
                enabled = connected && !transport.playing,
            ) {
                Icon(Icons.Default.PlayArrow, "Play",
                     tint = if (transport.playing) PLAY_GREEN else PLAY_GREEN.copy(alpha = 0.5f),
                     modifier = Modifier.size(32.dp))
            }

            IconButton(
                onClick = onStop,
                enabled = connected && (transport.recording || transport.playing),
            ) {
                Icon(Icons.Default.Stop, "Stop",
                     tint = STOP_GRAY, modifier = Modifier.size(32.dp))
            }

            Spacer(Modifier.weight(1f))

            /* --- Engine state chip --- */
            val stateColor = when (state.engineState) {
                "RECORDING", "RECORD_PLAY" -> RECORD_RED
                "PLAYING"                  -> PLAY_GREEN
                "IDLE", "ARMED"            -> Color(0xFF888888)
                else                       -> Color(0xFF555555)
            }
            Surface(
                color = stateColor.copy(alpha = 0.15f),
                shape = MaterialTheme.shapes.small,
            ) {
                Text(
                    text = state.engineState,
                    color = stateColor,
                    fontSize = 11.sp,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }

            /* --- Connection dot --- */
            val dotColor = when (state.connection) {
                EngineConnectionState.CONNECTED    -> Color(0xFF00C853)
                EngineConnectionState.CONNECTING   -> Color(0xFFFFAB00)
                EngineConnectionState.DISCONNECTED -> Color(0xFF666666)
            }
            Surface(
                shape = MaterialTheme.shapes.small,
                color = dotColor,
                modifier = Modifier.size(8.dp),
            ) {}
        }
    }
}
