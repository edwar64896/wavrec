package com.wavrec.engine

import com.sun.jna.Pointer
import com.sun.jna.platform.win32.Kernel32
import com.sun.jna.platform.win32.WinNT

/* -------------------------------------------------------------------------
 * WfmReader — maps the WavRecWfmRegion shared memory and drains waveform
 * blocks as the engine produces them.
 *
 *   Header (64 bytes):
 *     [0..3]   magic
 *     [4]      version
 *     [5..7]   padding
 *     [8..11]  decimation (samples per block)
 *     [12..15] sample_rate
 *     [16..19] write_pos (producer advances)
 *     [20..23] read_pos  (this reader advances)
 *     [24..63] padding
 *
 *   Block (24 bytes, natural alignment — see WavRecWfmBlock in ipc_protocol.h):
 *     [0]      channel_id
 *     [1..3]   padding
 *     [4..7]   n_samples
 *     [8..15]  timeline_frame (u64)
 *     [16..19] min
 *     [20..23] max
 * ---------------------------------------------------------------------- */

class WfmReader {

    private var handle : WinNT.HANDLE? = null
    private var pointer: Pointer?      = null
    private var isOpen  = false

    private var lastSeenWrite = 0L

    companion object {
        const val RING_SLOTS  = 4096
        const val BLOCK_SIZE  = 24
        const val HEADER_SIZE = 64L
        const val REGION_SIZE = HEADER_SIZE.toInt() + RING_SLOTS * BLOCK_SIZE

        private const val OFF_DECIMATION  = 8L
        private const val OFF_SAMPLE_RATE = 12L
        private const val OFF_WRITE_POS   = 16L
        private const val OFF_READ_POS    = 20L

        private const val BLK_N_SAMPLES     = 4L
        private const val BLK_TIMELINE_FRAME= 8L
        private const val BLK_MIN           = 16L
        private const val BLK_MAX           = 20L
    }

    data class Block(
        val channelId    : Int,
        val timelineFrame: Long,
        val nSamples     : Int,
        val min          : Float,
        val max          : Float,
    )

    var decimation : Int = 512 ; private set
    var sampleRate : Int = 48000; private set

    fun open(shmName: String): Boolean {
        if (isOpen) return true
        if (!System.getProperty("os.name").contains("Windows", ignoreCase = true))
            return false
        val k32 = Kernel32.INSTANCE
        val access = WinNT.FILE_MAP_READ or WinNT.FILE_MAP_WRITE
        val h = k32.OpenFileMapping(access, false, "Global\\$shmName")
             ?: k32.OpenFileMapping(access, false, shmName)
             ?: return false
        val ptr = k32.MapViewOfFile(h, access, 0, 0, REGION_SIZE)
            ?: run { k32.CloseHandle(h); return false }
        handle  = h
        pointer = ptr
        decimation = ptr.getInt(OFF_DECIMATION).takeIf { it > 0 } ?: 512
        sampleRate = ptr.getInt(OFF_SAMPLE_RATE).takeIf { it > 0 } ?: 48000
        /* Reset our read cursor to whatever the engine has already written
         * so we don't accidentally process pre-existing stale blocks. */
        lastSeenWrite = ptr.getInt(OFF_WRITE_POS).toLong() and 0xFFFFFFFFL
        ptr.setInt(OFF_READ_POS, lastSeenWrite.toInt())
        isOpen  = true
        return true
    }

    /** Drain all blocks between our last-seen write_pos and the current one.
     *  After draining, advance read_pos so the engine sees back-pressure. */
    fun drain(onBlock: (Block) -> Unit): Int {
        val ptr = pointer ?: return 0
        val writePos = ptr.getInt(OFF_WRITE_POS).toLong() and 0xFFFFFFFFL
        if (writePos == lastSeenWrite) return 0

        var pos = lastSeenWrite
        var drained = 0
        while (pos < writePos) {
            val slot = (pos and (RING_SLOTS - 1).toLong()).toInt()
            val off  = HEADER_SIZE + slot * BLOCK_SIZE
            val channelId     = ptr.getByte(off).toInt() and 0xFF
            val nSamples      = ptr.getInt(off + BLK_N_SAMPLES)
            val timelineFrame = ptr.getLong(off + BLK_TIMELINE_FRAME)
            val min           = ptr.getFloat(off + BLK_MIN)
            val max           = ptr.getFloat(off + BLK_MAX)
            onBlock(Block(channelId, timelineFrame, nSamples, min, max))
            pos++
            drained++
        }
        lastSeenWrite = writePos
        ptr.setInt(OFF_READ_POS, writePos.toInt())
        return drained
    }

    fun close() {
        if (!isOpen) return
        pointer?.let { Kernel32.INSTANCE.UnmapViewOfFile(it) }
        handle?.let  { Kernel32.INSTANCE.CloseHandle(it) }
        pointer = null
        handle  = null
        isOpen  = false
    }
}
