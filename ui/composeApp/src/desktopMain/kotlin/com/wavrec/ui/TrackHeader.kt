package com.wavrec.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.*
import com.wavrec.model.EngineState

/** Bulk-action strip above the track list.  Collapses the per-track ARM/MON
 *  clicks into single "arm all" / "mon all" toggles and exposes a
 *  Clear Alerts button that resets latched clip, near-clip and low-signal
 *  state across every track. */
@Composable
fun TrackHeader(
    state            : EngineState,
    onArmAll         : (Boolean) -> Unit,
    onMonitorAll     : (Boolean) -> Unit,
    onClearAlerts    : () -> Unit,
    modifier         : Modifier = Modifier,
) {
    val tracks      = state.tracks
    val nTracks     = tracks.size
    val nArmed      = tracks.count { it.armed }
    val nMonitored  = tracks.count { it.monitor }
    val hasAlert    = tracks.any { it.alert != com.wavrec.model.TrackAlert.NONE }

    /* Tri-state: none → click = all on; all → click = all off;
     * mixed → click = all on (bring everybody in line). */
    val allArmed      = nTracks > 0 && nArmed     == nTracks
    val allMonitored  = nTracks > 0 && nMonitored == nTracks

    Surface(
        color          = Color(0xFF141414),
        tonalElevation = 1.dp,
        modifier       = modifier.fillMaxWidth().height(22.dp),
    ) {
        Row(
            modifier              = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
            verticalAlignment     = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            /* Match the track-number column on the left */
            Spacer(Modifier.width(22.dp))

            /* Arm all */
            HeaderChip(
                label     = "R",
                enabledBg = Color(0xFFE53935),
                mixed     = nArmed in 1 until nTracks,
                on        = allArmed,
                onClick   = { onArmAll(!allArmed) },
            )

            /* Monitor all */
            HeaderChip(
                label     = "M",
                enabledBg = Color(0xFF007AFF),
                mixed     = nMonitored in 1 until nTracks,
                on        = allMonitored,
                onClick   = { onMonitorAll(!allMonitored) },
            )

            Spacer(Modifier.weight(1f))

            /* Clear alerts */
            val clearTint = if (hasAlert) Color(0xFFFFAB00) else Color(0xFF444444)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .height(16.dp)
                    .clickable(onClick = onClearAlerts)
                    .padding(horizontal = 4.dp),
            ) {
                Icon(
                    Icons.Default.NotificationsOff,
                    contentDescription = "Clear alerts",
                    tint     = clearTint,
                    modifier = Modifier.size(12.dp),
                )
                Spacer(Modifier.width(3.dp))
                Text("Clear Alerts", color = clearTint, fontSize = 10.sp)
            }
        }
    }
}

/** Tri-state chip matching the per-track R/M buttons' geometry. */
@Composable
private fun HeaderChip(
    label     : String,
    enabledBg : Color,
    mixed     : Boolean,
    on        : Boolean,
    onClick   : () -> Unit,
) {
    val bg = when {
        on    -> enabledBg
        mixed -> enabledBg.copy(alpha = 0.35f)
        else  -> Color(0xFF2A2A2A)
    }
    Surface(
        onClick  = onClick,
        color    = bg,
        shape    = MaterialTheme.shapes.extraSmall,
        modifier = Modifier.size(width = 18.dp, height = 14.dp),
    ) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(label, color = Color.White, fontSize = 8.sp)
        }
    }
}
