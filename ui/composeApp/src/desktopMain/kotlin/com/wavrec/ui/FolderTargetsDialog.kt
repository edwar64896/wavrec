package com.wavrec.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.wavrec.model.DiskTarget
import com.wavrec.model.Folder
import java.io.File
import javax.swing.JFileChooser

/** Compact popup for editing a folder's recording targets (redundant paths).
 *  - Shows each path with its live free-space / OK status (from EVT_DISK_STATUS).
 *  - "+ Add…" opens a Swing folder picker.  Cross-platform (JVM).
 *  - Paths persisted via vm.setFolderTargets. */
@Composable
fun FolderTargetsDialog(
    folder      : Folder,
    diskStatus  : List<DiskTarget>,
    onSetTargets: (List<String>) -> Unit,
    onDismiss   : () -> Unit,
) {
    /* Local working copy so the user can add/remove without each change
     * round-tripping through pushSessionInit.  Commit on Done. */
    var paths by remember(folder.id) { mutableStateOf(folder.targets) }

    /* Map path → live status, case-insensitive on Windows via normalized key. */
    val statusByPath = remember(diskStatus) {
        diskStatus.associateBy { it.path.replace('\\', '/').trimEnd('/').lowercase() }
    }
    fun statusFor(p: String): DiskTarget? =
        statusByPath[p.replace('\\', '/').trimEnd('/').lowercase()]

    Dialog(
        onDismissRequest = onDismiss,
        properties       = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(
            color    = Color(0xFF1A1A1A),
            shape    = MaterialTheme.shapes.small,
            modifier = Modifier.width(560.dp).wrapContentHeight(),
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(16.dp)) {
                /* Title */
                Text(
                    text     = "Recording Targets — ${folder.name}",
                    color    = Color(0xFFEEEEEE),
                    fontSize = 14.sp,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text     = "Each target writes a full redundant copy of this folder's tracks.",
                    color    = Color(0xFF888888),
                    fontSize = 10.sp,
                )
                Spacer(Modifier.height(12.dp))

                /* Paths list */
                if (paths.isEmpty()) {
                    Box(Modifier.fillMaxWidth().height(48.dp),
                        contentAlignment = Alignment.Center) {
                        Text("No targets — click Add to choose one.",
                             color = Color(0xFF888888), fontSize = 11.sp)
                    }
                } else {
                    LazyColumn(
                        modifier = Modifier.heightIn(max = 260.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        items(paths, key = { it }) { path ->
                            TargetRow(
                                path     = path,
                                status   = statusFor(path),
                                onRemove = { paths = paths - path },
                            )
                        }
                    }
                }

                Spacer(Modifier.height(12.dp))

                /* Footer: Add + Done */
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FilledTonalButton(
                        onClick = {
                            val picked = pickFolder(paths.firstOrNull())
                            if (picked != null && picked !in paths)
                                paths = paths + picked
                        },
                        colors  = ButtonDefaults.filledTonalButtonColors(
                            containerColor = Color(0xFF2A2A2A),
                            contentColor   = Color(0xFF00C853),
                        ),
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp),
                    ) {
                        Icon(Icons.Default.Add, null, modifier = Modifier.size(14.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Add…", fontSize = 11.sp)
                    }
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onDismiss) {
                        Text("Cancel", color = Color(0xFFAAAAAA), fontSize = 11.sp)
                    }
                    Spacer(Modifier.width(4.dp))
                    FilledTonalButton(
                        onClick = { onSetTargets(paths); onDismiss() },
                        colors  = ButtonDefaults.filledTonalButtonColors(
                            containerColor = Color(0xFF00C853),
                            contentColor   = Color.Black,
                        ),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp),
                    ) {
                        Text("Done", fontSize = 11.sp)
                    }
                }
            }
        }
    }
}

@Composable
private fun TargetRow(
    path     : String,
    status   : DiskTarget?,
    onRemove : () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFF242424), MaterialTheme.shapes.extraSmall)
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        /* Status dot: green OK / red bad / grey unknown */
        val dotColor = when {
            status == null -> Color(0xFF555555)
            status.ok      -> Color(0xFF00C853)
            else           -> Color(0xFFE53935)
        }
        Box(
            modifier = Modifier.size(8.dp)
                .background(dotColor, shape = androidx.compose.foundation.shape.CircleShape)
        )
        Spacer(Modifier.width(8.dp))

        /* Path + free-space suffix */
        Text(
            text     = path,
            color    = Color(0xFFCCCCCC),
            fontSize = 11.sp,
            maxLines = 1,
            overflow = TextOverflow.MiddleEllipsis,
            modifier = Modifier.weight(1f),
        )
        if (status != null) {
            val gb = status.freeBytes / 1_073_741_824.0
            Text(
                text     = "%.1f GB".format(gb),
                color    = if (status.ok) Color(0xFF888888) else Color(0xFFE53935),
                fontSize = 10.sp,
            )
        }

        Spacer(Modifier.width(8.dp))
        Box(
            modifier = Modifier.size(18.dp).clickable(onClick = onRemove),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Default.Close, "Remove target",
                 tint = Color(0xFF888888), modifier = Modifier.size(14.dp))
        }
    }
}

/** Swing folder picker.  Blocks on the invoking thread until user confirms or
 *  cancels.  Runs on the UI event thread — fine for a modal dialog. */
private fun pickFolder(initialDir: String?): String? {
    val start = initialDir?.let { File(it).takeIf(File::isDirectory) }
                ?: File(System.getProperty("user.home"))
    val chooser = JFileChooser(start)
    chooser.fileSelectionMode = JFileChooser.DIRECTORIES_ONLY
    chooser.dialogTitle       = "Select recording target folder"
    return if (chooser.showOpenDialog(null) == JFileChooser.APPROVE_OPTION)
        chooser.selectedFile.absolutePath
    else null
}
