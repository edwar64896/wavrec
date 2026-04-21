package com.wavrec.engine

import androidx.compose.runtime.*

/* -------------------------------------------------------------------------
 * WaveformBuffers — per-track circular min/max column history, kept outside
 * the main StateFlow to avoid copying lists on every append.  Composables
 * observe `version` to redraw when new data arrives.
 *
 * Each column = one engine-emitted block = `decimation` input samples.
 * ---------------------------------------------------------------------- */

class WaveformBuffers(
    val nTracks : Int = 128,
    val capacity: Int = 4096,
) {
    /** Min samples per track (interleaved column storage). */
    private val mins = Array(nTracks) { FloatArray(capacity) }
    private val maxs = Array(nTracks) { FloatArray(capacity) }

    /** Write position (monotonically increasing — modulo capacity for index). */
    private val writePos = IntArray(nTracks)

    /** Version counter forces Canvas recomposition when new blocks arrive.
     *  One counter per track so a row only repaints when its own data changes. */
    private val _versions = Array(nTracks) { mutableStateOf(0) }
    fun version(track: Int): State<Int> = _versions[track]

    fun append(track: Int, min: Float, max: Float) {
        if (track < 0 || track >= nTracks) return
        val slot = writePos[track] and (capacity - 1)
        mins[track][slot] = min
        maxs[track][slot] = max
        writePos[track]++
        _versions[track].value = writePos[track]
    }

    /** Clear all history — called on record start so we don't show stale data. */
    fun reset() {
        for (i in 0 until nTracks) {
            writePos[i] = 0
            mins[i].fill(0f)
            maxs[i].fill(0f)
            _versions[i].value = 0
        }
    }

    /** Total columns ever written for [track] (useful for absolute timing). */
    fun count(track: Int): Int = writePos[track]

    /** Copy the most recent [maxColumns] columns into [outMin]/[outMax].
     *  Returns the number of columns actually copied (≤ maxColumns).  The
     *  oldest column copied is at index 0.  If fewer than [maxColumns] have
     *  been written, the remaining output slots are left untouched. */
    fun snapshot(track: Int,
                 maxColumns: Int,
                 outMin: FloatArray,
                 outMax: FloatArray): Int {
        if (track < 0 || track >= nTracks) return 0
        val wp    = writePos[track]
        val count = if (wp < maxColumns) wp else maxColumns
        if (count == 0) return 0
        val start = wp - count
        val trackMin = mins[track]
        val trackMax = maxs[track]
        for (i in 0 until count) {
            val slot = (start + i) and (capacity - 1)
            outMin[i] = trackMin[slot]
            outMax[i] = trackMax[slot]
        }
        return count
    }
}
