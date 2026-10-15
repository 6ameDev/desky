package com.desky.shared.data

import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.UdpSocketBinder
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketException
import java.net.SocketTimeoutException
import android.net.Network

/** Wake interval so a blocked `receive` notices cancellation/close. */
private const val RECEIVE_TIMEOUT_MS = 500

/** Receive buffer; protocol frames are 6 (control) / 9 (telemetry) bytes. */
private const val MAX_DATAGRAM_BYTES = 64

/**
 * Android [UdpSocketManager]: `DatagramSocket` bound to an ephemeral port in
 * the constructor (broadcast enabled, 500 ms `soTimeout` for an
 * interruptible receive loop), loop launched in `init` into [scope] on
 * `Dispatchers.IO` feeding a no-replay flow with a 64-slot buffer.
 *
 * Wi-Fi pinning: [WifiSocketBinder.attach] (wired via `dataModule`) calls
 * [bindToWifiNetwork] on every `onAvailable`, which re-points this same fd at
 * the Wi-Fi network — rebind-only, so the ephemeral port never churns. See
 * `WifiSocketBinder` KDoc for the mobile-data root cause.
 */
actual fun createUdpSocketManager(scope: CoroutineScope): UdpSocketManager =
    DatagramSocketManager(scope)

internal class DatagramSocketManager(
    private val scope: CoroutineScope,
) : UdpSocketManager {
    private val socket = DatagramSocket().apply {
        broadcast = true
        soTimeout = RECEIVE_TIMEOUT_MS
    }

    private var wifiBinder: UdpSocketBinder? = null

    private val _incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = _incoming.asSharedFlow()

    private val receiveJob: Job

    init {
        receiveJob = scope.launch(Dispatchers.IO) {
            val buf = ByteArray(MAX_DATAGRAM_BYTES)
            while (isActive) {
                val packet = DatagramPacket(buf, buf.size)
                try {
                    socket.receive(packet)
                } catch (e: SocketTimeoutException) {
                    continue
                } catch (e: SocketException) {
                    // Closed locally -> exit; transient error -> keep listening.
                    if (socket.isClosed) break else continue
                } catch (e: IOException) {
                    if (!isActive || socket.isClosed) break else continue
                }
                val host = packet.address?.hostAddress ?: continue
                _incoming.emit(UdpDatagram(packet.data.copyOf(packet.length), host, packet.port))
            }
        }
    }

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        withContext(Dispatchers.IO) {
            val address = InetAddress.getByName(host)
            socket.send(DatagramPacket(bytes, bytes.size, address, port))
        }
    }

    /**
     * Pin [socket] to [network] (per-socket binding — no process-wide route
     * change). Called by [WifiSocketBinder] on every `onAvailable`; safe to
     * call repeatedly (re-points the fd) and safe after [close] (ignored).
     */
    internal fun bindToWifiNetwork(network: Network) {
        try {
            network.bindSocket(socket)
        } catch (_: Exception) {
            // Socket already closed (close() won the race) — nothing to bind.
        }
    }

    /** Remember [binder] so [close] can unregister its network callback. */
    internal fun setWifiBinder(binder: UdpSocketBinder?) {
        wifiBinder = binder
    }

    override suspend fun close() {
        receiveJob.cancel()
        socket.close()
        wifiBinder?.detach()
        wifiBinder = null
    }
}
