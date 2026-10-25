package com.vcast.sender.media

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Surface
import com.vcast.sender.protocol.Protocol
import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * Captures the screen through a [VirtualDisplay] into an H.264 [MediaCodec] encoder (Surface
 * input) and hands encoded frames to [sink] as complete protocol frames.
 *
 * A single thread owns the codec: it drains output and applies size changes (rotation,
 * resized captured app window). Android 14+ only allows one VirtualDisplay per
 * MediaProjection, so size changes resize the existing display and swap its surface.
 */
class VideoEncoder(
    private val projection: MediaProjection,
    private val maxLongSide: Int,
    private val densityDpi: Int,
    private val sink: Sink,
) {
    interface Sink {
        /** [frame] is a complete protocol frame (see [Protocol.newFrame]). */
        fun onVideoFrame(type: Int, frame: ByteArray)
        fun onVideoInfo(width: Int, height: Int, fps: Int)
        fun onVideoError(message: String)
    }

    private data class Size(val width: Int, val height: Int)

    @Volatile private var running = false
    private var thread: Thread? = null
    private val pendingSource = AtomicReference<Size?>(null)
    @Volatile private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var currentSize: Size? = null
    private var lastPtsUs = Long.MIN_VALUE
    private var configSent = false
    @Volatile private var lastSyncRequestNs = 0L

    /** Starts capturing a source of [sourceWidth] x [sourceHeight] pixels. */
    fun start(sourceWidth: Int, sourceHeight: Int) {
        pendingSource.set(Size(sourceWidth, sourceHeight))
        running = true
        thread = Thread(::loop, "vcast-video").apply { start() }
    }

    /** The captured screen / window changed size (e.g. rotation). Thread-safe. */
    fun onSourceSizeChanged(width: Int, height: Int) {
        if (width > 0 && height > 0) pendingSource.set(Size(width, height))
    }

    /** Asks the encoder for an IDR frame (rate-limited). Thread-safe. */
    fun requestSyncFrame() {
        val now = System.nanoTime()
        if (now - lastSyncRequestNs < 300_000_000L) return
        lastSyncRequestNs = now
        try {
            codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (e: IllegalStateException) {
            // codec is being reconfigured; the new one starts with an IDR anyway
        }
    }

    fun stop() {
        running = false
        thread?.let {
            if (it !== Thread.currentThread()) {
                try {
                    it.join(2000)
                } catch (e: InterruptedException) {
                    Thread.currentThread().interrupt()
                }
            }
        }
        thread = null
    }

    private fun loop() {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                pendingSource.getAndSet(null)?.let { reconfigure(it) }
                val c = codec ?: break
                val index = c.dequeueOutputBuffer(info, 50_000)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> onFormatChanged(c.outputFormat)
                    index >= 0 -> {
                        handleOutput(c, index, info)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "video encoder failed", e)
            if (running) sink.onVideoError("Video encoder error: ${e.message ?: e.javaClass.simpleName}")
        } finally {
            releaseCodec()
            try {
                virtualDisplay?.release()
            } catch (e: Exception) {
                Log.w(TAG, "release virtual display", e)
            }
            virtualDisplay = null
        }
    }

    private fun handleOutput(c: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        try {
            if (info.size <= 0) return
            val buf = c.getOutputBuffer(index) ?: return
            buf.position(info.offset)
            buf.limit(info.offset + info.size)
            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                val sps = ByteArray(info.size)
                buf.get(sps)
                sendConfig(sps)
                return
            }
            if (!configSent) {
                // Some encoders only publish csd-0/csd-1 in the output format.
                val f = c.outputFormat
                val cfg = csdOf(f)
                if (cfg == null) {
                    Log.w(TAG, "frame before codec config; dropping")
                    requestSyncFrame()
                    return
                }
                sendConfig(cfg)
            }
            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
            val type = if (key) Protocol.TYPE_VIDEO_KEYFRAME else Protocol.TYPE_VIDEO_FRAME
            var pts = info.presentationTimeUs
            if (pts <= lastPtsUs) pts = lastPtsUs + 1
            lastPtsUs = pts
            val frame = Protocol.newFrame(type, info.size, pts)
            buf.get(frame, Protocol.HEADER_SIZE, info.size)
            sink.onVideoFrame(type, frame)
        } finally {
            c.releaseOutputBuffer(index, false)
        }
    }

    private fun onFormatChanged(format: MediaFormat) {
        if (!configSent) csdOf(format)?.let { sendConfig(it) }
    }

    private fun csdOf(format: MediaFormat): ByteArray? {
        val out = ByteArrayOutputStream()
        for (key in arrayOf("csd-0", "csd-1")) {
            val b = format.getByteBuffer(key) ?: continue
            val dup = b.duplicate()
            dup.position(0)
            val arr = ByteArray(dup.remaining())
            dup.get(arr)
            out.write(arr)
        }
        return if (out.size() > 0) out.toByteArray() else null
    }

    private fun sendConfig(parameterSets: ByteArray) {
        val payload = Protocol.videoConfigPayload(Protocol.VIDEO_CODEC_H264, parameterSets)
        val pts = max(lastPtsUs, System.nanoTime() / 1000)
        sink.onVideoFrame(Protocol.TYPE_VIDEO_CONFIG, Protocol.encodeFrame(Protocol.TYPE_VIDEO_CONFIG, payload, pts))
        configSent = true
    }

    private fun reconfigure(source: Size) {
        val caps = probeCapabilities()
        val target = targetSize(source, caps)
        if (target == currentSize && codec != null) return
        Log.i(TAG, "source ${source.width}x${source.height} -> encode ${target.width}x${target.height}")

        virtualDisplay?.surface = null
        releaseCodec()

        val fps = if (caps?.videoCapabilities?.areSizeAndRateSupported(target.width, target.height, 60.0) == true) 60 else 30
        val bitrate = if (maxLongSide > 1280) 8_000_000 else 4_000_000
        val c = MediaCodec.createEncoderByType(MIME)
        val surface: Surface
        try {
            configure(c, target, fps, bitrate, caps, full = true)
        } catch (e: Exception) {
            Log.w(TAG, "configure with extras failed, retrying minimal", e)
            c.reset()
            configure(c, target, fps, bitrate, caps, full = false)
        }
        surface = c.createInputSurface()
        c.start()
        codec = c
        inputSurface = surface
        currentSize = target
        configSent = false

        val vd = virtualDisplay
        if (vd == null) {
            virtualDisplay = projection.createVirtualDisplay(
                "Vcast",
                target.width,
                target.height,
                densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                surface,
                null,
                null,
            )
        } else {
            vd.resize(target.width, target.height, densityDpi)
            vd.surface = surface
        }
        sink.onVideoInfo(target.width, target.height, fps)
        requestSyncFrame()
    }

    private fun configure(
        c: MediaCodec,
        size: Size,
        fps: Int,
        bitrate: Int,
        caps: MediaCodecInfo.CodecCapabilities?,
        full: Boolean,
    ) {
        val f = MediaFormat.createVideoFormat(MIME, size.width, size.height)
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        // Keep producing frames on a static screen so the receiver never times out.
        f.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)
        if (full) {
            f.setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                caps?.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency) == true
            ) {
                f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                f.setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, fps.toFloat())
            }
        }
        c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    }

    private fun probeCapabilities(): MediaCodecInfo.CodecCapabilities? = try {
        val c = MediaCodec.createEncoderByType(MIME)
        try {
            c.codecInfo.getCapabilitiesForType(MIME)
        } finally {
            c.release()
        }
    } catch (e: Exception) {
        null
    }

    /** Scales [source] so its long side is <= [maxLongSide], with sizes the encoder accepts. */
    private fun targetSize(source: Size, caps: MediaCodecInfo.CodecCapabilities?): Size {
        val vc = caps?.videoCapabilities
        var limit = maxLongSide
        while (true) {
            for (align in intArrayOf(2, 16)) {
                val s = scaled(source, limit, align)
                if (vc == null || vc.isSizeSupported(s.width, s.height)) return s
            }
            if (limit <= 320) return scaled(source, limit, 16)
            limit = (limit * 3) / 4
        }
    }

    private fun releaseCodec() {
        val c = codec ?: return
        codec = null
        try {
            c.stop()
        } catch (e: Exception) {
            Log.w(TAG, "codec stop", e)
        }
        c.release()
        inputSurface?.release()
        inputSurface = null
    }

    companion object {
        private const val TAG = "VcastVideo"
        const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC

        private fun scaled(source: Size, maxLong: Int, align: Int): Size {
            val long = max(source.width, source.height)
            val scale = if (long > maxLong) maxLong.toDouble() / long else 1.0
            fun dim(v: Int): Int = max(align, ((v * scale) / align).roundToInt() * align)
            var w = dim(source.width)
            var h = dim(source.height)
            // rounding may overshoot the limit by one step
            while (max(w, h) > maxLong) {
                if (w >= h) w -= align else h -= align
            }
            return Size(w, h)
        }

        /** Exposed for tests: computes the encode size without codec constraints. */
        fun computeSize(width: Int, height: Int, maxLong: Int, align: Int = 2): Pair<Int, Int> {
            val s = scaled(Size(width, height), maxLong, align)
            return s.width to s.height
        }
    }
}
