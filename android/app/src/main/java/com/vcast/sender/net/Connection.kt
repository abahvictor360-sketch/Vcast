package com.vcast.sender.net

import com.vcast.sender.protocol.Protocol
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * TCP stream to one Vcast receiver: handshake, a single writer thread fed by a bounded
 * [SendQueue], PINGs while idle, BYE on clean shutdown and disconnect detection.
 *
 * Plain JVM code (no Android APIs) so it can be exercised by unit tests and against
 * tools/mock_receiver.py.
 */
class Connection(
    val host: String,
    val port: Int,
    private val listener: Listener,
    private val queue: SendQueue = SendQueue(),
) {
    interface Listener {
        /** A video frame was dropped; the encoder should produce a sync frame soon. */
        fun onSyncFrameNeeded()

        /** Called once when the connection ends. [error] is null for a local, clean close. */
        fun onClosed(error: String?)

        /** Called about once per second from the writer thread. */
        fun onStats(stats: Stats) {}
    }

    data class Stats(
        val bitsPerSecond: Long,
        val totalBytes: Long,
        val droppedVideoFrames: Long,
        val droppedAudioFrames: Long,
        val queuedBytes: Long,
    )

    private val socket = Socket()
    private lateinit var out: OutputStream
    private val closed = AtomicBoolean(false)
    private val closing = AtomicBoolean(false)
    private var writer: Thread? = null
    private var reader: Thread? = null

    @Volatile private var writeStartedNs = 0L
    @Volatile var receiverName: String = ""
        private set

    /**
     * Connects, performs the handshake and queues HELLO. Blocking; call it off the main
     * thread. Throws [IOException] with a user-readable message on failure.
     */
    fun connect(helloPayload: ByteArray, timeoutMs: Int = 5000) {
        try {
            socket.tcpNoDelay = true
            // A small kernel send buffer keeps latency low and lets SendQueue see congestion.
            socket.sendBufferSize = 256 * 1024
            socket.connect(InetSocketAddress(host, port), timeoutMs)
            socket.soTimeout = timeoutMs
            out = socket.getOutputStream()
            out.write(Protocol.HANDSHAKE)
            out.flush()
            val line = Protocol.readLine(socket.getInputStream())
            when (val r = Protocol.parseHandshakeLine(line)) {
                is Protocol.HandshakeResult.Ok -> receiverName = r.receiverName
                is Protocol.HandshakeResult.Err -> throw IOException("Receiver refused: ${r.reason}")
            }
            socket.soTimeout = 0
        } catch (e: IOException) {
            closeSocketQuietly()
            throw e
        }
        queue.offer(Protocol.TYPE_HELLO, Protocol.encodeFrame(Protocol.TYPE_HELLO, helloPayload, nowUs()))
        writer = Thread(::writeLoop, "vcast-writer").apply { start() }
        reader = Thread(::readLoop, "vcast-reader").apply {
            isDaemon = true
            start()
        }
    }

    val isOpen: Boolean get() = !closed.get() && !closing.get()

    /**
     * Queues a complete frame (header + payload, see [Protocol.newFrame]). Never blocks on
     * the network. Returns false if the frame was dropped.
     */
    fun send(type: Int, frame: ByteArray): Boolean {
        checkStuckWrite()
        return when (queue.offer(type, frame)) {
            SendQueue.Result.QUEUED -> true
            SendQueue.Result.DROPPED_NEED_SYNC -> {
                listener.onSyncFrameNeeded()
                false
            }
            else -> false
        }
    }

    /** Sends BYE (dropping anything still queued) and closes the connection. */
    fun close() {
        if (!closing.compareAndSet(false, true)) return
        queue.clear()
        queue.offerFinal(Protocol.newFrame(Protocol.TYPE_BYE, 0, nowUs()))
        val w = writer
        if (w != null && w !== Thread.currentThread()) {
            try {
                w.join(1500)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        finish(null)
    }

    private fun writeLoop() {
        var lastWriteNs = System.nanoTime()
        var statBytes = 0L
        var totalBytes = 0L
        var statStartNs = System.nanoTime()
        try {
            while (true) {
                var frame = queue.take(250)
                if (frame == null) {
                    if (queue.isClosed) break
                    // PING when nothing was sent for a second (protocol: at least every 2 s).
                    if (System.nanoTime() - lastWriteNs < 1_000_000_000L) {
                        frame = null
                    } else {
                        frame = Protocol.newFrame(Protocol.TYPE_PING, 0, nowUs())
                    }
                }
                if (frame != null) {
                    writeStartedNs = System.nanoTime()
                    out.write(frame)
                    writeStartedNs = 0
                    lastWriteNs = System.nanoTime()
                    statBytes += frame.size
                    totalBytes += frame.size
                }
                val now = System.nanoTime()
                if (now - statStartNs >= 1_000_000_000L) {
                    val bps = statBytes * 8 * 1_000_000_000L / (now - statStartNs)
                    listener.onStats(
                        Stats(bps, totalBytes, queue.droppedVideoFrames, queue.droppedAudioFrames, queue.bytes),
                    )
                    statBytes = 0
                    statStartNs = now
                }
            }
            out.flush()
            finish(null)
        } catch (e: IOException) {
            finish(if (closing.get()) null else "Connection lost (${e.message ?: e.javaClass.simpleName})")
        }
    }

    private fun readLoop() {
        // The receiver sends nothing after the handshake; reading detects when it hangs up.
        val buf = ByteArray(256)
        try {
            val input = socket.getInputStream()
            while (input.read(buf) >= 0) { /* ignore */ }
            finish(if (closing.get()) null else "Receiver closed the connection")
        } catch (e: IOException) {
            finish(if (closing.get()) null else "Connection lost (${e.message ?: e.javaClass.simpleName})")
        }
    }

    /** If one write has been blocked for longer than the receiver's own timeout, give up. */
    private fun checkStuckWrite() {
        val started = writeStartedNs
        if (started != 0L && System.nanoTime() - started > 10_000_000_000L) {
            finish("Network stalled")
        }
    }

    private fun finish(error: String?) {
        if (!closed.compareAndSet(false, true)) return
        queue.close()
        closeSocketQuietly()
        listener.onClosed(error)
    }

    private fun closeSocketQuietly() {
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }

    companion object {
        /** Monotonic clock in microseconds; the same clock MediaCodec surface timestamps use. */
        fun nowUs(): Long = System.nanoTime() / 1000
    }
}
