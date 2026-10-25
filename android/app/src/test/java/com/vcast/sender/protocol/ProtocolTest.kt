package com.vcast.sender.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException

class ProtocolTest {
    @Test
    fun headerLayoutIsBigEndian() {
        val f = Protocol.encodeFrame(Protocol.TYPE_VIDEO_KEYFRAME, byteArrayOf(1, 2, 3), 0x0102030405060708L)
        assertArrayEquals(
            byteArrayOf(0x12, 0, 0, 0, 3, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3),
            f,
        )
    }

    @Test
    fun negativePtsRoundTrips() {
        val f = Protocol.newFrame(Protocol.TYPE_PING, 0, -5L)
        assertEquals(13, f.size)
        val r = Protocol.FrameReader(ByteArrayInputStream(f)).read()!!
        assertEquals(Protocol.TYPE_PING, r.type)
        assertEquals(-5L, r.ptsUs)
        assertEquals(0, r.payload.size)
    }

    @Test
    fun readerParsesSequenceAndCleanEof() {
        val out = ByteArrayOutputStream()
        out.write(Protocol.encodeFrame(Protocol.TYPE_HELLO, Protocol.helloPayload("Pixel \"8\"", "Android 15", "1.0"), 1))
        out.write(Protocol.encodeFrame(Protocol.TYPE_VIDEO_CONFIG, Protocol.videoConfigPayload(1, byteArrayOf(0, 0, 0, 1, 0x67)), 2))
        out.write(Protocol.newFrame(Protocol.TYPE_BYE, 0, 3))
        val reader = Protocol.FrameReader(ByteArrayInputStream(out.toByteArray()))
        val hello = reader.read()!!
        assertEquals(Protocol.TYPE_HELLO, hello.type)
        val json = Json.parseObject(String(hello.payload, Charsets.UTF_8))!!
        assertEquals("Pixel \"8\"", json["device"])
        assertEquals("Android 15", json["os"])
        assertEquals("1.0", json["app"])
        val cfg = reader.read()!!
        assertArrayEquals(byteArrayOf(1, 0, 0, 0, 1, 0x67), cfg.payload)
        assertEquals(Protocol.TYPE_BYE, reader.read()!!.type)
        assertNull(reader.read())
    }

    @Test(expected = EOFException::class)
    fun truncatedFrameIsAnError() {
        val f = Protocol.encodeFrame(Protocol.TYPE_AUDIO_FRAME, ByteArray(10), 0)
        Protocol.FrameReader(ByteArrayInputStream(f.copyOf(f.size - 1))).read()
    }

    @Test
    fun audioConfigPayload() {
        val p = Protocol.audioConfigPayload(Protocol.AUDIO_CODEC_AAC, 48000, 2, byteArrayOf(0x11, 0x90.toByte()))
        assertArrayEquals(byteArrayOf(1, 0, 0, 0xBB.toByte(), 0x80.toByte(), 2, 0x11, 0x90.toByte()), p)
    }

    @Test
    fun discoveryReply() {
        val d = "VCAST!\n{\"name\":\"Living room OBS\",\"port\":48690,\"version\":1}".toByteArray()
        assertEquals(Protocol.DiscoveryReply("Living room OBS", 48690, 1), Protocol.parseDiscoveryReply(d))
        val withGarbage = d + ByteArray(5)
        assertEquals(Protocol.DiscoveryReply("Living room OBS", 48690, 1), Protocol.parseDiscoveryReply(withGarbage))
        assertEquals(
            Protocol.DiscoveryReply("Aé", 5000, 1),
            Protocol.parseDiscoveryReply("VCAST!\n{\"name\":\"A\\u00e9\",\"port\":5000,\"extra\":[1,{\"x\":null}]}\n".toByteArray()),
        )
        assertNull(Protocol.parseDiscoveryReply("VCAST?\n".toByteArray()))
        assertNull(Protocol.parseDiscoveryReply("VCAST!\n{\"port\":70000}".toByteArray()))
        assertNull(Protocol.parseDiscoveryReply("VCAST!\nnot json".toByteArray()))
    }

    @Test
    fun handshakeLine() {
        assertEquals(Protocol.HandshakeResult.Ok("Living room"), Protocol.parseHandshakeLine("VCAST/1 OK Living room"))
        assertEquals(Protocol.HandshakeResult.Ok("x"), Protocol.parseHandshakeLine("VCAST/1 OK x\r"))
        assertEquals(Protocol.HandshakeResult.Err("busy"), Protocol.parseHandshakeLine("VCAST/1 ERR busy"))
        assertTrue(Protocol.parseHandshakeLine("HTTP/1.1 400") is Protocol.HandshakeResult.Err)
        assertEquals("abc", Protocol.readLine(ByteArrayInputStream("abc\nrest".toByteArray())))
    }

    @Test
    fun hostPort() {
        assertEquals(Protocol.HostPort("192.168.1.20", 48690), Protocol.parseHostPort(" 192.168.1.20 "))
        assertEquals(Protocol.HostPort("192.168.1.20", 5000), Protocol.parseHostPort("192.168.1.20:5000"))
        assertEquals(Protocol.HostPort("pc.local", 48690), Protocol.parseHostPort("pc.local"))
        assertEquals(Protocol.HostPort("fe80::1", 48690), Protocol.parseHostPort("fe80::1"))
        assertEquals(Protocol.HostPort("fe80::1", 7), Protocol.parseHostPort("[fe80::1]:7"))
        assertNull(Protocol.parseHostPort(""))
        assertNull(Protocol.parseHostPort("1.2.3.4:"))
        assertNull(Protocol.parseHostPort("1.2.3.4:99999"))
        assertNull(Protocol.parseHostPort("a b"))
    }
}
