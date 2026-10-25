package com.vcast.sender

import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

/** "Cast to OBS" Quick Settings tile: tap to cast to the last used receiver, tap again to stop. */
class CastTileService : TileService() {
    private val listener: (CastState.Status) -> Unit = { updateTile(it) }

    override fun onStartListening() {
        super.onStartListening()
        CastState.addListener(listener) // also calls updateTile with the current status
    }

    override fun onStopListening() {
        CastState.removeListener(listener)
        super.onStopListening()
    }

    override fun onClick() {
        super.onClick()
        if (CastState.status.isActive) {
            CastService.stop(this)
            return
        }
        if (isLocked) {
            unlockAndRun { launchQuickCast() }
        } else {
            launchQuickCast()
        }
    }

    private fun launchQuickCast() {
        val intent = Intent(this, QuickCastActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val pi = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            startActivityAndCollapse(pi)
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(intent)
        }
    }

    private fun updateTile(s: CastState.Status) {
        val tile = qsTile ?: return
        tile.label = getString(R.string.tile_label)
        tile.icon = Icon.createWithResource(this, R.drawable.ic_stat_cast)
        tile.state = if (s.isActive) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            tile.subtitle = when (s) {
                is CastState.Status.Casting -> s.receiver
                is CastState.Status.Connecting -> getString(R.string.tile_connecting)
                else -> Prefs.lastReceiver(this)?.name
            }
        }
        tile.updateTile()
    }

    companion object {
        /** Asks the system to refresh the tile (e.g. after casting started or stopped). */
        fun refresh(context: Context) {
            try {
                requestListeningState(context, ComponentName(context, CastTileService::class.java))
            } catch (e: Exception) {
                // the tile is not added; nothing to refresh
            }
        }
    }
}
