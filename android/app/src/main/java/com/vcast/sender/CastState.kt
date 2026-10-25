package com.vcast.sender

import android.os.Handler
import android.os.Looper

/** Casting status shared between [CastService] and [MainActivity] (same process). */
object CastState {
    sealed class Status {
        data class Idle(val message: String? = null) : Status()
        data class Connecting(val target: String) : Status()
        data class Casting(val receiver: String, val detail: String) : Status()
        data class Error(val message: String) : Status()

        val isActive: Boolean get() = this is Connecting || this is Casting
    }

    private val main = Handler(Looper.getMainLooper())
    private val listeners = LinkedHashSet<(Status) -> Unit>()

    /** Current status; read on the main thread. */
    var status: Status = Status.Idle()
        private set

    /** Updates the status from any thread; listeners run on the main thread. */
    fun post(s: Status) {
        if (Looper.myLooper() == Looper.getMainLooper()) set(s) else main.post { set(s) }
    }

    private fun set(s: Status) {
        status = s
        for (l in listeners.toList()) l(s)
    }

    fun addListener(l: (Status) -> Unit) {
        listeners.add(l)
        l(status)
    }

    fun removeListener(l: (Status) -> Unit) {
        listeners.remove(l)
    }
}
