package com.vcast.sender.net

import com.vcast.sender.protocol.Protocol

/**
 * Bounded queue between the encoders and the single network writer thread.
 *
 * Frames are queued whole (header + payload) and are either sent completely or not at all.
 * When the network cannot keep up:
 *  - non-key video frames are dropped once more than [softLimitBytes] are queued, and from
 *    then on every non-key video frame is dropped until the next keyframe (P-frames are
 *    useless without the frames they reference);
 *  - a keyframe evicts the stale video frames still waiting in the queue;
 *  - audio frames are only dropped above [hardLimitBytes];
 *  - configuration / control frames are never dropped.
 * Every video drop returns [Result.DROPPED_NEED_SYNC] so the caller can ask the encoder for a
 * new sync frame.
 */
class SendQueue(
    private val softLimitBytes: Int = 1 shl 20,
    private val hardLimitBytes: Int = 8 shl 20,
) {
    enum class Result { QUEUED, DROPPED, DROPPED_NEED_SYNC, CLOSED }

    private class Item(val type: Int, val data: ByteArray)

    private val lock = Object()
    private val items = ArrayDeque<Item>()
    private var queuedBytes = 0L
    private var droppingVideo = false
    private var closed = false

    @Volatile var droppedVideoFrames = 0L
        private set
    @Volatile var droppedAudioFrames = 0L
        private set

    val isClosed: Boolean get() = synchronized(lock) { closed }
    val size: Int get() = synchronized(lock) { items.size }
    val bytes: Long get() = synchronized(lock) { queuedBytes }

    fun offer(type: Int, frame: ByteArray): Result = synchronized(lock) {
        if (closed) return Result.CLOSED
        val n = frame.size
        when (type) {
            Protocol.TYPE_VIDEO_FRAME -> {
                if (droppingVideo || queuedBytes + n > softLimitBytes) {
                    droppingVideo = true
                    droppedVideoFrames++
                    return Result.DROPPED_NEED_SYNC
                }
            }
            Protocol.TYPE_VIDEO_KEYFRAME -> {
                if (queuedBytes + n > softLimitBytes) evictVideoLocked()
                if (queuedBytes + n > hardLimitBytes) {
                    droppingVideo = true
                    droppedVideoFrames++
                    return Result.DROPPED_NEED_SYNC
                }
                droppingVideo = false
            }
            Protocol.TYPE_AUDIO_FRAME -> {
                if (queuedBytes + n > hardLimitBytes) {
                    droppedAudioFrames++
                    return Result.DROPPED
                }
            }
        }
        items.addLast(Item(type, frame))
        queuedBytes += n
        lock.notifyAll()
        Result.QUEUED
    }

    /** Removes queued video frames; a newer keyframe makes them obsolete. */
    private fun evictVideoLocked() {
        val it = items.iterator()
        while (it.hasNext()) {
            val item = it.next()
            if (item.type == Protocol.TYPE_VIDEO_FRAME || item.type == Protocol.TYPE_VIDEO_KEYFRAME) {
                it.remove()
                queuedBytes -= item.data.size
                droppedVideoFrames++
            }
        }
    }

    /** Waits up to [timeoutMs] for a frame. Returns null on timeout or when closed and empty. */
    fun take(timeoutMs: Long): ByteArray? = synchronized(lock) {
        val deadline = System.nanoTime() + timeoutMs * 1_000_000
        while (items.isEmpty()) {
            if (closed) return null
            val left = (deadline - System.nanoTime()) / 1_000_000
            if (left <= 0) return null
            lock.wait(left)
        }
        val item = items.removeFirst()
        queuedBytes -= item.data.size
        item.data
    }

    /** Drops everything that is queued (used before sending BYE). */
    fun clear() = synchronized(lock) {
        items.clear()
        queuedBytes = 0
    }

    /** Queues [frame] even if the queue is closed for producers (for BYE). */
    fun offerFinal(frame: ByteArray) = synchronized(lock) {
        items.addLast(Item(Protocol.TYPE_BYE, frame))
        queuedBytes += frame.size
        closed = true
        lock.notifyAll()
    }

    fun close() = synchronized(lock) {
        closed = true
        lock.notifyAll()
    }
}
