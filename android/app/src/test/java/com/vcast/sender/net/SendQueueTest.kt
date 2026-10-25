package com.vcast.sender.net

import com.vcast.sender.protocol.Protocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SendQueueTest {
    private fun frame(type: Int, size: Int) = Protocol.newFrame(type, size - Protocol.HEADER_SIZE, 0)

    private fun drainTypes(q: SendQueue): List<Int> {
        val types = ArrayList<Int>()
        while (true) {
            val f = q.take(0) ?: break
            types.add(f[0].toInt() and 0xff)
        }
        return types
    }

    @Test
    fun dropsNonKeyVideoUntilNextKeyframe() {
        val q = SendQueue(softLimitBytes = 1000, hardLimitBytes = 5000)
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_CONFIG, frame(Protocol.TYPE_VIDEO_CONFIG, 50)))
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_KEYFRAME, frame(Protocol.TYPE_VIDEO_KEYFRAME, 600)))
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 300)))
        // over the soft limit -> dropped, and the following P-frames too even though they would fit
        assertEquals(SendQueue.Result.DROPPED_NEED_SYNC, q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 300)))
        q.take(0); q.take(0); q.take(0) // network catches up
        assertEquals(SendQueue.Result.DROPPED_NEED_SYNC, q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 20)))
        // audio is not affected
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_AUDIO_FRAME, frame(Protocol.TYPE_AUDIO_FRAME, 30)))
        // keyframe resumes video
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_KEYFRAME, frame(Protocol.TYPE_VIDEO_KEYFRAME, 500)))
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 20)))
        assertEquals(
            listOf(Protocol.TYPE_AUDIO_FRAME, Protocol.TYPE_VIDEO_KEYFRAME, Protocol.TYPE_VIDEO_FRAME),
            drainTypes(q),
        )
        assertEquals(2L, q.droppedVideoFrames)
    }

    @Test
    fun keyframeEvictsStaleVideoButKeepsConfigAndAudio() {
        val q = SendQueue(softLimitBytes = 1000, hardLimitBytes = 5000)
        q.offer(Protocol.TYPE_VIDEO_CONFIG, frame(Protocol.TYPE_VIDEO_CONFIG, 50))
        q.offer(Protocol.TYPE_VIDEO_KEYFRAME, frame(Protocol.TYPE_VIDEO_KEYFRAME, 400))
        q.offer(Protocol.TYPE_AUDIO_FRAME, frame(Protocol.TYPE_AUDIO_FRAME, 40))
        q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 400))
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_VIDEO_KEYFRAME, frame(Protocol.TYPE_VIDEO_KEYFRAME, 900)))
        assertEquals(
            listOf(Protocol.TYPE_VIDEO_CONFIG, Protocol.TYPE_AUDIO_FRAME, Protocol.TYPE_VIDEO_KEYFRAME),
            drainTypes(q),
        )
        assertEquals(0L, q.bytes)
    }

    @Test
    fun hugeKeyframeIsDroppedAboveHardLimit() {
        val q = SendQueue(softLimitBytes = 100, hardLimitBytes = 1000)
        q.offer(Protocol.TYPE_AUDIO_FRAME, frame(Protocol.TYPE_AUDIO_FRAME, 900))
        assertEquals(SendQueue.Result.DROPPED_NEED_SYNC, q.offer(Protocol.TYPE_VIDEO_KEYFRAME, frame(Protocol.TYPE_VIDEO_KEYFRAME, 200)))
        assertEquals(SendQueue.Result.DROPPED, q.offer(Protocol.TYPE_AUDIO_FRAME, frame(Protocol.TYPE_AUDIO_FRAME, 200)))
        // control frames are never dropped
        assertEquals(SendQueue.Result.QUEUED, q.offer(Protocol.TYPE_PING, frame(Protocol.TYPE_PING, 13)))
    }

    @Test
    fun closeDrainsFinalFrameThenReturnsNull() {
        val q = SendQueue()
        q.offer(Protocol.TYPE_VIDEO_FRAME, frame(Protocol.TYPE_VIDEO_FRAME, 20))
        q.clear()
        q.offerFinal(Protocol.newFrame(Protocol.TYPE_BYE, 0, 0))
        assertEquals(SendQueue.Result.CLOSED, q.offer(Protocol.TYPE_AUDIO_FRAME, frame(Protocol.TYPE_AUDIO_FRAME, 20)))
        assertEquals(listOf(Protocol.TYPE_BYE), drainTypes(q))
        assertNull(q.take(10))
    }
}
