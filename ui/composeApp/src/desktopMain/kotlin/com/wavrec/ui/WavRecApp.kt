package com.wavrec.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.*
import com.wavrec.model.AppViewModel

private val AppBackground = Color(0xFF141414)

@Composable
fun WavRecApp(vm: AppViewModel) {
    val state    by vm.state.collectAsState()
    var showSettings by remember { mutableStateOf(true) }
    var editTargetsForFolder: Int? by remember { mutableStateOf(null) }

    MaterialTheme(
        colorScheme = darkColorScheme(
            background   = AppBackground,
            surface      = Color(0xFF1C1C1C),
            primary      = Color(0xFF00C853),
            onBackground = Color(0xFFEEEEEE),
            onSurface    = Color(0xFFCCCCCC),
        )
    ) {
        Surface(modifier = Modifier.fillMaxSize(), color = AppBackground) {
            Column(modifier = Modifier.fillMaxSize()) {

                /* Transport bar + settings toggle — fixed 56dp row */
                Row(
                    modifier          = Modifier.fillMaxWidth().height(56.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TransportBar(
                        state    = state,
                        onRecord = vm::record,
                        onPlay   = vm::play,
                        onStop   = vm::stop,
                        modifier = Modifier.weight(1f).fillMaxHeight(),
                    )
                    Surface(
                        color    = Color(0xFF1A1A1A),
                        modifier = Modifier.fillMaxHeight().width(48.dp),
                        onClick  = { showSettings = !showSettings },
                    ) {
                        Box(contentAlignment = Alignment.Center, modifier = Modifier.fillMaxSize()) {
                            Icon(
                                Icons.Default.Settings,
                                "Settings",
                                tint = if (showSettings) Color(0xFF00C853) else Color(0xFF555555),
                            )
                        }
                    }
                }

                /* Collapsible settings bar */
                if (showSettings) {
                    SettingsBar(
                        state                = state,
                        onSelectDevice       = vm::setDevice,
                        onSelectSampleRate   = vm::setSampleRate,
                        onSelectSampleFormat = vm::setSampleFormat,
                        onSelectTimecodeRate = vm::setTimecodeRate,
                        onSelectPreRoll      = vm::setPreRoll,
                        onSelectScene        = vm::setScene,
                        onSelectTake         = vm::setTake,
                        onAddFolder          = { vm.addFolder() },
                    )
                    HorizontalDivider(color = Color(0xFF2A2A2A))
                }

                /* Bulk actions */
                TrackHeader(
                    state          = state,
                    onArmAll       = vm::armAllTracks,
                    onMonitorAll   = vm::monitorAllTracks,
                    onClearAlerts  = vm::clearAllAlerts,
                )
                HorizontalDivider(color = Color(0xFF2A2A2A))

                /* Track list, grouped by folder */
                TrackList(
                    tracks           = state.tracks,
                    folders          = state.folders,
                    diskStatus       = state.diskTargets,
                    targetCommits    = state.targetCommits,
                    waveforms        = vm.waveforms,
                    onArm            = vm::armTrack,
                    onMonitor        = vm::setMonitor,
                    onLabelChange    = vm::setTrackLabel,
                    onInputChange    = vm::setTrackInput,
                    onRemove         = vm::removeTrack,
                    onMoveTrack      = vm::moveTrackToFolder,
                    onReorderTrack   = vm::reorderTrackInFolder,
                    onFolderArm      = vm::armFolder,
                    onFolderMon      = vm::monitorFolder,
                    onFolderCollapse = vm::setFolderCollapsed,
                    onFolderRename   = vm::renameFolder,
                    onFolderAddTrack = { id -> vm.addTrack(folderId = id) },
                    onFolderEditTargets = { editTargetsForFolder = it },
                    onFolderRemove   = vm::removeFolder,
                    modifier         = Modifier.weight(1f),
                )

                /* Status bar */
                StatusBar(state = state, onResetClips = vm::resetClips)
            }

            /* Folder targets editor — shown when user clicks a folder's target label */
            editTargetsForFolder?.let { fid ->
                val folder = state.folders.firstOrNull { it.id == fid }
                if (folder != null) {
                    FolderTargetsDialog(
                        folder       = folder,
                        diskStatus   = state.diskTargets,
                        onSetTargets = { vm.setFolderTargets(fid, it) },
                        onDismiss    = { editTargetsForFolder = null },
                    )
                } else {
                    editTargetsForFolder = null
                }
            }
        }
    }
}

@Composable
private fun StatusBar(
    state        : com.wavrec.model.EngineState,
    onResetClips : () -> Unit,
) {
    Surface(
        color     = Color(0xFF121212),
        tonalElevation = 2.dp,
        modifier  = Modifier.fillMaxWidth().height(26.dp),
    ) {
        Row(
            modifier            = Modifier.fillMaxSize().padding(horizontal = 12.dp),
            verticalAlignment   = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text(
                "${state.tracks.size} tracks",
                color = Color(0xFF555555), fontSize = 10.sp,
            )

            state.diskTargets.forEach { t ->
                val gb = t.freeBytes / 1_073_741_824.0
                Text("%.1f GB free".format(gb),
                     color = if (t.ok) Color(0xFF666666) else Color(0xFFE53935),
                     fontSize = 10.sp)
            }
            if (state.remainingSecs > 0)
                Text("~${state.remainingSecs / 60}min",
                     color = Color(0xFF555555), fontSize = 10.sp)

            Spacer(Modifier.weight(1f))

            state.lastError?.let { Text(it, color = Color(0xFFE53935), fontSize = 9.sp, maxLines = 1) }

            Text("${state.sampleRate / 1000}kHz",
                 color = Color(0xFF444444), fontSize = 10.sp)
        }
    }
}
