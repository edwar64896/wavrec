package com.wavrec.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.key.*
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import com.wavrec.model.DiskTarget
import com.wavrec.model.Folder
import com.wavrec.model.TrackAlert
import com.wavrec.model.TrackState

/** Folder summary/header row.  Acts as:
 *  - Collapse toggle for the folder's tracks.
 *  - Bulk R / M controls (tri-state vs. per-track armed/monitor state).
 *  - Aggregate status display: track count, worst alert, combined VU.
 *  - Edit handle for folder name.
 *  - Target badges (read-only in this pass; editing comes later). */
@Composable
fun FolderHeader(
    folder        : Folder,
    tracks        : List<TrackState>,
    diskStatus    : List<DiskTarget>,
    targetCommits : Map<Pair<Int, Int>, Long>,
    onArm         : (Boolean) -> Unit,
    onMon         : (Boolean) -> Unit,
    onCollapse    : () -> Unit,
    onRename      : (String) -> Unit,
    onAddTrack    : () -> Unit,
    onEditTargets : () -> Unit,
    onRemove      : () -> Unit,
) {
    val total     = tracks.size
    val armed     = tracks.count { it.armed }
    val mon       = tracks.count { it.monitor }
    val allArmed  = total > 0 && armed == total
    val allMon    = total > 0 && mon   == total

    /* Worst-severity alert across the folder. */
    val worstAlert = when {
        tracks.any { it.alert == TrackAlert.CLIPPING   } -> TrackAlert.CLIPPING
        tracks.any { it.alert == TrackAlert.NEAR_CLIP  } -> TrackAlert.NEAR_CLIP
        tracks.any { it.alert == TrackAlert.LOW_SIGNAL } -> TrackAlert.LOW_SIGNAL
        else                                             -> TrackAlert.NONE
    }
    val alertCount = tracks.count { it.alert != TrackAlert.NONE }

    /* Aggregate VU — max across all tracks' metering. */
    val aggTruePeak = tracks.maxOfOrNull { it.meter.truePeak }    ?: 0f
    val aggRms      = tracks.maxOfOrNull { it.meter.rms }         ?: 0f
    val aggHold     = tracks.maxOfOrNull { it.meter.peakHold }    ?: 0f
    val aggActive   = tracks.any { it.meter.active }
    val aggClip     = tracks.any { it.meter.clip }

    var editingName by remember(folder.id) { mutableStateOf(false) }
    var nameDraft   by remember(folder.id) { mutableStateOf(folder.name) }

    Surface(
        color          = Color(0xFF0F0F0F),
        tonalElevation = 1.dp,
        modifier       = Modifier.fillMaxWidth().height(26.dp),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xFF161616))
                .padding(horizontal = 4.dp),
            verticalAlignment     = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            /* Collapse chevron */
            Box(
                modifier = Modifier.size(22.dp).clickable(onClick = onCollapse),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    if (folder.collapsed) Icons.Default.ChevronRight
                    else                  Icons.Default.ExpandMore,
                    contentDescription = "Collapse",
                    tint = Color(0xFFAAAAAA),
                    modifier = Modifier.size(16.dp),
                )
            }

            /* Bulk R / M tri-state buttons */
            FolderChip(
                label     = "R",
                enabledBg = Color(0xFFE53935),
                mixed     = armed in 1 until total,
                on        = allArmed,
                onClick   = { onArm(!allArmed) },
            )
            FolderChip(
                label     = "M",
                enabledBg = Color(0xFF007AFF),
                mixed     = mon in 1 until total,
                on        = allMon,
                onClick   = { onMon(!allMon) },
            )

            /* Folder name — click to edit */
            if (editingName) {
                val focusRequester = remember(folder.id) { FocusRequester() }
                var hadFocus by remember(folder.id) { mutableStateOf(false) }
                LaunchedEffect(Unit) { focusRequester.requestFocus() }
                fun commit() {
                    editingName = false
                    if (nameDraft.isNotBlank() && nameDraft != folder.name)
                        onRename(nameDraft)
                    else nameDraft = folder.name
                }
                fun cancel() { editingName = false; nameDraft = folder.name }
                BasicTextField(
                    value         = nameDraft,
                    onValueChange = { nameDraft = it },
                    singleLine    = true,
                    textStyle     = TextStyle(color = Color(0xFFFFFFFF), fontSize = 12.sp),
                    cursorBrush   = SolidColor(Color(0xFF00C853)),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { commit() }),
                    modifier      = Modifier
                        .width(140.dp)
                        .focusRequester(focusRequester)
                        .onFocusChanged {
                            if (it.isFocused) hadFocus = true
                            else if (hadFocus && editingName) commit()
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
                    text = folder.name,
                    color = Color(0xFFFFFFFF),
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .width(140.dp)
                        .clickable { editingName = true; nameDraft = folder.name },
                )
            }

            /* Track count */
            Text(
                text = "$armed/$total",
                color = Color(0xFF888888),
                fontSize = 10.sp,
                modifier = Modifier.widthIn(min = 34.dp),
            )

            /* Alert summary: worst-severity icon + count */
            if (alertCount > 0) {
                val (icon, tint) = when (worstAlert) {
                    TrackAlert.CLIPPING   -> Icons.Default.Error     to Color(0xFFFF1744)
                    TrackAlert.NEAR_CLIP  -> Icons.Default.Warning   to Color(0xFFFFAB00)
                    TrackAlert.LOW_SIGNAL -> Icons.Default.VolumeOff to Color(0xFF9E9E9E)
                    TrackAlert.NONE       -> Icons.Default.Error     to Color(0xFF444444)
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(icon, null, tint = tint, modifier = Modifier.size(12.dp))
                    Spacer(Modifier.width(2.dp))
                    Text(alertCount.toString(), color = tint, fontSize = 10.sp)
                }
            } else {
                Spacer(Modifier.width(20.dp))
            }

            /* Aggregate waveform space (intentionally empty on collapsed view —
             * keeps horizontal alignment with track-row waveform columns). */
            Spacer(Modifier.weight(1f))

            /* Aggregate VU — shows the loudest channel in this folder. */
            VuMeter(
                truePeak = aggTruePeak,
                rms      = aggRms,
                peakHold = aggHold,
                clip     = aggClip,
                active   = aggActive,
                modifier = Modifier.width(8.dp).fillMaxHeight().padding(vertical = 4.dp),
            )

            /* Per-target disk chips — each flashes bright green on EVT_TARGET_COMMIT.
             * Click any chip to open the targets editor. */
            FolderTargetBar(
                folder        = folder,
                diskStatus    = diskStatus,
                targetCommits = targetCommits,
                onEditTargets = onEditTargets,
            )

            /* Add track to this folder */
            Box(
                modifier = Modifier.size(16.dp).clickable(onClick = onAddTrack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Default.Add, "Add track to folder",
                     tint = Color(0xFF00C853), modifier = Modifier.size(13.dp))
            }

            /* Delete folder */
            Box(
                modifier = Modifier.size(16.dp).clickable(onClick = onRemove),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Default.Close, "Remove folder",
                     tint = Color(0xFF444444), modifier = Modifier.size(12.dp))
            }
        }
    }
}

/** Tri-state bulk chip matching per-track R/M geometry. */
@Composable
private fun FolderChip(
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
        modifier = Modifier.size(width = 20.dp, height = 16.dp),
    ) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(label, color = Color.White, fontSize = 9.sp)
        }
    }
}
