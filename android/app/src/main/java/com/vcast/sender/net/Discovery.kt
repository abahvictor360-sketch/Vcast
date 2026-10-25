package com.vcast.sender.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.vcast.sender.protocol.Protocol
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/** A receiver found on the network. */
data class Receiver(val name: String, val address: String, val port: Int) {
    val key: String get() = "$address:$port"
}

/**
 * Finds Vcast receivers: broadcasts `VCAST?\n` to UDP 48689 every [INTERVAL_MS] and collects
 * the `VCAST!\n{json}` answers. Results are delivered on the main thread.
 */
class Discovery(context: Context, private val onUpdate: (List<Receiver>) -> Unit) {
    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private var worker: Worker? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private class Worker {
        @Volatile var running = true
        @Volatile var socket: DatagramSocket? = null
    }

    fun start() {
        if (worker != null) return
        val w = Worker()
        worker = w
        val wifi = appContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        multicastLock = wifi?.createMulticastLock("vcast-discovery")?.apply {
            setReferenceCounted(false)
            try {
                acquire()
            } catch (e: RuntimeException) {
                Log.w(TAG, "multicast lock", e)
            }
        }
        Thread({ run(w) }, "vcast-discovery").apply {
            isDaemon = true
            start()
        }
    }

    fun stop() {
        val w = worker ?: return
        worker = null
        w.running = false
        w.socket?.close()
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }

    private fun run(w: Worker) {
        val found = LinkedHashMap<String, Pair<Receiver, Long>>()
        var lastPublished: List<Receiver>? = null
        try {
            DatagramSocket().use { s ->
                w.socket = s
                if (!w.running) return
                s.broadcast = true
                s.soTimeout = 250
                val buf = ByteArray(2048)
                while (w.running) {
                    sendQueries(s)
                    val roundEnd = System.currentTimeMillis() + INTERVAL_MS
                    while (w.running && System.currentTimeMillis() < roundEnd) {
                        val p = DatagramPacket(buf, buf.size)
                        try {
                            s.receive(p)
                        } catch (e: SocketTimeoutException) {
                            continue
                        }
                        val reply = Protocol.parseDiscoveryReply(p.data, p.length) ?: continue
                        val addr = p.address.hostAddress ?: continue
                        val r = Receiver(reply.name, addr, reply.port)
                        found[r.key] = r to System.currentTimeMillis()
                        lastPublished = publish(w, found, lastPublished)
                    }
                    val cutoff = System.currentTimeMillis() - EXPIRE_MS
                    found.entries.removeAll { it.value.second < cutoff }
                    lastPublished = publish(w, found, lastPublished)
                }
            }
        } catch (e: Exception) {
            if (w.running) Log.w(TAG, "discovery stopped", e)
        }
    }

    private fun publish(w: Worker, found: Map<String, Pair<Receiver, Long>>, last: List<Receiver>?): List<Receiver> {
        val list = found.values.map { it.first }.sortedBy { it.name.lowercase() + it.key }
        if (list != last) main.post { if (w.running) onUpdate(list) }
        return list
    }

    private fun sendQueries(s: DatagramSocket) {
        val targets = LinkedHashSet<InetAddress>()
        targets.add(InetAddress.getByName("255.255.255.255"))
        targets.addAll(directedBroadcasts())
        for (t in targets) {
            try {
                val q = Protocol.DISCOVERY_QUERY
                s.send(DatagramPacket(q, q.size, t, Protocol.DISCOVERY_PORT))
            } catch (e: Exception) {
                // e.g. ENETUNREACH on an interface that just went away
                Log.d(TAG, "query to $t failed: ${e.message}")
            }
        }
    }

    /** Directed broadcast addresses of the Wi-Fi / Ethernet networks (and hotspot). */
    private fun directedBroadcasts(): Set<InetAddress> {
        val result = LinkedHashSet<InetAddress>()
        val cm = appContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        if (cm != null) {
            try {
                @Suppress("DEPRECATION")
                val networks = cm.allNetworks.toMutableList()
                cm.activeNetwork?.let { if (it !in networks) networks.add(0, it) }
                for (n in networks) {
                    val caps = cm.getNetworkCapabilities(n) ?: continue
                    if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) &&
                        !caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
                    ) {
                        continue
                    }
                    val lp = cm.getLinkProperties(n) ?: continue
                    for (la in lp.linkAddresses) {
                        val a = la.address as? Inet4Address ?: continue
                        broadcastOf(a, la.prefixLength)?.let { result.add(it) }
                    }
                }
            } catch (e: Exception) {
                Log.d(TAG, "link properties", e)
            }
        }
        // Also cover interfaces that are not "networks", e.g. the phone's own hotspot.
        try {
            for (ni in NetworkInterface.getNetworkInterfaces() ?: return result) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    if (ia.address is Inet4Address) ia.broadcast?.let { result.add(it) }
                }
            }
        } catch (e: Exception) {
            Log.d(TAG, "interfaces", e)
        }
        return result
    }

    companion object {
        private const val TAG = "VcastDiscovery"
        const val INTERVAL_MS = 2000L
        const val EXPIRE_MS = 7000L

        fun broadcastOf(a: Inet4Address, prefix: Int): InetAddress? {
            if (prefix !in 1..30) return null
            val ip = Protocol.getInt(a.address, 0)
            val mask = -1 shl (32 - prefix)
            val b = ByteArray(4)
            Protocol.putInt(b, 0, (ip and mask) or mask.inv())
            return InetAddress.getByAddress(b)
        }
    }
}
