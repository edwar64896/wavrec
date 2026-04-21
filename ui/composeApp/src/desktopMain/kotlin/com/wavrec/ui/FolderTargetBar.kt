package com.wavrec.ui

import androidx.compose.animation.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import com.wavrec.model.DiskTarget
import com.wavrec.model.Folder

/* Palette */
private val DOT_OK        = Color(0xFF00C853)   /* idle: ok */
private val DOT_FLASH     = Color(0xFFB9F6CA)   /* bright pulse on commit */
private val DOT_BAD       = Color(0xFFE53935)
private val DOT_INACTIVE  = Color(0xFF555555)
private val CHIP_BG       = Color(0xFF222222)
private val CHIP_TEXT     = Color(0xFFBBBBBB)
private val EMPTY_TXT     = Color(0xFFE53935)

/** Compact row of per-target chips for a folder.
 *  Each chip shows:
 *   - status dot (green OK / red fail / grey inactive) that pulses bright
 *     green on every EVT_TARGET_COMMIT for that (folder, target) pair
 *   - short free-space label ("2.1T", "500G", "999M")
 *  Clicking any chip opens the target editor for this folder. */
@Composable
fun FolderTargetBar(
    folder        : Folder,
    diskStatus    : List<DiskTarget>,
    targetCommits : Map<Pair<Int, Int>, Long>,
    onEditTargets : () -> Unit,
    modifier      : Modifier = Modifier,
) {
    if (folder.targets.isEmpty()) {
        Text(
            text     = "→ set target…",
            color    = EMPTY_TXT,
            fontSize = 10.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = modifier
                .widthIn(max = 140.dp)
                .clickable(onClick = onEditTargets)
                .padding(horizontal = 4.dp),
        )
        return
    }

    /* Build a path→status lookup once per recomposition. */
    val statusByPath = remember(diskStatus) {
        diskStatus.associateBy { it.path.replace('\\', '/').trimEnd('/').lowercase() }
    }

    Row(
        modifier              = modifier,
        horizontalArrangement = Arrangement.spacedBy(3.dp),
        verticalAlignment     = Alignment.CenterVertically,
    ) {
        folder.targets.forEachIndexed { idx, path ->
            val key    = path.replace('\\', '/').trimEnd('/').lowercase()
            val status = statusByPath[key]
            val commit = targetCommits[folder.id to idx] ?: 0L
            TargetChip(
                status       = status,
                lastCommitMs = commit,
                onClick      = onEditTargets,
            )
        }
    }
}

@Composable
private fun TargetChip(
    status       : DiskTarget?,
    lastCommitMs : Long,
    onClick      : () -> Unit,
) {
    val baseline = when {
        status == null -> DOT_INACTIVE
        status.ok      -> DOT_OK
        else           -> DOT_BAD
    }

    /* Pulse animation: snap bright on commit, fade back to baseline over 400ms.
     * Two effects: one that re-snaps baseline whenever it changes (status flip),
     * another that runs the pulse whenever lastCommitMs changes. */
    val animColor = remember { Animatable(baseline) }
    LaunchedEffect(baseline) {
        if (animColor.value != baseline && !animColor.isRunning)
            animColor.snapTo(baseline)
    }
    LaunchedEffect(lastCommitMs) {
        if (lastCommitMs > 0) {
            animColor.snapTo(DOT_FLASH)
            animColor.animateTo(baseline,
                animationSpec = tween(durationMillis = 400, easing = LinearEasing))
        }
    }

    val freeLabel = status?.let { formatFreeBytes(it.freeBytes) } ?: "—"

    Row(
        modifier = Modifier
            .height(18.dp)
            .background(CHIP_BG, shape = RoundedCornerShape(3.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(7.dp)
                .background(animColor.value, shape = CircleShape),
        )
        Spacer(Modifier.width(3.dp))
        Text(
            text     = freeLabel,
            color    = if (status?.ok == false) DOT_BAD else CHIP_TEXT,
            fontSize = 9.sp,
            maxLines = 1,
        )
    }
}

/** "2.1T", "512G", "998M", "—" */
private fun formatFreeBytes(bytes: Long): String = when {
    bytes <= 0L                -> "—"
    bytes >= 1_000_000_000_000 -> "%.1fT".format(bytes / 1_099_511_627_776.0)
    bytes >= 1_000_000_000     -> "%dG".format(bytes / 1_073_741_824L)
    bytes >= 1_000_000         -> "%dM".format(bytes / 1_048_576L)
    else                       -> "%dK".format(bytes / 1024L)
}
