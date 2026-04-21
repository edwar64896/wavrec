package com.wavrec.engine

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.*
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.CRC32

/* -------------------------------------------------------------------------
 * WavRec IPC constants
 * ---------------------------------------------------------------------- */

private const val WAVREC_MSG_MAGIC = 0x57415652.toInt()  // "WAVR"
private const val HEADER_SIZE = 16
private const val IPC_HOST = "127.0.0.1"
private const val IPC_PORT = 27182

/* -------------------------------------------------------------------------
 * EngineRunner — manages the engine process and the IPC pipe
 *
 * Lifecycle:
 *   1. launch() spawns the engine executable
 *   2. Rx coroutine connects to the named pipe, reads framed messages,
 *      parses them and emits EngineEvent objects
 *   3. sendCommand() serialises and writes command frames
 *   4. shutdown() sends CMD_SHUTDOWN and waits for process exit
 * ---------------------------------------------------------------------- */

class EngineRunner(
    /** Absolute path to the wavrec_engine executable. */
    private val engineExePath: String,
    private val scope: CoroutineScope,
) {
    private var process: Process? = null

    private val _events = MutableSharedFlow<EngineEvent>(
        replay         = 0,
        extraBufferCapacity = 512,
        onBufferOverflow = kotlinx.coroutines.channels.BufferOverflow.DROP_OLDEST,
    )
    val events: SharedFlow<EngineEvent> = _events.asSharedFlow()

    private var output: DataOutputStream? = null
    private val seq = AtomicInteger(0)
    /* Serialises writes to the output stream — multiple rapid sendCommand
     * calls must not interleave their (header + payload) byte streams. */
    private val sendMutex = Mutex()

    /* ------------------------------------------------------------------ */

    fun launch() {
        scope.launch(Dispatchers.IO) {
            runCatching {
                val logFile = java.io.File(
                    System.getProperty("user.home"), "WavRec/engine.log"
                ).also { it.parentFile.mkdirs() }
                process = ProcessBuilder(engineExePath)
                    .redirectError(logFile)
                    .redirectOutput(logFile)
                    .start()
            }.onFailure { e ->
                _events.emit(EngineEvent.EngineError(
                    code = 9001, severity = "fatal",
                    message = "Failed to launch engine: ${e.message}"
                ))
                return@launch
            }

            /* Reconnect loop — if the engine resets the socket we just
             * reconnect.  The engine's IPC rx thread accepts a fresh client
             * after any disconnect, so the next attempt succeeds. */
            val logFile = java.io.File(System.getProperty("user.home"), "WavRec/ui.log")
            while (isActive) {
                connectAndRead()
                logFile.appendText("[${System.currentTimeMillis()}] connectAndRead returned — reconnecting in 500ms\n")
                delay(500)
            }
        }
    }

    private suspend fun connectAndRead() = withContext(Dispatchers.IO) {
        val logFile = java.io.File(System.getProperty("user.home"), "WavRec/ui.log")
        fun log(msg: String) = logFile.appendText("[${System.currentTimeMillis()}] $msg\n")

        log("Connecting to $IPC_HOST:$IPC_PORT")
        var socket: Socket? = null
        var lastError = ""
        repeat(50) { attempt ->
            if (socket != null) return@repeat
            runCatching {
                socket = Socket(IPC_HOST, IPC_PORT).also {
                    it.tcpNoDelay   = true
                    it.soTimeout    = 0   /* blocking reads */
                }
                log("TCP connected on attempt $attempt")
            }.onFailure { e ->
                lastError = e.message ?: e.javaClass.simpleName
                if (attempt % 5 == 0) log("Attempt $attempt failed: $lastError")
                delay(200)
            }
        }

        val sock = socket ?: run {
            log("FATAL: Could not connect after 50 attempts. Last error: $lastError")
            _events.emit(EngineEvent.EngineError(
                code = 9002, severity = "fatal",
                message = "Could not connect to engine: $lastError"
            ))
            return@withContext
        }

        val input  = DataInputStream(BufferedInputStream(sock.getInputStream(), 65536))
        output     = DataOutputStream(sock.getOutputStream())

        log("Streams ready, entering read loop")
        val headerBuf = ByteArray(HEADER_SIZE)
        while (isActive) {
            try {
                input.readFully(headerBuf)
                log("Header rx: type=0x${(headerBuf[4].toInt() and 0xFF).toString(16)}")
            } catch (e: EOFException) { log("EOF"); break }
            catch (e: IOException)    { log("IOException: ${e.message}"); break }

            val bb = ByteBuffer.wrap(headerBuf).order(ByteOrder.LITTLE_ENDIAN)
            val magic      = bb.getInt(0)
            val msgType    = headerBuf[4].toInt() and 0xFF
            val payloadLen = bb.getShort(6).toInt() and 0xFFFF
            val crc32Wire  = bb.getInt(12)

            if (magic != WAVREC_MSG_MAGIC) continue

            val payloadBytes = if (payloadLen > 0) {
                ByteArray(payloadLen).also {
                    try { input.readFully(it) }
                    catch (e: IOException) { break }
                }
            } else ByteArray(0)

            /* CRC validation */
            if (payloadLen > 0) {
                val crc = CRC32().apply { update(payloadBytes) }.value.toInt()
                if (crc != crc32Wire) continue
            }

            val json  = if (payloadLen > 0) String(payloadBytes, Charsets.UTF_8) else ""
            if (msgType == MsgType.EVT_READY) {
                log("EVT_READY: payloadLen=$payloadLen json_prefix=${json.take(120)}")
                val parsed = EngineEventParser.parse(msgType, json)
                log("EVT_READY: parsed devices=${if (parsed is EngineEvent.Ready) parsed.devices.size else "NOT_READY"}")
                _events.emit(parsed)
            } else {
                val event = EngineEventParser.parse(msgType, json)
                _events.emit(event)
            }
        }

        output = null
        runCatching { sock.close() }
    }

    /* ------------------------------------------------------------------ */

    fun sendCommand(msgType: Int, jsonPayload: String = "{}") {
        val payload = jsonPayload.toByteArray(Charsets.UTF_8)
        val crc     = if (payload.isEmpty()) 0
                      else CRC32().apply { update(payload) }.value.toInt()

        val header = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(WAVREC_MSG_MAGIC)
            put(msgType.toByte())
            put(0)                           /* flags */
            putShort(payload.size.toShort())
            putInt(seq.getAndIncrement())
            putInt(crc)
        }.array()

        val logFile = java.io.File(System.getProperty("user.home"), "WavRec/ui.log")
        scope.launch(Dispatchers.IO) {
            sendMutex.withLock {
                runCatching {
                    val out = output ?: run {
                        logFile.appendText("[${System.currentTimeMillis()}] sendCommand type=0x${msgType.toString(16)} SKIPPED (output=null)\n")
                        return@withLock
                    }
                    out.write(header)
                    if (payload.isNotEmpty()) out.write(payload)
                    out.flush()
                    logFile.appendText("[${System.currentTimeMillis()}] sendCommand type=0x${msgType.toString(16)} sent ${payload.size}B\n")
                }.onFailure { e ->
                    logFile.appendText("[${System.currentTimeMillis()}] sendCommand type=0x${msgType.toString(16)} FAILED: ${e.message}\n")
                }
            }
        }
    }

    fun shutdown() {
        sendCommand(MsgType.CMD_SHUTDOWN)
        scope.launch(Dispatchers.IO) {
            delay(1000)
            process?.destroyForcibly()
        }
    }
}
