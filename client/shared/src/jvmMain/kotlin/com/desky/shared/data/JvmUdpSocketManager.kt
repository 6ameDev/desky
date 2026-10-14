package com.desky.shared.data

import com.desky.shared.domain.UdpDatagram
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

/** Wake interval so a blocked `receive` notices cancellation/close. */
private const val RECEIVE_TIMEOUT_MS = 500

/** Receive buffer; protocol frames are 6 (control) / 9 (telemetry) bytes. */
private const val MAX_DATAGRAM_BYTES = 64

/**
 * JVM [UdpSocketManager]: `DatagramSocket` bound to an ephemeral port in the
 * constructor (broadcast enabled, 500 ms `soTimeout` for an interruptible
 * receive loop), loop launched in `init` into [scope] on `Dispatchers.IO`
 * feeding a no-replay flow with a 64-slot buffer.
 *
 * Identical behavior to the androidMain manager (the two cannot share code:
 * neither source set sees the other). exercised by `:shared:jvmTest` only
 * indirectly — tests use a fake — but kept fully functional for desktop use.
 */
actual fun createUdpSocketManager(scope: CoroutineScope): UdpSocketManager =
    JvmDatagramSocketManager(scope)

internal class JvmDatagramSocketManager(
    private val scope: CoroutineScope,
) : UdpSocketManager {
    private val socket = DatagramSocket().apply {
        broadcast = true
        soTimeout = RECEIVE_TIMEOUT_MS
    }

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

    override suspend fun close() {
        receiveJob.cancel()
        socket.close()
    }
}
