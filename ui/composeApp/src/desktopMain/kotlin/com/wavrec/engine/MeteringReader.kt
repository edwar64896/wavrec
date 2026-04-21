package com.wavrec.engine

import com.sun.jna.Native
import com.sun.jna.Pointer
import com.sun.jna.platform.win32.Kernel32
import com.sun.jna.platform.win32.WinNT
import com.wavrec.model.ChannelMeter

/* -------------------------------------------------------------------------
 * MeteringReader — reads from the WavRecMeterRegion shared memory.
 *
 * Layout (matches ipc_protocol.h WavRecMeterRegion):
 *
 *   Header (16 bytes):
 *     [0..3]  uint32  magic
 *     [4]     uint8   version
 *     [5]     uint8   write_index  (0 or 1 — atomic, acquire-load)
 *     [6..7]  uint16  n_channels
 *     [8..15] padding
 *
 *   WavRecMeterChannel × 128  (slot 0, starting at offset 16)
 *   WavRecMeterChannel × 128  (slot 1, starting at offset 16 + 128×16)
 *
 *   WavRecMeterChannel (16 bytes):
 *     [0..3]  float   true_peak
 *     [4..7]  float   rms
 *     [8..11] float   peak_hold
 *     [12]    uint8   clip
 *     [13]    uint8   active
 *     [14..15] padding
 * ---------------------------------------------------------------------- */

class MeteringReader {

    private var handle : WinNT.HANDLE? = null
    private var pointer: Pointer?      = null
    private var isOpen  = false

    companion object {
        const val MAX_CHANNELS       = 128
        private const val HEADER_SIZE          = 16
        private const val CHANNEL_SIZE         = 16
        private const val CHANNEL_ARRAY_SIZE   = MAX_CHANNELS * CHANNEL_SIZE  // 2048
        const val REGION_SIZE        = HEADER_SIZE + CHANNEL_ARRAY_SIZE * 2  // 4112

        private const val OFF_WRITE_INDEX      = 5L
        private const val OFF_CHANNELS_SLOT0   = HEADER_SIZE.toLong()
        private const val OFF_CHANNELS_SLOT1   = (HEADER_SIZE + CHANNEL_ARRAY_SIZE).toLong()
    }

    fun open(shmName: String): Boolean {
        if (isOpen) return true
        return if (System.getProperty("os.name").contains("Windows", ignoreCase = true))
            openWindows(shmName)
        else
            openPosix(shmName)
    }

    private fun openWindows(name: String): Boolean {
        val k32 = Kernel32.INSTANCE
        /* Try Global\ namespace first (cross-session), fall back to local */
        val h = k32.OpenFileMapping(WinNT.FILE_MAP_READ, false, "Global\\$name")
             ?: k32.OpenFileMapping(WinNT.FILE_MAP_READ, false, name)
             ?: return false

        val ptr = k32.MapViewOfFile(h, WinNT.FILE_MAP_READ, 0, 0, REGION_SIZE)
            ?: run { k32.CloseHandle(h); return false }

        handle  = h
        pointer = ptr
        isOpen  = true
        return true
    }

    private fun openPosix(name: String): Boolean {
        /* TODO: implement via CLibrary.shm_open + mmap for macOS/Linux */
        return false
    }

    /** Read all channel meters from the current (writer-completed) slot. */
    fun read(n: Int = MAX_CHANNELS): Array<ChannelMeter> {
        val ptr = pointer ?: return Array(n) { ChannelMeter() }

        /* Acquire-load the write_index byte */
        val writeIndex = ptr.getByte(OFF_WRITE_INDEX).toInt() and 0x01
        val base = if (writeIndex == 0) OFF_CHANNELS_SLOT0 else OFF_CHANNELS_SLOT1

        return Array(n.coerceAtMost(MAX_CHANNELS)) { i ->
            val off = base + i * CHANNEL_SIZE
            ChannelMeter(
                truePeak  = ptr.getFloat(off),
                rms       = ptr.getFloat(off + 4),
                peakHold  = ptr.getFloat(off + 8),
                clip      = ptr.getByte(off + 12) != 0.toByte(),
                active    = ptr.getByte(off + 13) != 0.toByte(),
            )
        }
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
