package com.vcast.sender.protocol

import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.nio.charset.StandardCharsets

/**
 * Vcast sender protocol, version 1 (see docs/protocol.md in the repository).
 *
 * This file is plain Kotlin/JVM (no Android APIs) so it can be unit-tested on the JVM.
 * All integers are big-endian.
 */
object Protocol {
    const val DISCOVERY_PORT = 48689
    const val DEFAULT_STREAM_PORT = 48690
    const val VERSION = 1

    val DISCOVERY_QUERY: ByteArray = "VCAST?\n".toByteArray(StandardCharsets.US_ASCII)
    const val DISCOVERY_REPLY_PREFIX = "VCAST!\n"
    val HANDSHAKE: ByteArray = "VCAST/1\n".toByteArray(StandardCharsets.US_ASCII)

    const val HEADER_SIZE = 13
    const val MAX_PAYLOAD = 16 * 1024 * 1024

    const val TYPE_HELLO = 0x01
    const val TYPE_VIDEO_CONFIG = 0x10
    const val TYPE_VIDEO_FRAME = 0x11
    const val TYPE_VIDEO_KEYFRAME = 0x12
    const val TYPE_AUDIO_CONFIG = 0x20
    const val TYPE_AUDIO_FRAME = 0x21
    const val TYPE_PING = 0x30
    const val TYPE_BYE = 0x31

    const val VIDEO_CODEC_H264 = 1
    const val VIDEO_CODEC_H265 = 2
    const val AUDIO_CODEC_AAC = 1

    fun typeName(type: Int): String = when (type) {
        TYPE_HELLO -> "HELLO"
        TYPE_VIDEO_CONFIG -> "VIDEO_CONFIG"
        TYPE_VIDEO_FRAME -> "VIDEO_FRAME"
        TYPE_VIDEO_KEYFRAME -> "VIDEO_KEYFRAME"
        TYPE_AUDIO_CONFIG -> "AUDIO_CONFIG"
        TYPE_AUDIO_FRAME -> "AUDIO_FRAME"
        TYPE_PING -> "PING"
        TYPE_BYE -> "BYE"
        else -> "0x%02x".format(type)
    }

    /** Writes a 13-byte frame header into [dst] at [offset]. */
    fun writeHeader(dst: ByteArray, offset: Int, type: Int, length: Int, ptsUs: Long) {
        require(type in 0..0xff) { "bad frame type $type" }
        require(length in 0..MAX_PAYLOAD) { "bad payload length $length" }
        dst[offset] = type.toByte()
        putInt(dst, offset + 1, length)
        putLong(dst, offset + 5, ptsUs)
    }

    /**
     * Allocates a complete frame (header + room for [payloadLength] bytes) with the header
     * already filled in. The caller copies the payload to offset [HEADER_SIZE].
     */
    fun newFrame(type: Int, payloadLength: Int, ptsUs: Long): ByteArray {
        val frame = ByteArray(HEADER_SIZE + payloadLength)
        writeHeader(frame, 0, type, payloadLength, ptsUs)
        return frame
    }

    fun encodeFrame(type: Int, payload: ByteArray, ptsUs: Long): ByteArray {
        val frame = newFrame(type, payload.size, ptsUs)
        System.arraycopy(payload, 0, frame, HEADER_SIZE, payload.size)
        return frame
    }

    fun helloPayload(device: String, os: String, app: String): ByteArray =
        ("{\"device\":" + Json.quote(device) + ",\"os\":" + Json.quote(os) +
            ",\"app\":" + Json.quote(app) + "}").toByteArray(StandardCharsets.UTF_8)

    /** VIDEO_CONFIG payload: codec byte followed by the Annex-B parameter sets. */
    fun videoConfigPayload(codec: Int, parameterSets: ByteArray): ByteArray {
        val out = ByteArray(1 + parameterSets.size)
        out[0] = codec.toByte()
        System.arraycopy(parameterSets, 0, out, 1, parameterSets.size)
        return out
    }

    /** AUDIO_CONFIG payload: codec u8, sample rate u32, channels u8, AudioSpecificConfig. */
    fun audioConfigPayload(codec: Int, sampleRate: Int, channels: Int, asc: ByteArray): ByteArray {
        val out = ByteArray(6 + asc.size)
        out[0] = codec.toByte()
        putInt(out, 1, sampleRate)
        out[5] = channels.toByte()
        System.arraycopy(asc, 0, out, 6, asc.size)
        return out
    }

    fun putInt(dst: ByteArray, offset: Int, v: Int) {
        dst[offset] = (v ushr 24).toByte()
        dst[offset + 1] = (v ushr 16).toByte()
        dst[offset + 2] = (v ushr 8).toByte()
        dst[offset + 3] = v.toByte()
    }

    fun putLong(dst: ByteArray, offset: Int, v: Long) {
        for (i in 0 until 8) dst[offset + i] = (v ushr (56 - 8 * i)).toByte()
    }

    fun getInt(src: ByteArray, offset: Int): Int =
        ((src[offset].toInt() and 0xff) shl 24) or
            ((src[offset + 1].toInt() and 0xff) shl 16) or
            ((src[offset + 2].toInt() and 0xff) shl 8) or
            (src[offset + 3].toInt() and 0xff)

    fun getLong(src: ByteArray, offset: Int): Long {
        var v = 0L
        for (i in 0 until 8) v = (v shl 8) or (src[offset + i].toLong() and 0xff)
        return v
    }

    // ---------------------------------------------------------------- discovery

    data class DiscoveryReply(val name: String, val port: Int, val version: Int)

    /** Parses a `VCAST!\n{json}` datagram; returns null if it is not a valid reply. */
    fun parseDiscoveryReply(data: ByteArray, length: Int = data.size): DiscoveryReply? {
        val text = try {
            String(data, 0, length, StandardCharsets.UTF_8)
        } catch (e: Exception) {
            return null
        }
        if (!text.startsWith(DISCOVERY_REPLY_PREFIX)) return null
        val obj = Json.parseObject(text.substring(DISCOVERY_REPLY_PREFIX.length).trim { it.isWhitespace() || it == '\u0000' }) ?: return null
        val port = (obj["port"] as? Number)?.toInt() ?: DEFAULT_STREAM_PORT
        if (port !in 1..65535) return null
        val version = (obj["version"] as? Number)?.toInt() ?: VERSION
        val name = (obj["name"] as? String)?.takeIf { it.isNotBlank() } ?: "OBS"
        return DiscoveryReply(name, port, version)
    }

    // ---------------------------------------------------------------- handshake

    sealed class HandshakeResult {
        data class Ok(val receiverName: String) : HandshakeResult()
        data class Err(val reason: String) : HandshakeResult()
    }

    /** Parses the receiver's handshake line (without the trailing newline). */
    fun parseHandshakeLine(line: String): HandshakeResult {
        val l = line.trimEnd('\r', '\n')
        return when {
            l == "VCAST/1 OK" -> HandshakeResult.Ok("")
            l.startsWith("VCAST/1 OK ") -> HandshakeResult.Ok(l.substring(11))
            l == "VCAST/1 ERR" -> HandshakeResult.Err("refused")
            l.startsWith("VCAST/1 ERR ") -> HandshakeResult.Err(l.substring(12))
            else -> HandshakeResult.Err("unexpected reply: " + l.take(80))
        }
    }

    /** Reads one '\n'-terminated line of at most [maxLen] bytes. */
    fun readLine(input: InputStream, maxLen: Int = 1024): String {
        val buf = java.io.ByteArrayOutputStream()
        while (true) {
            val b = input.read()
            if (b < 0) throw EOFException("connection closed during handshake")
            if (b == '\n'.code) break
            if (buf.size() >= maxLen) throw IOException("handshake line too long")
            buf.write(b)
        }
        return String(buf.toByteArray(), StandardCharsets.UTF_8)
    }

    // ---------------------------------------------------------------- host[:port]

    data class HostPort(val host: String, val port: Int)

    /** Parses "ip", "ip:port", "host", "host:port", "[v6]" or "[v6]:port". */
    fun parseHostPort(input: String): HostPort? {
        val s = input.trim()
        if (s.isEmpty() || s.any { it.isWhitespace() }) return null
        var host: String
        var portText: String? = null
        if (s.startsWith("[")) {
            val end = s.indexOf(']')
            if (end < 0) return null
            host = s.substring(1, end)
            val rest = s.substring(end + 1)
            if (rest.isNotEmpty()) {
                if (!rest.startsWith(":")) return null
                portText = rest.substring(1)
            }
        } else if (s.count { it == ':' } == 1) {
            host = s.substringBefore(':')
            portText = s.substringAfter(':')
        } else if (s.count { it == ':' } > 1) {
            host = s // bare IPv6 address
        } else {
            host = s
        }
        if (host.isEmpty()) return null
        val port = if (portText == null) {
            DEFAULT_STREAM_PORT
        } else {
            portText.toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
        }
        return HostPort(host, port)
    }

    // ---------------------------------------------------------------- reading frames

    class Frame(val type: Int, val ptsUs: Long, val payload: ByteArray)

    /** Reads frames from a stream (used by tests and tooling). */
    class FrameReader(private val input: InputStream) {
        private val header = ByteArray(HEADER_SIZE)

        /** Returns the next frame, or null on a clean end of stream at a frame boundary. */
        fun read(): Frame? {
            if (!readFully(header, 0, HEADER_SIZE, allowEof = true)) return null
            val type = header[0].toInt() and 0xff
            val length = getInt(header, 1)
            if (length < 0 || length > MAX_PAYLOAD) throw IOException("bad frame length $length")
            val pts = getLong(header, 5)
            val payload = ByteArray(length)
            readFully(payload, 0, length, allowEof = false)
            return Frame(type, pts, payload)
        }

        private fun readFully(b: ByteArray, off: Int, len: Int, allowEof: Boolean): Boolean {
            var n = 0
            while (n < len) {
                val r = input.read(b, off + n, len - n)
                if (r < 0) {
                    if (allowEof && n == 0) return false
                    throw EOFException("truncated frame")
                }
                n += r
            }
            return true
        }
    }
}
