@file:OptIn(ExperimentalForeignApi::class)

package com.desky.shared.data

import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.UdpSocketManager
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.cValuesOf
import kotlinx.cinterop.convert
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.sizeOf
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import platform.posix.AF_INET
import platform.posix.INADDR_ANY
import platform.posix.IPPROTO_UDP
import platform.posix.SOCK_DGRAM
import platform.posix.SOL_SOCKET
import platform.posix.SO_BROADCAST
import platform.posix.SO_RCVTIMEO
import platform.posix.bind
import platform.posix.close
import platform.posix.memset
import platform.posix.recvfrom
import platform.posix.sendto
import platform.posix.setsockopt
import platform.posix.sockaddr
import platform.posix.sockaddr_in
import platform.posix.socklen_tVar
import platform.posix.socket
import platform.posix.timeval
import kotlin.concurrent.Volatile

/**
 * iOS [UdpSocketManager] over POSIX UDP (`platform.posix` only).
 *
 * Deliberately conservative interop (bisected October 2026, Kotlin 2.4.20):
 * - no `inet_pton`/`inet_ntop` (absent from this toolchain's
 *   `platform.posix`): IPv4 literal <-> network-order conversion is the
 *   pure-Kotlin, unit-tested code in common `Ipv4.kt` (little-endian
 *   network-order semantics — see the KDoc there before touching it);
 *   only literals are supported (discovery broadcast + discovered device
 *   IPs — no DNS in v1 scope);
 * - no `htons`/`ntohs`: port byte order is swapped manually;
 * - no `errno`/`strerror`: failures carry plain messages naming the call;
 * - otherwise canonical patterns only: `alloc`'d structs with field access,
 *   `cValuesOf` for the read-only setsockopt flag, `usePinned` +
 *   `addressOf` for the byte buffers. (Note for future readers: cinterop
 *   extension members such as `Var.value`, `CPointer[]`, and `addressOf`
 *   need explicit imports — `import kotlinx.cinterop.value` etc. Bare
 *   `import kotlinx.cinterop.X` for the type alone is not enough.)
 *
 * The receive loop wakes every [RECEIVE_TIMEOUT_US] via `SO_RCVTIMEO`, so
 * [close] never waits on a blocked `recvfrom` longer than that window.
 * Loop and send run on `Dispatchers.Default`: `Dispatchers.IO` is internal
 * on Kotlin/Native, and Default is the platform's background pool.
 */
actual fun createUdpSocketManager(scope: CoroutineScope): UdpSocketManager =
    PosixUdpSocketManager(scope)

/** Wake interval so a blocked `recvfrom` notices cancellation/close. */
private const val RECEIVE_TIMEOUT_US = 500_000

/** Receive buffer; protocol frames are 6 (control) / 9 (telemetry) bytes. */
private const val MAX_DATAGRAM_BYTES = 64

internal class PosixUdpSocketManager(
    private val scope: CoroutineScope,
) : UdpSocketManager {
    /** Bound eagerly at construction to an ephemeral port. */
    private val fd: Int = createBoundSocket()

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
        receiveJob = scope.launch(Dispatchers.Default) {
            while (isActive && !closed) {
                val datagram = receiveOne()
                if (closed || !isActive) break
                // tryEmit (never emit): a full buffer must drop, not suspend
                // the receiver and stall pairing/telemetry reads.
                if (datagram != null && !_incoming.tryEmit(datagram)) {
                    droppedIncoming++
                }
            }
        }
    }

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        withContext(Dispatchers.Default) {
            memScoped {
                val dest = alloc<sockaddr_in>()
                memset(dest.ptr, 0, sizeOf<sockaddr_in>().convert())
                dest.sin_len = sizeOf<sockaddr_in>().toUByte()
                dest.sin_family = AF_INET.toUByte()
                dest.sin_port = hostPortToNetworkOrder(port)
                dest.sin_addr.s_addr = parseIpv4ToNetworkOrder(host)
                val sent = bytes.usePinned { pinned ->
                    sendto(
                        fd,
                        pinned.addressOf(0),
                        bytes.size.convert(),
                        0,
                        dest.ptr.reinterpret<sockaddr>(),
                        sizeOf<sockaddr_in>().convert(),
                    )
                }
                if (sent < 0) throw IllegalStateException("sendto to $host:$port failed")
            }
        }
    }

    override suspend fun close() {
        closed = true
        receiveJob.cancel()
        close(fd)
    }

    /**
     * One blocking `recvfrom` round. Returns null on timeout or after
     * [close]; never throws for transport-level failures (the loop simply
     * retries). The Kotlin side owns the byte buffer (`usePinned`), so no
     * C-array indexing is needed.
     */
    private fun receiveOne(): UdpDatagram? {
        val buf = ByteArray(MAX_DATAGRAM_BYTES)
        var sender: Pair<String, Int>? = null
        var count = -1
        buf.usePinned { pinned ->
            memScoped {
                val src = alloc<sockaddr_in>()
                memset(src.ptr, 0, sizeOf<sockaddr_in>().convert())
                // Mutable socklen_t slot, initialized to the struct size as
                // recvfrom requires on entry (it writes the actual length back).
                val addrLen = alloc<socklen_tVar>()
                addrLen.value = sizeOf<sockaddr_in>().convert()
                val n = recvfrom(
                    fd,
                    pinned.addressOf(0),
                    MAX_DATAGRAM_BYTES.convert(),
                    0,
                    src.ptr.reinterpret<sockaddr>(),
                    addrLen.ptr,
                )
                if (n <= 0) return@memScoped
                sender = formatIpv4FromNetworkOrder(src.sin_addr.s_addr) to networkOrderToHostPort(src.sin_port)
                count = n.toInt()
            }
        }
        val s = sender ?: return null
        return UdpDatagram(buf.copyOf(count), s.first, s.second)
    }
}

/** Create, configure (broadcast + recv timeout), and bind a UDP socket. */
private fun createBoundSocket(): Int {
    val newFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
    if (newFd == -1) throw IllegalStateException("socket() failed")
    memScoped {
        // Kernel only reads the flag: immutable cValues need no Var slot.
        if (setsockopt(
                newFd,
                SOL_SOCKET,
                SO_BROADCAST,
                cValuesOf(1),
                sizeOf<IntVar>().convert(),
            ) != 0
        ) {
            close(newFd)
            throw IllegalStateException("setsockopt(SO_BROADCAST) failed")
        }
        val timeout = alloc<timeval>()
        memset(timeout.ptr, 0, sizeOf<timeval>().convert())
        timeout.tv_sec = 0
        timeout.tv_usec = RECEIVE_TIMEOUT_US
        if (setsockopt(
                newFd,
                SOL_SOCKET,
                SO_RCVTIMEO,
                timeout.ptr,
                sizeOf<timeval>().convert(),
            ) != 0
        ) {
            close(newFd)
            throw IllegalStateException("setsockopt(SO_RCVTIMEO) failed")
        }
        val addr = alloc<sockaddr_in>()
        memset(addr.ptr, 0, sizeOf<sockaddr_in>().convert())
        addr.sin_len = sizeOf<sockaddr_in>().toUByte()
        addr.sin_family = AF_INET.toUByte()
        addr.sin_port = 0u
        addr.sin_addr.s_addr = INADDR_ANY
        if (bind(newFd, addr.ptr.reinterpret<sockaddr>(), sizeOf<sockaddr_in>().convert()) != 0) {
            close(newFd)
            throw IllegalStateException("bind() to ephemeral port failed")
        }
    }
    return newFd
}
