package com.desky.shared.domain

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.mapNotNull

/** Decode the raw socket stream into telemetry snapshots. */
class ObserveTelemetryUseCase(
    private val udp: UdpSocketManager,
) {
    /**
     * Garbage datagrams (wrong header/length/checksum) are dropped via
     * [mapNotNull] and never crash the flow.
     */
    operator fun invoke(): Flow<Telemetry> =
        udp.incoming.mapNotNull { datagram ->
            runCatching { TelemetryPacketCodec.decode(datagram.bytes) }.getOrNull()
        }
}
