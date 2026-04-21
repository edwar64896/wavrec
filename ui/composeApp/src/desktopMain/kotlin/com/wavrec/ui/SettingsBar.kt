package com.wavrec.ui

import androidx.compose.animation.*
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
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
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

private val PRE_ROLL_OPTIONS = listOf(
    "Off" to "0",
    "1s"  to "1",
    "2s"  to "2",
    "3s"  to "3",
    "5s"  to "5",
    "10s" to "10",
)

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
    onSelectPreRoll      : (Float) -> Unit,
    onSelectScene        : (Int) -> Unit,
    onSelectTake         : (Int) -> Unit,
    onAddFolder          : () -> Unit,
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

            StringDropdown(
                label    = "Pre",
                selected = if (state.preRollSeconds <= 0f) "Off"
                           else "%gs".format(state.preRollSeconds),
                options  = PRE_ROLL_OPTIONS,
                onSelect = { onSelectPreRoll(it.toFloat()) },
                modifier = Modifier.weight(0.7f),
            )

            /* Scene: ◀ NN ▶ — scene change resets take to 1 */
            NumericStepper(
                label    = "S",
                value    = state.sceneNum,
                min      = 1,
                max      = 99,
                digits   = 2,
                onChange = onSelectScene,
                modifier = Modifier.weight(0.65f),
            )

            /* Take: ◀ NNN ▶ — auto-advances on punch */
            NumericStepper(
                label    = "T",
                value    = state.takeNum,
                min      = 1,
                max      = 999,
                digits   = 3,
                onChange = onSelectTake,
                modifier = Modifier.weight(0.75f),
            )

            Spacer(Modifier.weight(0.3f))

            /* Add folder button */
            FilledTonalButton(
                onClick = onAddFolder,
                colors  = ButtonDefaults.filledTonalButtonColors(
                    containerColor = Color(0xFF2A2A2A),
                    contentColor   = Color(0xFFFFAB00),
                ),
                contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp),
            ) {
                Icon(Icons.Default.CreateNewFolder, contentDescription = "Add folder",
                     modifier = Modifier.size(14.dp))
                Spacer(Modifier.width(4.dp))
                Text("Folder", fontSize = 11.sp)
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

/* Compact numeric stepper: "S: ◀ 01 ▶" — click the number to type a new
 * value, arrows clamp at [min, max].  Used for Scene and Take. */
@Composable
private fun NumericStepper(
    label    : String,
    value    : Int,
    min      : Int,
    max      : Int,
    digits   : Int,
    onChange : (Int) -> Unit,
    modifier : Modifier = Modifier,
) {
    var editing by remember(value) { mutableStateOf(false) }
    var draft   by remember(value) { mutableStateOf("%0${digits}d".format(value)) }

    Row(
        modifier              = modifier,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment     = Alignment.CenterVertically,
    ) {
        Text(
            text     = "$label:",
            color    = Color(0xFFAAAAAA),
            fontSize = 11.sp,
        )

        /* Down arrow */
        Box(
            modifier = Modifier
                .size(18.dp)
                .clickable { onChange((value - 1).coerceAtLeast(min)) },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Default.ChevronLeft, null,
                 tint = Color(0xFFAAAAAA), modifier = Modifier.size(14.dp))
        }

        /* Display / inline editor */
        if (editing) {
            val focus = remember { FocusRequester() }
            var hadFocus by remember { mutableStateOf(false) }
            LaunchedEffect(Unit) { focus.requestFocus() }

            fun commit() {
                editing = false
                val n = draft.toIntOrNull()?.coerceIn(min, max)
                if (n != null && n != value) onChange(n)
                draft = "%0${digits}d".format(value)
            }
            fun cancel() {
                editing = false
                draft = "%0${digits}d".format(value)
            }

            BasicTextField(
                value         = draft,
                onValueChange = { s -> draft = s.filter { it.isDigit() }.take(digits) },
                singleLine    = true,
                textStyle     = TextStyle(
                    color     = Color(0xFFEEEEEE),
                    fontSize  = 12.sp,
                    textAlign = TextAlign.Center,
                ),
                cursorBrush   = SolidColor(Color(0xFF00C853)),
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Number,
                    imeAction    = ImeAction.Done,
                ),
                keyboardActions = KeyboardActions(onDone = { commit() }),
                modifier      = Modifier
                    .width(40.dp)
                    .background(Color(0xFF222222))
                    .focusRequester(focus)
                    .onFocusChanged {
                        if (it.isFocused) hadFocus = true
                        else if (hadFocus && editing) commit()
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
                text     = "%0${digits}d".format(value),
                color    = Color(0xFFEEEEEE),
                fontSize = 12.sp,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .width(40.dp)
                    .clickable { editing = true; draft = "%0${digits}d".format(value) }
                    .padding(vertical = 2.dp),
            )
        }

        /* Up arrow */
        Box(
            modifier = Modifier
                .size(18.dp)
                .clickable { onChange((value + 1).coerceAtMost(max)) },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Default.ChevronRight, null,
                 tint = Color(0xFFAAAAAA), modifier = Modifier.size(14.dp))
        }
    }
}
