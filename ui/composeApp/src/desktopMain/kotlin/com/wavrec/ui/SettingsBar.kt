package com.wavrec.ui

import androidx.compose.animation.*
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.*
import com.wavrec.model.AudioDevice
import com.wavrec.model.EngineState

private val SAMPLE_RATES  = listOf(44100, 48000, 88200, 96000, 176400, 192000)
private val SAMPLE_FORMATS = listOf(
    "pcm16"   to "16-bit",
    "pcm24"   to "24-bit",
    "pcm32"   to "32-bit",
    "float32" to "Float",
)

/* Production recording rates — drop-frame is a delivery format, not a
 * recording format, so only non-drop rates are offered. */
private val TC_RATES = listOf("23.976", "24", "25", "29.97", "30")

private fun formatSampleRate(sr: Int): String = when {
    sr % 1000 == 0 -> "${sr / 1000}k"
    else           -> "%.1fk".format(sr / 1000.0)
}

@Composable
fun SettingsBar(
    state                : EngineState,
    onSelectDevice       : (String) -> Unit,
    onSelectSampleRate   : (Int) -> Unit,
    onSelectSampleFormat : (String) -> Unit,
    onSelectTimecodeRate : (String, Boolean) -> Unit,
    onAddTrack           : () -> Unit,
    modifier             : Modifier = Modifier,
) {
    Surface(
        color     = Color(0xFF161616),
        modifier  = modifier.fillMaxWidth(),
        tonalElevation = 2.dp,
    ) {
        Row(
            modifier            = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment   = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            DeviceDropdown(
                label    = "ASIO",
                selected = state.selectedDevice,
                devices  = state.devices,
                onSelect = onSelectDevice,
                modifier = Modifier.weight(2f),
            )

            StringDropdown(
                label    = "SR",
                selected = formatSampleRate(state.sampleRate),
                options  = SAMPLE_RATES.map { formatSampleRate(it) to it.toString() },
                onSelect = { onSelectSampleRate(it.toInt()) },
                modifier = Modifier.weight(0.7f),
            )

            StringDropdown(
                label    = "Fmt",
                selected = SAMPLE_FORMATS.firstOrNull { it.first == state.sampleFormat }?.second ?: state.sampleFormat,
                options  = SAMPLE_FORMATS.map { it.second to it.first },
                onSelect = onSelectSampleFormat,
                modifier = Modifier.weight(0.7f),
            )

            StringDropdown(
                label    = "TC",
                selected = state.timecodeRate,
                options  = TC_RATES.map { it to it },
                onSelect = { onSelectTimecodeRate(it, false) },
                modifier = Modifier.weight(0.7f),
            )

            Spacer(Modifier.weight(0.3f))

            /* Add track button */
            FilledTonalButton(
                onClick = onAddTrack,
                colors  = ButtonDefaults.filledTonalButtonColors(
                    containerColor = Color(0xFF2A2A2A),
                    contentColor   = Color(0xFF00C853),
                ),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp),
            ) {
                Icon(Icons.Default.Add, contentDescription = "Add track",
                     modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(4.dp))
                Text("Add Track", fontSize = 11.sp)
            }
        }
    }
}

@Composable
private fun DeviceDropdown(
    label    : String,
    selected : String,
    devices  : List<AudioDevice>,
    onSelect : (String) -> Unit,
    modifier : Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }
    val displayName = selected.ifBlank { if (devices.isEmpty()) "No devices" else "Select…" }

    Box(modifier = modifier) {
        OutlinedButton(
            onClick  = { if (devices.isNotEmpty()) expanded = true },
            colors   = ButtonDefaults.outlinedButtonColors(contentColor = Color(0xFFAAAAAA)),
            border   = androidx.compose.foundation.BorderStroke(0.5.dp, Color(0xFF3A3A3A)),
            contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text     = "$label: $displayName",
                fontSize = 11.sp,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            Icon(Icons.Default.ArrowDropDown, null, modifier = Modifier.size(16.dp))
        }

        DropdownMenu(
            expanded         = expanded,
            onDismissRequest = { expanded = false },
        ) {
            devices.forEach { dev ->
                DropdownMenuItem(
                    text    = {
                        Text(
                            text = dev.name + if (dev.isDefault) " ★" else "",
                            fontSize = 12.sp,
                            color = if (dev.name == selected) Color(0xFF00C853)
                                    else Color(0xFFCCCCCC),
                        )
                    },
                    onClick = { onSelect(dev.name); expanded = false },
                )
            }
        }
    }
}

/* Generic string dropdown — options are (displayLabel, value) pairs.
 * `selected` is compared against displayLabel to highlight the active option. */
@Composable
private fun StringDropdown(
    label    : String,
    selected : String,
    options  : List<Pair<String, String>>,
    onSelect : (String) -> Unit,
    modifier : Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }

    Box(modifier = modifier) {
        OutlinedButton(
            onClick  = { expanded = true },
            colors   = ButtonDefaults.outlinedButtonColors(contentColor = Color(0xFFAAAAAA)),
            border   = androidx.compose.foundation.BorderStroke(0.5.dp, Color(0xFF3A3A3A)),
            contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text     = "$label: $selected",
                fontSize = 11.sp,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            Icon(Icons.Default.ArrowDropDown, null, modifier = Modifier.size(16.dp))
        }

        DropdownMenu(
            expanded         = expanded,
            onDismissRequest = { expanded = false },
        ) {
            options.forEach { (display, value) ->
                DropdownMenuItem(
                    text    = {
                        Text(
                            text     = display,
                            fontSize = 12.sp,
                            color    = if (display == selected) Color(0xFF00C853)
                                       else Color(0xFFCCCCCC),
                        )
                    },
                    onClick = { onSelect(value); expanded = false },
                )
            }
        }
    }
}
