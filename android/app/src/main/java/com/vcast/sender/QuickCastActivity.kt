package com.vcast.sender

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import com.vcast.sender.net.Discovery

/**
 * Invisible activity started by the "Cast to OBS" Quick Settings tile. Starting a
 * MediaProjection needs an activity (the consent dialog, and Android 14+ requires the
 * foreground service to be started from the foreground), so the tile launches this one.
 * It casts to the last used receiver, or opens the app's receiver list if there is none or
 * it does not answer.
 */
class QuickCastActivity : CastFlowActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (savedInstanceState != null) return // recreated in the middle of the flow

        if (CastState.status.isActive) {
            CastService.stop(this)
            finish()
            return
        }
        val last = Prefs.lastReceiver(this)
        if (last == null) {
            openPicker(null)
            return
        }
        Toast.makeText(this, getString(R.string.looking_for, last.name), Toast.LENGTH_SHORT).show()
        val discovery = Discovery(this) {}
        Thread({
            val found = discovery.probe(last)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (found != null) requestCast(found) else openPicker(getString(R.string.not_reachable, last.name))
            }
        }, "vcast-probe").start()
    }

    private fun openPicker(message: String?) {
        if (message != null) Toast.makeText(this, message, Toast.LENGTH_LONG).show()
        startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        finish()
    }

    override fun onCastFlowFinished(started: Boolean) {
        finish()
    }
}
