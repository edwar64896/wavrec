package com.wavrec.ui

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.*
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.key.*
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import com.wavrec.engine.WaveformBuffers
import com.wavrec.model.TrackAlert
import com.wavrec.model.TrackState

/* Row height kept compact so ~48 rows fit in a 1080p window at 100% scale
 * and users can reasonably expand to 64+ tracks via scrolling. */
private val ROW_HEIGHT = 22.dp

private val BG_EVEN      = Color(0xFF1C1C1C)
private val BG_ODD       = Color(0xFF181818)
private val BG_ARMED     = Color(0x20E53935)
private val ARM_ON       = Color(0xFFE53935)
private val ARM_OFF      = Color(0xFF3A3A3A)
private val ARM_PRE_ON   = Color(0xFFFF9500)
private val ARM_PRE_OFF  = Color(0xFF664400)
private val CH_TEXT      = Color(0xFF888888)
private val DEL_COLOR    = Color(0xFF444444)
private val MONITOR_ON   = Color(0xFF007AFF)
private val MONITOR_OFF  = Color(0xFF2A2A2A)
private val WFM_BG       = Color(0xFF0F0F0F)

@Composable
fun TrackList(
    tracks        : List<TrackState>,
    waveforms     : WaveformBuffers,
    onArm         : (Int, Boolean) -> Unit,
    onMonitor     : (Int, Boolean) -> Unit,
    onLabelChange : (Int, String) -> Unit,
    onInputChange : (Int, Int) -> Unit,
    onRemove      : (Int) -> Unit,
    modifier      : Modifier = Modifier,
) {
    val listState = rememberLazyListState()
    Box(modifier.fillMaxSize()) {
        LazyColumn(state = listState, modifier = Modifier.fillMaxSize()) {
            itemsIndexed(tracks, key = { _, t -> t.id }) { index, track ->
                TrackRow(
                    track         = track,
                    even          = index % 2 == 0,
                    waveforms     = waveforms,
                    onArm         = { onArm(track.id, !track.armed) },
                    onMonitor     = { onMonitor(track.id, !track.monitor) },
                    onLabelChange = { onLabelChange(track.id, it) },
                    onInputChange = { onInputChange(track.id, it) },
                    onRemove      = { onRemove(track.id) },
                )
            }
            item { Spacer(Modifier.height(8.dp)) }

            if (tracks.isEmpty()) {
                item {
                    Box(Modifier.fillMaxWidth().height(80.dp),
                        contentAlignment = Alignment.Center) {
                        Text("No tracks — click Add Track to begin",
                             color = Color(0xFF555555), fontSize = 13.sp)
                    }
                }
            }
        }
    }
}

@Composable
private fun TrackRow(
    track         : TrackState,
    even          : Boolean,
    waveforms     : WaveformBuffers,
    onArm         : () -> Unit,
    onMonitor     : () -> Unit,
    onLabelChange : (String) -> Unit,
    onInputChange : (Int) -> Unit,
    onRemove      : () -> Unit,
) {
    val bg = if (track.armed) BG_ARMED else if (even) BG_EVEN else BG_ODD

    var editingLabel by remember(track.id) { mutableStateOf(false) }
    var labelDraft   by remember(track.id) { mutableStateOf(track.label) }

    Row(
        modifier          = Modifier
            .fillMaxWidth()
            .height(ROW_HEIGHT)
            .background(bg)
            .padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text(
            text     = "%03d".format(track.id + 1),
            color    = Color(0xFF444444),
            fontSize = 9.sp,
            modifier = Modifier.width(22.dp),
        )

        /* Arm */
        val armColor = when (track.preArmed) {
            true  -> ARM_PRE_ON
            false -> ARM_PRE_OFF
            null  -> if (track.armed) ARM_ON else ARM_OFF
        }
        IndicatorButton("R", armColor, onArm)

        /* Monitor */
        IndicatorButton(
            label = "M",
            color = if (track.monitor) MONITOR_ON else MONITOR_OFF,
            onClick = onMonitor,
        )

        /* Label — click to edit */
        if (editingLabel) {
            fun commit() {
                editingLabel = false
                if (labelDraft.isNotBlank() && labelDraft != track.label)
                    onLabelChange(labelDraft)
                else labelDraft = track.label
            }
            fun cancel() { editingLabel = false; labelDraft = track.label }

            /* Track "was focused at least once" so we only commit on focus
             * LOSS, not on the initial unfocused render that happens before
             * focusRequester.requestFocus() has a chance to run. */
            val focusRequester = remember(track.id) { FocusRequester() }
            var hadFocus by remember(track.id) { mutableStateOf(false) }
            LaunchedEffect(Unit) { focusRequester.requestFocus() }

            BasicTextField(
                value         = labelDraft,
                onValueChange = { labelDraft = it },
                singleLine    = true,
                textStyle     = TextStyle(color = Color(0xFFEEEEEE), fontSize = 11.sp),
                cursorBrush   = SolidColor(Color(0xFF00C853)),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { commit() }),
                modifier      = Modifier
                    .width(100.dp)
                    .focusRequester(focusRequester)
                    .onFocusChanged {
                        if (it.isFocused) hadFocus = true
                        else if (hadFocus && editingLabel) commit()
                    }
                    .onPreviewKeyEvent { e ->
                        if (e.type == KeyEventType.KeyDown) when (e.key) {
                            Key.Enter, Key.NumPadEnter -> { commit(); true }
                            Key.Escape                 -> { cancel(); true }
                            else                       -> false
                        } else false
                    },
            )
        } else {
            Text(
                text     = track.label,
                color    = if (track.armed) Color(0xFFEEEEEE) else Color(0xFFBBBBBB),
                fontSize = 11.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .width(100.dp)
                    .clickable { editingLabel = true; labelDraft = track.label },
            )
        }

        /* Input channel spinner — ◀ N ▶ */
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(1.dp),
        ) {
            SmallArrow(Icons.Default.ChevronLeft) {
                onInputChange((track.hwInput - 1).coerceAtLeast(0))
            }
            Text(
                text     = "IN%02d".format(track.hwInput + 1),
                color    = CH_TEXT,
                fontSize = 10.sp,
                modifier = Modifier.widthIn(min = 36.dp),
            )
            SmallArrow(Icons.Default.ChevronRight) {
                onInputChange((track.hwInput + 1).coerceAtMost(127))
            }
        }

        /* Waveform strip — flexible width, dominates the row. */
        Surface(
            color    = WFM_BG,
            shape    = MaterialTheme.shapes.extraSmall,
            modifier = Modifier
                .weight(1f)
                .fillMaxHeight()
                .padding(vertical = 2.dp),
        ) {
            WaveformStrip(buffers = waveforms, trackId = track.id)
        }

        /* Alert icon — shown to the left of the VU meter when alert != NONE. */
        AlertIcon(track.alert)

        /* VU meter — thin vertical bar. */
        VuMeter(
            truePeak = track.meter.truePeak,
            rms      = track.meter.rms,
            peakHold = track.meter.peakHold,
            clip     = track.meter.clip,
            active   = track.meter.active,
            modifier = Modifier.width(8.dp).fillMaxHeight().padding(vertical = 2.dp),
        )

        /* Delete */
        Box(
            modifier = Modifier.size(14.dp).clickable(onClick = onRemove),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Default.Close, "Remove track",
                 tint = DEL_COLOR, modifier = Modifier.size(11.dp))
        }
    }
    HorizontalDivider(color = Color(0xFF242424), thickness = 0.5.dp)
}

/** Centred 18×14 R/M-style indicator.  `Box` fills the Surface so the
 * content alignment actually applies. */
@Composable
private fun IndicatorButton(
    label   : String,
    color   : Color,
    onClick : () -> Unit,
) {
    Surface(
        onClick  = onClick,
        color    = color,
        shape    = MaterialTheme.shapes.extraSmall,
        modifier = Modifier.size(width = 18.dp, height = 14.dp),
    ) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(
                text       = label,
                color      = Color.White,
                fontSize   = 8.sp,
                textAlign  = androidx.compose.ui.text.style.TextAlign.Center,
            )
        }
    }
}

private val ALERT_CLIP      = Color(0xFFFF1744)
private val ALERT_NEAR_CLIP = Color(0xFFFFAB00)
private val ALERT_LOW       = Color(0xFF9E9E9E)

@Composable
private fun AlertIcon(alert: TrackAlert) {
    /* Reserve a fixed-width slot even when NONE so the layout doesn't jitter. */
    Box(Modifier.size(width = 14.dp, height = 14.dp),
        contentAlignment = Alignment.Center) {
        if (alert == TrackAlert.NONE) return@Box
        val (icon, tint) = when (alert) {
            TrackAlert.CLIPPING   -> Icons.Default.Error       to ALERT_CLIP
            TrackAlert.NEAR_CLIP  -> Icons.Default.Warning     to ALERT_NEAR_CLIP
            TrackAlert.LOW_SIGNAL -> Icons.Default.VolumeOff   to ALERT_LOW
            TrackAlert.NONE       -> return@Box
        }
        Icon(icon, contentDescription = alert.name,
             tint = tint, modifier = Modifier.size(12.dp))
    }
}

@Composable
private fun SmallArrow(
    icon    : androidx.compose.ui.graphics.vector.ImageVector,
    onClick : () -> Unit,
) {
    Box(
        modifier = Modifier.size(14.dp).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, tint = CH_TEXT, modifier = Modifier.size(12.dp))
    }
}
