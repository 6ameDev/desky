package com.desky.shared.core

import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.UdpSocketBinder
import com.desky.shared.core.UdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketException
import java.net.SocketTimeoutException
import android.net.Network
import android.util.Log
import kotlin.concurrent.Volatile

/** Log tag for socket-creation/binding failures (key material is never logged). */
private const val TAG = "DatagramSocketManager"

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
 *
 * Startup robustness: socket construction can throw `SocketException`, and
 * this manager is built inside the Koin singleton graph on the app startup
 * path — so the socket is nullable with a logged fallback instead of a
 * throwing initializer. Null surfaces a closed/broken manager: the receive
 * loop exits immediately and [send] fails fast with `IOException` (reported
 * to the caller, never a startup crash).
 *
 * Ownership: app scope (`DeskyApp` Koin singleton), NOT any ViewModel.
 * ViewModels (e.g. `RobotControlViewModel`) hold the manager for API
 * symmetry but must never close it — closing the shared singleton would
 * break every other screen. Close/detach live and die with the process
 * (there is no `DeskyApp.onTerminate` wiring: it never runs on real
 * devices, so KDoc-only by design — no new lifecycle dependency).
 */
actual fun createUdpSocketManager(scope: CoroutineScope): UdpSocketManager =
    DatagramSocketManager(scope)

internal class DatagramSocketManager(
    private val scope: CoroutineScope,
) : UdpSocketManager {
    private val socket: DatagramSocket? = try {
        DatagramSocket().apply {
            broadcast = true
            soTimeout = RECEIVE_TIMEOUT_MS
        }
    } catch (e: SocketException) {
        Log.w(TAG, "UDP socket creation failed; manager unavailable", e)
        null
    }

    private var wifiBinder: UdpSocketBinder? = null

    /**
     * Resolved peer addresses by host. `InetAddress.getByName` on
     * `desky.local` hits mDNS per call — on a 50 Hz send loop that stacks a
     * blocking lookup behind every radio stall, so resolve once per host and
     * evict the entry only on send failure (roam/rekey), letting the next
     * send re-resolve. Guarded by [addressMutex]; sends never hold it
     * across the socket. Lifecycle signals ([bindToWifiNetwork], unpair,
     * background) clear the map the same way so a silent roam with no send
     * error still re-resolves.
     */
    private val addressMutex = Mutex()
    private val addressCache = mutableMapOf<String, InetAddress>()

    /**
     * Close guard: [close] is idempotent — safe to call twice or racing a
     * `bindToWifiNetwork` (which treats a closed/null socket as no-op).
     * Volatile, not locked: every step inside is itself idempotent
     * (`Job.cancel`, `DatagramSocket.close`, `UdpSocketBinder.detach`).
     */
    @Volatile
    private var closed = false

    private val _incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = _incoming.asSharedFlow()

    /**
     * Datagrams dropped because the 64-slot buffer was full. Diagnostic only:
     * a drop means the consumer is behind, never a socket error.
     */
    @Volatile
    var droppedIncoming = 0
        private set

    private val receiveJob: Job

    init {
        receiveJob = scope.launch(Dispatchers.IO) {
            // Null socket (creation failed) -> broken manager: exit, no loop.
            val s = socket ?: return@launch
            val buf = ByteArray(MAX_DATAGRAM_BYTES)
            while (isActive) {
                val packet = DatagramPacket(buf, buf.size)
                try {
                    s.receive(packet)
                } catch (e: SocketTimeoutException) {
                    continue
                } catch (e: SocketException) {
                    // Closed locally -> exit; transient error -> keep listening.
                    if (s.isClosed) break else continue
                } catch (e: IOException) {
                    if (!isActive || s.isClosed) break else continue
                }
                val host = packet.address?.hostAddress ?: continue
                // tryEmit (never emit): a full buffer must drop, not suspend
                // the receiver and stall pairing/telemetry reads.
                if (!_incoming.tryEmit(UdpDatagram(packet.data.copyOf(packet.length), host, packet.port))) {
                    droppedIncoming++
                }
            }
        }
    }

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        withContext(Dispatchers.IO) {
            val s = socket ?: throw IOException("UDP socket unavailable (creation failed)")
            val address = resolveHost(host)
            try {
                s.send(DatagramPacket(bytes, bytes.size, address, port))
            } catch (e: IOException) {
                // Stale mapping (roam/rekey): drop only our entry so the next
                // send re-resolves; a concurrent winner is left alone.
                addressMutex.withLock { addressCache.remove(host, address) }
                throw e
            }
        }
    }

    override suspend fun evictAll() {
        addressMutex.withLock { addressCache.clear() }
    }

    /** Best-effort clear for the non-suspending network callback; skips when contended. */
    internal fun evictAllNow() {
        if (addressMutex.tryLock()) {
            try {
                addressCache.clear()
            } finally {
                addressMutex.unlock()
            }
        }
    }

    private suspend fun resolveHost(host: String): InetAddress {
        addressMutex.withLock { addressCache[host]?.let { return it } }
        val resolved = InetAddress.getByName(host)
        addressMutex.withLock { addressCache[host] = resolved }
        return resolved
    }

    /**
     * Pin [socket] to [network] (per-socket binding — no process-wide route
     * change). Called by [WifiSocketBinder] on every `onAvailable`; safe to
     * call repeatedly (re-points the fd) and safe after [close] (ignored).
     *
     * @return true when the fd was re-pointed; false when there is nothing to
     *   bind (creation failed / already closed) or the bind threw.
     */
    internal fun bindToWifiNetwork(network: Network): Boolean {
        return try {
            val s = socket ?: return false
            network.bindSocket(s)
            evictAllNow()
            true
        } catch (_: Exception) {
            // Socket already closed (close() won the race) — nothing to bind.
            false
        }
    }

    /** Remember [binder] so [close] can unregister its network callback. */
    internal fun setWifiBinder(binder: UdpSocketBinder?) {
        wifiBinder = binder
    }

    /**
     * Idempotent + safe double-call: second (or concurrent) calls no-op.
     * App-scope only — never call from a ViewModel (see class KDoc).
     */
    override suspend fun close() {
        if (closed) return
        closed = true
        receiveJob.cancel()
        // DatagramSocket.close() is itself idempotent; null = creation failed.
        socket?.close()
        // detach() is idempotent (safe when never attached); null afterwards
        // so a repeated close cannot re-detach.
        wifiBinder?.detach()
        wifiBinder = null
    }
}
