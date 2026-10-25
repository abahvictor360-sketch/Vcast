package com.vcast.sender

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.Point
import android.graphics.drawable.Icon
import android.hardware.display.DisplayManager
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.DisplayMetrics
import android.util.Log
import android.view.Display
import com.vcast.sender.media.AudioEncoder
import com.vcast.sender.media.VideoEncoder
import com.vcast.sender.net.Connection
import com.vcast.sender.protocol.Protocol
import java.io.IOException
import java.util.Locale
import java.util.concurrent.Executors

/**
 * Foreground service (type mediaProjection) that owns one casting session: the
 * MediaProjection, the encoders and the connection to the receiver.
 */
class CastService : Service() {
    private val main = Handler(Looper.getMainLooper())
    private val control = Executors.newSingleThreadExecutor { r -> Thread(r, "vcast-control") }
    private var session: Session? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> start(intent)
            ACTION_STOP -> {
                val s = session
                if (s == null) {
                    stopSelfCompletely()
                } else {
                    s.stop(null)
                }
            }
            else -> if (session == null) stopSelfCompletely()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        session?.stop(null)
        control.shutdown()
        super.onDestroy()
    }

    private fun start(intent: Intent) {
        session?.let {
            session = null
            it.stop(null, replaced = true)
        }
        val host = intent.getStringExtra(EXTRA_HOST)
        val port = intent.getIntExtra(EXTRA_PORT, Protocol.DEFAULT_STREAM_PORT)
        val maxLongSide = intent.getIntExtra(EXTRA_MAX_LONG_SIDE, 1920)
        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
        val data: Intent? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(EXTRA_RESULT_DATA)
        }
        var audio = intent.getBooleanExtra(EXTRA_AUDIO, false) &&
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        if (host == null || data == null) {
            CastState.post(CastState.Status.Error("Missing cast parameters"))
            stopSelfCompletely()
            return
        }
        val name = intent.getStringExtra(EXTRA_NAME) ?: host

        // Android 14 requires the service to be in the foreground before getMediaProjection().
        try {
            goForeground(getString(R.string.notification_connecting, name), audio)
        } catch (e: Exception) {
            Log.w(TAG, "startForeground with microphone type failed", e)
            audio = false
            try {
                goForeground(getString(R.string.notification_connecting, name), false)
            } catch (e2: Exception) {
                Log.e(TAG, "startForeground failed", e2)
                CastState.post(CastState.Status.Error("Could not start screen capture: ${e2.message}"))
                stopSelfCompletely()
                return
            }
        }

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val projection = try {
            mpm.getMediaProjection(resultCode, data)
        } catch (e: Exception) {
            Log.e(TAG, "getMediaProjection", e)
            null
        }
        if (projection == null) {
            CastState.post(CastState.Status.Error("Screen capture permission was not granted"))
            stopSelfCompletely()
            return
        }
        val s = Session(projection, host, port, name, maxLongSide, audio)
        session = s
        s.begin()
    }

    private fun goForeground(text: String, microphone: Boolean) {
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            if (microphone && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            }
            startForeground(NOTIFICATION_ID, n, type)
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW),
            )
        }
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this,
            1,
            Intent(this, CastService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stopAction = Notification.Action.Builder(
            Icon.createWithResource(this, R.drawable.ic_stop),
            getString(R.string.stop),
            stop,
        ).build()
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_cast)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(stopAction)
            .build()
    }

    private fun stopSelfCompletely() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun defaultDisplay(): Display? =
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).getDisplay(Display.DEFAULT_DISPLAY)

    /** Real (unscaled, rotated) size of the default display and its density. */
    private fun displayInfo(): Triple<Int, Int, Int> {
        val d = defaultDisplay() ?: return Triple(1080, 1920, 320)
        val metrics = DisplayMetrics()
        val p = Point()
        @Suppress("DEPRECATION")
        d.getRealMetrics(metrics)
        @Suppress("DEPRECATION")
        d.getRealSize(p)
        return Triple(p.x, p.y, metrics.densityDpi)
    }

    /** One casting session. All transitions run on the [control] thread. */
    private inner class Session(
        private val projection: MediaProjection,
        private val host: String,
        private val port: Int,
        private val name: String,
        private val maxLongSide: Int,
        private val audioEnabled: Boolean,
    ) : Connection.Listener, VideoEncoder.Sink, AudioEncoder.Sink {
        @Volatile private var stopped = false
        @Volatile private var connection: Connection? = null
        @Volatile private var video: VideoEncoder? = null
        private var audio: AudioEncoder? = null
        @Volatile private var contentSize: Point? = null
        @Volatile private var videoInfo = ""
        @Volatile private var audioNote = ""
        @Volatile private var receiverLabel = name

        private val projectionCallback = object : MediaProjection.Callback() {
            override fun onStop() {
                stop("Screen sharing was stopped")
            }

            override fun onCapturedContentResize(width: Int, height: Int) {
                // Android 14+: size of the captured display or single app window.
                contentSize = Point(width, height)
                video?.onSourceSizeChanged(width, height)
            }
        }

        private val displayListener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {}
            override fun onDisplayRemoved(displayId: Int) {}
            override fun onDisplayChanged(displayId: Int) {
                if (displayId != Display.DEFAULT_DISPLAY || contentSize != null) return
                val (w, h, _) = displayInfo()
                video?.onSourceSizeChanged(w, h)
            }
        }

        fun begin() {
            // Must be registered before createVirtualDisplay() on Android 14+.
            projection.registerCallback(projectionCallback, main)
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).registerDisplayListener(displayListener, main)
            CastState.post(CastState.Status.Connecting(name))
            control.execute { connectAndStart() }
        }

        private fun connectAndStart() {
            if (stopped) return
            val conn = Connection(host, port, this)
            connection = conn
            try {
                conn.connect(hello())
            } catch (e: IOException) {
                Log.w(TAG, "connect failed", e)
                stopNow(describeConnectError(e))
                return
            }
            if (stopped) {
                conn.close()
                return
            }
            if (conn.receiverName.isNotBlank()) receiverLabel = conn.receiverName
            main.post { if (!stopped) updateNotification(getString(R.string.notification_casting, receiverLabel)) }

            val (dw, dh, dpi) = displayInfo()
            val src = contentSize ?: Point(dw, dh)
            val v = VideoEncoder(projection, maxLongSide, dpi, this)
            video = v
            try {
                v.start(src.x, src.y)
            } catch (e: Exception) {
                Log.e(TAG, "video start", e)
                stopNow("Could not start the video encoder: ${e.message}")
                return
            }
            v.requestSyncFrame()
            if (audioEnabled && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val a = AudioEncoder(projection, this)
                audio = a
                a.start()
            }
            publishCasting(null)
        }

        private fun describeConnectError(e: IOException): String {
            val msg = e.message ?: e.javaClass.simpleName
            return when {
                msg.contains("refused", true) -> "Could not connect to $host:$port (is OBS running with a Vcast Receiver source?)"
                msg.contains("timed out", true) || msg.contains("timeout", true) ->
                    "No answer from $host:$port (same Wi-Fi? firewall?)"
                else -> "Could not connect to $host:$port: $msg"
            }
        }

        private fun hello(): ByteArray {
            val device = (Build.MANUFACTURER.replaceFirstChar { it.titlecase(Locale.ROOT) } + " " + Build.MODEL).trim()
            return Protocol.helloPayload(device, "Android " + Build.VERSION.RELEASE, BuildConfig.VERSION_NAME)
        }

        private fun publishCasting(stats: Connection.Stats?) {
            if (stopped) return
            val parts = ArrayList<String>()
            if (videoInfo.isNotEmpty()) parts.add(videoInfo)
            if (stats != null) {
                parts.add(String.format(Locale.ROOT, "%.1f Mbit/s", stats.bitsPerSecond / 1e6))
                if (stats.droppedVideoFrames > 0) parts.add("${stats.droppedVideoFrames} frames dropped")
            }
            if (audioNote.isNotEmpty()) parts.add(audioNote)
            CastState.post(CastState.Status.Casting(receiverLabel, parts.joinToString(" · ")))
        }

        /** Stops the session from any thread. [error] null means stopped by the user. */
        fun stop(error: String?, replaced: Boolean = false) {
            if (stopped) return
            try {
                control.execute { stopNow(error, replaced) }
            } catch (e: java.util.concurrent.RejectedExecutionException) {
                stopNow(error, replaced) // service already destroyed
            }
        }

        private fun stopNow(error: String?, replaced: Boolean = false) {
            if (stopped) return
            stopped = true
            Log.i(TAG, "stopping: ${error ?: "by user"}")
            video?.stop()
            audio?.stop()
            connection?.close()
            main.post {
                try {
                    (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).unregisterDisplayListener(displayListener)
                    projection.unregisterCallback(projectionCallback)
                    projection.stop()
                } catch (e: Exception) {
                    Log.w(TAG, "projection stop", e)
                }
                if (session === this) session = null
                if (!replaced) {
                    CastState.post(if (error == null) CastState.Status.Idle("Stopped") else CastState.Status.Error(error))
                    if (session == null) stopSelfCompletely()
                }
            }
        }

        // Connection.Listener
        override fun onSyncFrameNeeded() {
            video?.requestSyncFrame()
        }

        override fun onClosed(error: String?) {
            if (error != null) stop(error)
        }

        override fun onStats(stats: Connection.Stats) {
            publishCasting(stats)
        }

        // VideoEncoder.Sink
        override fun onVideoFrame(type: Int, frame: ByteArray) {
            connection?.send(type, frame)
        }

        override fun onVideoInfo(width: Int, height: Int, fps: Int) {
            videoInfo = "${width}×$height · $fps fps"
            publishCasting(null)
        }

        override fun onVideoError(message: String) {
            stop(message)
        }

        // AudioEncoder.Sink
        override fun onAudioFrame(type: Int, frame: ByteArray) {
            connection?.send(type, frame)
        }

        override fun onAudioError(message: String) {
            audioNote = "no audio"
            Log.w(TAG, message)
        }
    }

    companion object {
        private const val TAG = "VcastService"
        private const val CHANNEL_ID = "cast"
        private const val NOTIFICATION_ID = 1

        const val ACTION_START = "com.vcast.sender.START"
        const val ACTION_STOP = "com.vcast.sender.STOP"
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_NAME = "name"
        const val EXTRA_MAX_LONG_SIDE = "maxLongSide"
        const val EXTRA_AUDIO = "audio"

        fun stop(context: Context) {
            context.startService(Intent(context, CastService::class.java).setAction(ACTION_STOP))
        }
    }
}
