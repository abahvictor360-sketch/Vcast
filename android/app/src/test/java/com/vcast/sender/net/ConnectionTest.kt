package com.vcast.sender.net

import com.vcast.sender.protocol.Protocol
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/** Runs [Connection] against an in-process receiver on localhost. */
class ConnectionTest {
    private class Recorder : Connection.Listener {
        val closed = CountDownLatch(1)
        @Volatile var error: String? = "not closed"
        override fun onSyncFrameNeeded() {}
        override fun onClosed(error: String?) {
            this.error = error
            closed.countDown()
        }
    }

    private fun receiver(reply: String, onFrames: (Protocol.FrameReader) -> Unit): ServerSocket {
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        Thread {
            server.accept().use { s ->
                val hs = ByteArray(8)
                var n = 0
                while (n < 8) n += s.getInputStream().read(hs, n, 8 - n)
                assertArrayEquals(Protocol.HANDSHAKE, hs)
                s.getOutputStream().write(reply.toByteArray())
                s.getOutputStream().flush()
                onFrames(Protocol.FrameReader(s.getInputStream()))
            }
        }.apply { isDaemon = true }.start()
        return server
    }

    @Test
    fun handshakeFramesPingAndBye() {
        val frames = LinkedBlockingQueue<Protocol.Frame>()
        val server = receiver("VCAST/1 OK Studio\n") { r ->
            while (true) frames.add(r.read() ?: break)
        }
        val rec = Recorder()
        val c = Connection("127.0.0.1", server.localPort, rec)
        c.connect(Protocol.helloPayload("Test", "JVM", "1.0"))
        assertEquals("Studio", c.receiverName)

        val payload = byteArrayOf(0, 0, 0, 1, 0x65, 1, 2, 3)
        val f = Protocol.newFrame(Protocol.TYPE_VIDEO_KEYFRAME, payload.size, 1234)
        System.arraycopy(payload, 0, f, Protocol.HEADER_SIZE, payload.size)
        assertTrue(c.send(Protocol.TYPE_VIDEO_KEYFRAME, f))

        assertEquals(Protocol.TYPE_HELLO, frames.poll(2, TimeUnit.SECONDS)!!.type)
        val key = frames.poll(2, TimeUnit.SECONDS)!!
        assertEquals(Protocol.TYPE_VIDEO_KEYFRAME, key.type)
        assertEquals(1234L, key.ptsUs)
        assertArrayEquals(payload, key.payload)
        // idle -> PING within ~1.25 s
        assertEquals(Protocol.TYPE_PING, frames.poll(3, TimeUnit.SECONDS)!!.type)

        c.close()
        var last: Protocol.Frame?
        do {
            last = frames.poll(2, TimeUnit.SECONDS)
        } while (last != null && last.type == Protocol.TYPE_PING)
        assertNotNull(last)
        assertEquals(Protocol.TYPE_BYE, last!!.type)
        assertTrue(rec.closed.await(2, TimeUnit.SECONDS))
        assertNull(rec.error)
        server.close()
    }

    @Test
    fun refusedHandshake() {
        val server = receiver("VCAST/1 ERR busy\n") {}
        val c = Connection("127.0.0.1", server.localPort, Recorder())
        try {
            c.connect(Protocol.helloPayload("Test", "JVM", "1.0"))
            fail("expected IOException")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("busy"))
        }
        server.close()
    }

    @Test
    fun receiverHangupIsReported() {
        val server = receiver("VCAST/1 OK x\n") { r -> r.read() } // read HELLO, then close
        val rec = Recorder()
        val c = Connection("127.0.0.1", server.localPort, rec)
        c.connect(Protocol.helloPayload("Test", "JVM", "1.0"))
        assertTrue(rec.closed.await(5, TimeUnit.SECONDS))
        assertNotNull(rec.error)
        server.close()
    }
}
