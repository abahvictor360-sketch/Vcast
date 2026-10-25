package com.vcast.sender

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.vcast.sender.net.Discovery
import com.vcast.sender.net.Receiver
import com.vcast.sender.protocol.Protocol

class MainActivity : Activity() {
    private lateinit var prefs: SharedPreferences
    private lateinit var statusText: TextView
    private lateinit var stopButton: Button
    private lateinit var qualityGroup: RadioGroup
    private lateinit var audioSwitch: Switch
    private lateinit var receiversList: LinearLayout
    private lateinit var searchingText: TextView
    private lateinit var manualAddress: EditText
    private lateinit var discovery: Discovery

    private var receivers: List<Receiver> = emptyList()
    private var pendingTarget: Receiver? = null
    private var castActive = false

    private val statusListener: (CastState.Status) -> Unit = { showStatus(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = getSharedPreferences("vcast", Context.MODE_PRIVATE)

        statusText = view(R.id.status)
        stopButton = view(R.id.stop)
        qualityGroup = view(R.id.quality)
        audioSwitch = view(R.id.audio)
        receiversList = view(R.id.receivers)
        searchingText = view(R.id.searching)
        manualAddress = view(R.id.manual_address)
        val audioNote = view<TextView>(R.id.audio_note)

        qualityGroup.check(if (prefs.getInt(PREF_QUALITY, 1080) == 720) R.id.quality_720 else R.id.quality_1080)
        qualityGroup.setOnCheckedChangeListener { _, id ->
            prefs.edit().putInt(PREF_QUALITY, if (id == R.id.quality_720) 720 else 1080).apply()
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            audioSwitch.isChecked = prefs.getBoolean(PREF_AUDIO, true)
            audioSwitch.setOnCheckedChangeListener { _, on -> prefs.edit().putBoolean(PREF_AUDIO, on).apply() }
            audioNote.setText(R.string.audio_note)
        } else {
            audioSwitch.isChecked = false
            audioSwitch.isEnabled = false
            audioNote.setText(R.string.audio_note_old)
        }

        manualAddress.setText(prefs.getString(PREF_MANUAL, ""))
        view<Button>(R.id.manual_cast).setOnClickListener { castToManualAddress() }
        manualAddress.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_GO) {
                castToManualAddress()
                true
            } else {
                false
            }
        }
        stopButton.setOnClickListener { CastService.stop(this) }

        discovery = Discovery(this) { list ->
            receivers = list
            renderReceivers()
        }
        savedInstanceState?.getStringArray(STATE_TARGET)?.let {
            if (it.size == 3) pendingTarget = Receiver(it[0], it[1], it[2].toIntOrNull() ?: Protocol.DEFAULT_STREAM_PORT)
        }
        renderReceivers()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        pendingTarget?.let { outState.putStringArray(STATE_TARGET, arrayOf(it.name, it.address, it.port.toString())) }
    }

    override fun onStart() {
        super.onStart()
        CastState.addListener(statusListener)
        discovery.start()
    }

    override fun onStop() {
        discovery.stop()
        CastState.removeListener(statusListener)
        super.onStop()
    }

    private fun renderReceivers() {
        receiversList.removeAllViews()
        searchingText.visibility = if (receivers.isEmpty()) View.VISIBLE else View.GONE
        val inflater = LayoutInflater.from(this)
        for (r in receivers) {
            val row = inflater.inflate(R.layout.item_receiver, receiversList, false)
            row.findViewById<TextView>(R.id.name)!!.text = r.name
            row.findViewById<TextView>(R.id.address)!!.text = r.key
            row.findViewById<Button>(R.id.cast)!!.apply {
                isEnabled = !castActive
                setOnClickListener { requestCast(r) }
            }
            receiversList.addView(row)
        }
    }

    private fun castToManualAddress() {
        val text = manualAddress.text.toString()
        val hp = Protocol.parseHostPort(text)
        if (hp == null) {
            manualAddress.error = getString(R.string.invalid_address)
            return
        }
        prefs.edit().putString(PREF_MANUAL, text.trim()).apply()
        requestCast(Receiver(hp.host, hp.host, hp.port))
    }

    private fun showStatus(s: CastState.Status) {
        castActive = s.isActive
        statusText.text = when (s) {
            is CastState.Status.Idle -> s.message ?: getString(R.string.status_idle)
            is CastState.Status.Connecting -> getString(R.string.status_connecting, s.target)
            is CastState.Status.Casting ->
                getString(R.string.status_casting, s.receiver) + if (s.detail.isNotEmpty()) "\n" + s.detail else ""
            is CastState.Status.Error -> s.message
        }
        stopButton.isEnabled = castActive
        view<Button>(R.id.manual_cast).isEnabled = !castActive
        renderReceivers()
    }

    // ------------------------------------------------------------------ cast flow

    /** Step 1: runtime permissions (notifications on 13+, RECORD_AUDIO for audio). */
    private fun requestCast(target: Receiver) {
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
            .putExtra(CastService.EXTRA_MAX_LONG_SIDE, if (qualityGroup.checkedRadioButtonId == R.id.quality_720) 1280 else 1920)
            .putExtra(CastService.EXTRA_AUDIO, audio)
        CastState.post(CastState.Status.Connecting(target.name))
        startForegroundService(intent)
    }

    private fun <T : View> view(id: Int): T = findViewById<T>(id)!!

    private fun wantsAudio() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && audioSwitch.isChecked

    companion object {
        private const val REQ_PERMISSIONS = 1
        private const val REQ_CAPTURE = 2
        private const val PREF_QUALITY = "quality"
        private const val PREF_AUDIO = "audio"
        private const val PREF_MANUAL = "manual"
        private const val STATE_TARGET = "pendingTarget"
    }
}
