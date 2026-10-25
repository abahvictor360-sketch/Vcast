package com.vcast.sender.media

import android.annotation.SuppressLint
import android.annotation.TargetApi
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Build
import android.util.Log
import com.vcast.sender.protocol.Protocol
import kotlin.math.abs
import kotlin.math.max

/**
 * Captures the audio other apps play (Android 10+, [AudioPlaybackCaptureConfiguration]) and
 * encodes it to AAC-LC 48 kHz stereo 128 kbps. Apps can opt out of playback capture, in which
 * case their audio is simply not included.
 */
@TargetApi(Build.VERSION_CODES.Q)
class AudioEncoder(
    private val projection: MediaProjection,
    private val sink: Sink,
) {
    interface Sink {
        fun onAudioFrame(type: Int, frame: ByteArray)
        fun onAudioError(message: String)
    }

    @Volatile private var running = false
    private var thread: Thread? = null

    /** The caller must hold RECORD_AUDIO. */
    fun start() {
        running = true
        thread = Thread(::loop, "vcast-audio").apply { start() }
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

    @SuppressLint("MissingPermission") // checked by the caller before start()
    private fun loop() {
        var record: AudioRecord? = null
        var codec: MediaCodec? = null
        try {
            val config = AudioPlaybackCaptureConfiguration.Builder(projection)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build()
            val pcmFormat = AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build()
            val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            val r = AudioRecord.Builder()
                .setAudioFormat(pcmFormat)
                .setBufferSizeInBytes(max(minBuf, 4 * CHUNK_BYTES))
                .setAudioPlaybackCaptureConfig(config)
                .build()
            record = r
            if (r.state != AudioRecord.STATE_INITIALIZED) throw IllegalStateException("AudioRecord not initialized")

            val f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, SAMPLE_RATE, CHANNELS)
            f.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            f.setInteger(MediaFormat.KEY_BIT_RATE, BITRATE)
            f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, CHUNK_BYTES * 2)
            val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
            c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            c.start()
            codec = c
            r.startRecording()

            val info = MediaCodec.BufferInfo()
            var baseUs = -1L
            var samples = 0L
            var lastPts = Long.MIN_VALUE
            while (running) {
                val inIndex = c.dequeueInputBuffer(20_000)
                if (inIndex >= 0) {
                    val inBuf = c.getInputBuffer(inIndex)!!
                    inBuf.clear()
                    val want = minOf(CHUNK_BYTES, inBuf.remaining())
                    val n = r.read(inBuf, want, AudioRecord.READ_BLOCKING)
                    if (n < 0) throw IllegalStateException("AudioRecord.read failed: $n")
                    val nowUs = System.nanoTime() / 1000
                    val chunkUs = (n / BYTES_PER_FRAME) * 1_000_000L / SAMPLE_RATE
                    // Timestamps follow the sample count, re-anchored to the monotonic clock
                    // when they drift (e.g. after the capture stalled).
                    var pts = if (baseUs < 0) -1 else baseUs + samples * 1_000_000L / SAMPLE_RATE
                    if (pts < 0 || abs(pts - (nowUs - chunkUs)) > 200_000) {
                        baseUs = nowUs - chunkUs
                        samples = 0
                        pts = baseUs
                    }
                    if (pts <= lastPts) pts = lastPts + 1
                    lastPts = pts
                    samples += n / BYTES_PER_FRAME
                    c.queueInputBuffer(inIndex, 0, max(n, 0), pts, 0)
                }
                drain(c, info)
            }
        } catch (e: Exception) {
            Log.e(TAG, "audio capture failed", e)
            if (running) sink.onAudioError("Audio capture stopped: ${e.message ?: e.javaClass.simpleName}")
        } finally {
            try {
                record?.stop()
            } catch (_: Exception) {
            }
            record?.release()
            try {
                codec?.stop()
            } catch (_: Exception) {
            }
            codec?.release()
        }
    }

    private fun drain(c: MediaCodec, info: MediaCodec.BufferInfo) {
        while (true) {
            val index = c.dequeueOutputBuffer(info, 0)
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                // csd-0 normally also arrives as a CODEC_CONFIG buffer; this covers encoders
                // that only put it in the format.
                c.outputFormat.getByteBuffer("csd-0")?.let { b ->
                    if (!configSent) {
                        val asc = ByteArray(b.remaining())
                        b.duplicate().get(asc)
                        sendConfig(asc)
                    }
                }
                continue
            }
            if (index < 0) return
            try {
                if (info.size <= 0) continue
                val buf = c.getOutputBuffer(index) ?: continue
                buf.position(info.offset)
                buf.limit(info.offset + info.size)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    val asc = ByteArray(info.size)
                    buf.get(asc)
                    sendConfig(asc)
                } else if (configSent) {
                    val frame = Protocol.newFrame(Protocol.TYPE_AUDIO_FRAME, info.size, info.presentationTimeUs)
                    buf.get(frame, Protocol.HEADER_SIZE, info.size)
                    sink.onAudioFrame(Protocol.TYPE_AUDIO_FRAME, frame)
                }
            } finally {
                c.releaseOutputBuffer(index, false)
            }
        }
    }

    private var configSent = false

    private fun sendConfig(asc: ByteArray) {
        val payload = Protocol.audioConfigPayload(Protocol.AUDIO_CODEC_AAC, SAMPLE_RATE, CHANNELS, asc)
        sink.onAudioFrame(
            Protocol.TYPE_AUDIO_CONFIG,
            Protocol.encodeFrame(Protocol.TYPE_AUDIO_CONFIG, payload, System.nanoTime() / 1000),
        )
        configSent = true
    }

    companion object {
        private const val TAG = "VcastAudio"
        const val SAMPLE_RATE = 48_000
        const val CHANNELS = 2
        const val BITRATE = 128_000
        private const val BYTES_PER_FRAME = 4 // 16-bit stereo
        private const val CHUNK_BYTES = 1024 * BYTES_PER_FRAME // one AAC frame worth of samples
    }
}
