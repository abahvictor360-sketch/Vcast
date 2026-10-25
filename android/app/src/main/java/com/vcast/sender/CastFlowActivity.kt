package com.vcast.sender

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import com.vcast.sender.net.Receiver
import com.vcast.sender.protocol.Protocol

/**
 * The steps every cast goes through, shared by [MainActivity] and [QuickCastActivity]:
 * runtime permissions -> system screen-capture consent -> start [CastService], which goes
 * foreground before calling getMediaProjection() (required on Android 14+).
 */
abstract class CastFlowActivity : Activity() {
    private var pendingTarget: Receiver? = null

    /** Called when the flow ends; [started] is true if the service was started. */
    protected open fun onCastFlowFinished(started: Boolean) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        savedInstanceState?.getStringArray(STATE_TARGET)?.let {
            if (it.size == 3) pendingTarget = Receiver(it[0], it[1], it[2].toIntOrNull() ?: Protocol.DEFAULT_STREAM_PORT)
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        pendingTarget?.let { outState.putStringArray(STATE_TARGET, arrayOf(it.name, it.address, it.port.toString())) }
    }

    protected fun wantsAudio() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && Prefs.audio(this)

    /** Step 1: runtime permissions (notifications on 13+, RECORD_AUDIO for phone audio). */
    protected fun requestCast(target: Receiver) {
        pendingTarget = target
        val needed = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (wantsAudio() && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.RECORD_AUDIO)
        }
        if (needed.isEmpty()) {
            requestCapture()
        } else {
            requestPermissions(needed.toTypedArray(), REQ_PERMISSIONS)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERMISSIONS || pendingTarget == null) return
        if (wantsAudio() && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, R.string.audio_permission_denied, Toast.LENGTH_LONG).show()
        }
        // Casting works without either permission; continue.
        requestCapture()
    }

    /** Step 2: the system screen-capture consent dialog. */
    private fun requestCapture() {
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_CAPTURE)
    }

    /** Step 3: start the foreground service, which then calls getMediaProjection(). */
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_CAPTURE) return
        val target = pendingTarget ?: return
        pendingTarget = null
        if (resultCode != RESULT_OK || data == null) {
            CastState.post(CastState.Status.Idle(getString(R.string.capture_denied)))
            onCastFlowFinished(false)
            return
        }
        val audio = wantsAudio() && checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        val intent = Intent(this, CastService::class.java)
            .setAction(CastService.ACTION_START)
            .putExtra(CastService.EXTRA_RESULT_CODE, resultCode)
            .putExtra(CastService.EXTRA_RESULT_DATA, data)
            .putExtra(CastService.EXTRA_HOST, target.address)
            .putExtra(CastService.EXTRA_PORT, target.port)
            .putExtra(CastService.EXTRA_NAME, target.name)
            .putExtra(CastService.EXTRA_MAX_LONG_SIDE, Prefs.maxLongSide(this))
            .putExtra(CastService.EXTRA_AUDIO, audio)
        Prefs.setLastReceiver(this, target)
        CastState.post(CastState.Status.Connecting(target.name))
        startForegroundService(intent)
        CastTileService.refresh(this)
        onCastFlowFinished(true)
    }

    companion object {
        private const val REQ_PERMISSIONS = 1
        private const val REQ_CAPTURE = 2
        private const val STATE_TARGET = "pendingTarget"
    }
}
