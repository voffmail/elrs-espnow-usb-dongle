package com.elrs.dongle.stub

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.SystemClock
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.SocketAddress

/**
 * Sends complete CRSF frames to the original viewer/app over UDP.
 *
 * An empty target host means broadcast; otherwise the host is unicast.
 * "localhost" is sent over loopback without binding to Wi-Fi. The socket is
 * bound to the Wi-Fi network for LAN targets.
 */
class NetOut(context: Context) {

    companion object {
        const val DEFAULT_PORT = 14550
        const val DEFAULT_TARGET_HOST = "localhost"
        private const val PREFS = "dongle"
        private const val PREF_TARGET_HOST = "target_host"
        private const val PREF_TARGET_PORT = "target_port"
        private const val REFRESH_MS = 2000L
    }

    private val appContext = context.applicationContext
    private val lock = Any()

    private var rawSocket: DatagramSocket? = null
    private var destination: InetAddress? = null
    private var networkKey = 0
    private var processBoundNetwork: Network? = null
    private var targetHost = ""
    private var targetPort = DEFAULT_PORT
    private var lastRefresh = 0L

    @Volatile var target: String = "no Wi-Fi"
        private set

    @Volatile var lastError: String? = null
        private set

    @Volatile var rawPackets = 0L
        private set

    fun reload() {
        synchronized(lock) {
            lastRefresh = 0L
            closeSocketLocked()
        }
    }

    fun sendRaw(data: ByteArray) {
        synchronized(lock) {
            refreshLocked()
            val addr = destination ?: return
            val socket = rawSocket ?: return
            try {
                socket.send(DatagramPacket(data, data.size, addr, targetPort))
                rawPackets++
                lastError = null
            } catch (e: IOException) {
                lastError = e.message
            }
        }
    }

    private fun refreshLocked() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastRefresh < REFRESH_MS) return
        lastRefresh = now

        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val host = prefs.getString(PREF_TARGET_HOST, DEFAULT_TARGET_HOST)
            .orEmpty().trim()
        val port = prefs.getInt(PREF_TARGET_PORT, DEFAULT_PORT)
        val settingsChanged = host != targetHost || port != targetPort
        if (settingsChanged) {
            closeSocketLocked()
            targetHost = host
            targetPort = port
        }

        // localhost means a viewer on this phone. Do not bind this socket to
        // Wi-Fi; loopback is not part of the Wi-Fi network.
        if (isLoopbackHost(targetHost)) {
            val key = -1
            if (networkKey != key || rawSocket == null) {
                closeSocketLocked()
                val loopback = InetAddress.getByName("127.0.0.1")
                destination = loopback
                target = "local ${loopback.hostAddress}:$targetPort"
                try {
                    rawSocket = DatagramSocket().apply { broadcast = false }
                    networkKey = key
                    lastError = null
                } catch (e: IOException) {
                    lastError = "local socket: ${e.message}"
                    closeSocketLocked()
                }
            }
            return
        }

        val cm = appContext.getSystemService(ConnectivityManager::class.java)
        if (cm == null) {
            target = "no ConnectivityManager"
            return
        }

        var wifi: Network? = null
        var ip: Inet4Address? = null
        var prefix = 0

        for (n in cm.allNetworks) {
            val caps = cm.getNetworkCapabilities(n) ?: continue
            // A VPN can report TRANSPORT_WIFI too (its underlying network).
            // Bind to the real Wi-Fi so LAN unicast/broadcast does not go
            // into the tunnel.
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue
            if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
            val lp = cm.getLinkProperties(n) ?: continue
            val link = lp.linkAddresses.firstOrNull { it.address is Inet4Address } ?: continue
            wifi = n
            ip = link.address as Inet4Address
            prefix = link.prefixLength
            break
        }

        if (wifi == null || ip == null) {
            closeSocketLocked()
            destination = null
            target = "no Wi-Fi"
            return
        }

        val wifiNetwork = wifi ?: return
        val ipAddr = ip ?: return

        val key = wifiNetwork.hashCode()

        if (key != networkKey || rawSocket == null) {
            closeSocketLocked()
            val b = try {
                if (targetHost.isEmpty()) broadcastAddress(ipAddr, prefix)
                else unicastOrBroadcast(ipAddr, prefix)
            } catch (e: IOException) {
                lastError = "target: ${e.message}"
                destination = null
                target = "invalid target"
                return
            }
            destination = b
            target = if (targetHost.isEmpty()) {
                "broadcast ${b.hostAddress}:$targetPort"
            } else {
                "unicast ${b.hostAddress}:$targetPort"
            }
            // Prefer binding the process to Wi-Fi: some OEM builds (MIUI,
            // HyperOS) reject Network.bindSocket() on an app socket with
            // EPERM. If process binding also fails, fall back to a plain
            // socket, which still works when Wi-Fi is the default network.
            val processBound = try {
                cm.bindProcessToNetwork(wifiNetwork)
            } catch (e: Exception) {
                false
            }
            if (processBound) {
                processBoundNetwork = wifiNetwork
                lastError = null
            }

            // Use an unbound socket: some Android builds reject
            // Network.bindSocket() on an already bound DatagramSocket.
            val socket = DatagramSocket(null as SocketAddress?)
            socket.broadcast = true
            if (!processBound) {
                try {
                    wifiNetwork.bindSocket(socket)
                    lastError = null
                } catch (e: IOException) {
                    lastError = "bind: ${e.message} (default net)"
                }
            }
            rawSocket = socket
            networkKey = key
        }
    }

    private fun isLoopbackHost(host: String): Boolean {
        val h = host.trim().lowercase()
        return h == "localhost" || h == "127.0.0.1" || h == "::1"
    }

    private fun unicastOrBroadcast(ip: Inet4Address, prefix: Int): InetAddress {
        if (targetHost.isNotEmpty()) {
            val address = InetAddress.getByName(targetHost)
            if (address is Inet4Address) return address
            throw IOException("not an IPv4 address")
        }
        return broadcastAddress(ip, prefix)
    }

    private fun broadcastAddress(ip: Inet4Address, prefix: Int): InetAddress {
        val bytes = ip.address
        val value = ((bytes[0].toInt() and 0xFF) shl 24) or
                ((bytes[1].toInt() and 0xFF) shl 16) or
                ((bytes[2].toInt() and 0xFF) shl 8) or
                (bytes[3].toInt() and 0xFF)
        val mask = if (prefix == 0) 0 else (-1 shl (32 - prefix))
        val b = value or mask.inv()
        return InetAddress.getByAddress(
            byteArrayOf(
                ((b ushr 24) and 0xFF).toByte(),
                ((b ushr 16) and 0xFF).toByte(),
                ((b ushr 8) and 0xFF).toByte(),
                (b and 0xFF).toByte(),
            )
        )
    }

    private fun closeSocketLocked() {
        rawSocket?.close()
        rawSocket = null
        destination = null
        networkKey = 0
        if (processBoundNetwork != null) {
            try {
                appContext.getSystemService(ConnectivityManager::class.java)
                    ?.bindProcessToNetwork(null)
            } catch (_: Exception) {
            }
            processBoundNetwork = null
        }
    }

    fun close() {
        synchronized(lock) {
            closeSocketLocked()
        }
    }

    fun status(): String {
        val err = lastError
        return if (err != null) "$target ($err)" else target
    }
}
