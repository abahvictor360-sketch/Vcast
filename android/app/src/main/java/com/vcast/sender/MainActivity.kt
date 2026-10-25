package com.vcast.sender

import android.annotation.TargetApi
import android.app.StatusBarManager
import android.content.ComponentName
import android.graphics.drawable.Icon
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

class MainActivity : CastFlowActivity() {
    private lateinit var statusText: TextView
    private lateinit var stopButton: Button
    private lateinit var quickCastButton: Button
    private lateinit var qualityGroup: RadioGroup
    private lateinit var audioSwitch: Switch
    private lateinit var receiversList: LinearLayout
    private lateinit var searchingText: TextView
    private lateinit var manualAddress: EditText
    private lateinit var discovery: Discovery

    private var receivers: List<Receiver> = emptyList()
    private var castActive = false

    private val statusListener: (CastState.Status) -> Unit = { showStatus(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = view(R.id.status)
        stopButton = view(R.id.stop)
        quickCastButton = view(R.id.quick_cast)
        qualityGroup = view(R.id.quality)
        audioSwitch = view(R.id.audio)
        receiversList = view(R.id.receivers)
        searchingText = view(R.id.searching)
        manualAddress = view(R.id.manual_address)
        val audioNote = view<TextView>(R.id.audio_note)

        qualityGroup.check(if (Prefs.quality(this) == 720) R.id.quality_720 else R.id.quality_1080)
        qualityGroup.setOnCheckedChangeListener { _, id ->
            Prefs.setQuality(this, if (id == R.id.quality_720) 720 else 1080)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            audioSwitch.isChecked = Prefs.audio(this)
            audioSwitch.setOnCheckedChangeListener { _, on -> Prefs.setAudio(this, on) }
            audioNote.setText(R.string.audio_note)
        } else {
            audioSwitch.isChecked = false
            audioSwitch.isEnabled = false
            audioNote.setText(R.string.audio_note_old)
        }

        manualAddress.setText(Prefs.manualAddress(this))
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

        val addTile = view<Button>(R.id.add_tile)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            addTile.visibility = View.VISIBLE
            addTile.setOnClickListener { requestAddTile() }
        } else {
            addTile.visibility = View.GONE
        }

        discovery = Discovery(this) { list ->
            receivers = list
            renderReceivers()
        }
        renderReceivers()
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
        // One-tap cast: exactly one receiver on the network and it is the one used last time.
        val only = receivers.singleOrNull()
        if (!castActive && only != null && Prefs.isLast(this, only)) {
            quickCastButton.text = getString(R.string.cast_to, only.name)
            quickCastButton.setOnClickListener { requestCast(only) }
            quickCastButton.visibility = View.VISIBLE
        } else {
            quickCastButton.visibility = View.GONE
        }
    }

    private fun castToManualAddress() {
        val text = manualAddress.text.toString()
        val hp = Protocol.parseHostPort(text)
        if (hp == null) {
            manualAddress.error = getString(R.string.invalid_address)
            return
        }
        Prefs.setManualAddress(this, text.trim())
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

    @TargetApi(Build.VERSION_CODES.TIRAMISU)
    private fun requestAddTile() {
        val sbm = getSystemService(StatusBarManager::class.java) ?: return
        sbm.requestAddTileService(
            ComponentName(this, CastTileService::class.java),
            getString(R.string.tile_label),
            Icon.createWithResource(this, R.drawable.ic_stat_cast),
            mainExecutor,
        ) { result ->
            val msg = when (result) {
                StatusBarManager.TILE_ADD_REQUEST_RESULT_TILE_ADDED -> R.string.tile_added
                StatusBarManager.TILE_ADD_REQUEST_RESULT_TILE_ALREADY_ADDED -> R.string.tile_already_added
                StatusBarManager.TILE_ADD_REQUEST_RESULT_TILE_NOT_ADDED -> null
                else -> R.string.tile_add_manually
            }
            if (msg != null) Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
        }
    }

    private fun <T : View> view(id: Int): T = findViewById<T>(id)!!
}
