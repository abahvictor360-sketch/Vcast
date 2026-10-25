package com.vcast.sender

import android.content.Context
import android.content.SharedPreferences
import com.vcast.sender.net.Receiver

/** Settings shared by the app screen, the quick-cast activity and the tile. */
object Prefs {
    private const val FILE = "vcast"
    private const val QUALITY = "quality"
    private const val AUDIO = "audio"
    private const val MANUAL = "manual"
    private const val LAST_NAME = "lastName"
    private const val LAST_ADDRESS = "lastAddress"
    private const val LAST_PORT = "lastPort"

    private fun p(c: Context): SharedPreferences = c.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    /** 720 or 1080. */
    fun quality(c: Context): Int = if (p(c).getInt(QUALITY, 1080) == 720) 720 else 1080
    fun setQuality(c: Context, q: Int) = p(c).edit().putInt(QUALITY, q).apply()
    fun maxLongSide(c: Context): Int = if (quality(c) == 720) 1280 else 1920

    fun audio(c: Context): Boolean = p(c).getBoolean(AUDIO, true)
    fun setAudio(c: Context, on: Boolean) = p(c).edit().putBoolean(AUDIO, on).apply()

    fun manualAddress(c: Context): String = p(c).getString(MANUAL, "") ?: ""
    fun setManualAddress(c: Context, s: String) = p(c).edit().putString(MANUAL, s).apply()

    /** The receiver the user cast to most recently, if any. */
    fun lastReceiver(c: Context): Receiver? {
        val prefs = p(c)
        val address = prefs.getString(LAST_ADDRESS, null) ?: return null
        val name = prefs.getString(LAST_NAME, null) ?: address
        val port = prefs.getInt(LAST_PORT, 0).takeIf { it in 1..65535 } ?: return null
        return Receiver(name, address, port)
    }

    fun setLastReceiver(c: Context, r: Receiver) {
        p(c).edit().putString(LAST_NAME, r.name).putString(LAST_ADDRESS, r.address).putInt(LAST_PORT, r.port).apply()
    }

    /** True if [r] looks like the last receiver (same address:port, or same name after a DHCP change). */
    fun isLast(c: Context, r: Receiver): Boolean {
        val last = lastReceiver(c) ?: return false
        return r.key == last.key || (r.name == last.name && last.name != last.address)
    }
}
