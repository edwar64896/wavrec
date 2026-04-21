package com.wavrec.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import com.wavrec.engine.WaveformBuffers

private val WFM_COLOR = Color(0xFF00C853)
private val WFM_BG    = Color(0xFF0F0F0F)

/** Renders a min/max waveform strip for [trackId] by reading from [buffers].
 *  Recomposes when the track's version counter ticks.
 *  One pixel column per stored block (scrolls off the left as new blocks arrive). */
@Composable
fun WaveformStrip(
    buffers : WaveformBuffers,
    trackId : Int,
    modifier: Modifier = Modifier,
) {
    val version by buffers.version(trackId)

    /* Scratch arrays reused across recompositions to avoid GC churn.
     * Sized to a generous upper bound; actual column count is clamped below. */
    val minBuf = remember { FloatArray(buffers.capacity) }
    val maxBuf = remember { FloatArray(buffers.capacity) }

    Canvas(modifier = modifier.fillMaxSize()) {
        /* Reading `version` above ensures recomposition on new data. */
        @Suppress("UNUSED_VARIABLE") val v = version

        val w = size.width
        val h = size.height
        if (w <= 0f || h <= 0f) return@Canvas

        /* Fit as many columns as pixels; older data scrolls off the left. */
        val maxCols = w.toInt().coerceAtLeast(1)
        val n = buffers.snapshot(trackId, maxCols, minBuf, maxBuf)
        if (n == 0) return@Canvas

        val midY   = h * 0.5f
        val scaleY = h * 0.5f
        val startX = w - n.toFloat()  /* right-align so newest is on the right */

        /* Draw each column as a vertical line from min→max. */
        for (i in 0 until n) {
            val x = startX + i
            val y0 = midY - maxBuf[i] * scaleY
            val y1 = midY - minBuf[i] * scaleY
            drawLine(
                color = WFM_COLOR,
                start = Offset(x, y0),
                end   = Offset(x, y1),
                strokeWidth = 1f,
                cap   = StrokeCap.Butt,
            )
        }
    }
}
