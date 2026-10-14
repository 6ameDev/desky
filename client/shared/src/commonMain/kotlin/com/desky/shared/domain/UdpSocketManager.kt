package com.desky.shared.domain

import kotlinx.coroutines.flow.Flow

/**
 * Raw UDP transport boundary. Deliberately untyped ([ByteArray] in/out):
 * packet framing lives in the codecs ([ControlPacketCodec],
 * [TelemetryPacketCodec]) and the use cases that drive them, so this
 * interface never needs a single-impl wrapper per direction (T5 provides
 * the platform socket).
 */
interface UdpSocketManager {
    /** Send a datagram to [host]:[port]. */
    suspend fun send(bytes: ByteArray, host: String, port: Int)

    /** Hot stream of received datagrams. */
    val incoming: Flow<UdpDatagram>

    /** Release the underlying socket. */
    suspend fun close()
}
