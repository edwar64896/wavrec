package com.wavrec.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.*
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlin.math.log10

private val METER_BG      = Color(0xFF111111)
private val METER_GREEN   = Color(0xFF00C853)
private val METER_YELLOW  = Color(0xFFFFAB00)
private val METER_RED     = Color(0xFFDD2222)
private val METER_HOLD    = Color(0xFFFFFFFF)
private val METER_CLIP    = Color(0xFFFF0000)
private val METER_DARK_GR = Color(0xFF003300)

private fun linearToDb(v: Float): Float =
    if (v <= 1e-9f) -90f else 20f * log10(v)

private fun dbToFraction(db: Float): Float =
    ((db.coerceIn(-60f, 0f) + 60f) / 60f)

@Composable
fun VuMeter(
    truePeak : Float,
    rms      : Float,
    peakHold : Float,
    clip     : Boolean,
    active   : Boolean,
    modifier : Modifier = Modifier.width(14.dp).fillMaxHeight(),
) {
    val peakDb = linearToDb(truePeak)
    val rmsDb  = linearToDb(rms)
    val holdDb = linearToDb(peakHold)

    Canvas(modifier = modifier) {
        val w = size.width
        val h = size.height

        /* Background */
        drawRect(METER_BG, size = size)

        if (!active) return@Canvas

        fun fracToY(frac: Float) = h - frac * h

        /* Tick marks at -6, -12, -18, -24, -48 dBFS */
        listOf(-6f, -12f, -18f, -24f, -48f).forEach { db ->
            val y = fracToY(dbToFraction(db))
            drawLine(Color(0xFF333333), Offset(0f, y), Offset(w, y), strokeWidth = 0.5f)
        }

        /* RMS fill bar — coloured by level */
        val rmsF   = dbToFraction(rmsDb)
        val rmsY   = fracToY(rmsF)
        val fillColor = when {
            rmsDb >= -6f  -> METER_RED
            rmsDb >= -18f -> METER_YELLOW
            else          -> METER_GREEN
        }
        drawRect(
            color    = fillColor,
            topLeft  = Offset(1f, rmsY),
            size     = Size(w - 2f, h - rmsY)
        )

        /* Peak hold line */
        val holdY = fracToY(dbToFraction(holdDb))
        drawLine(METER_HOLD, Offset(0f, holdY), Offset(w, holdY), strokeWidth = 1.5f)

        /* True-peak tick (single line, slightly brighter) */
        val peakY = fracToY(dbToFraction(peakDb))
        drawLine(Color(0xAAFFFFFF), Offset(0f, peakY), Offset(w, peakY), strokeWidth = 1f)

        /* Clip indicator — 3px red bar at top */
        if (clip) drawRect(METER_CLIP, topLeft = Offset(0f, 0f), size = Size(w, 3f))
    }
}
