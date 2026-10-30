package com.desky.shared.features.control

import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.UdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.flow.shareIn

/**
 * Session-scope lazy hub sharing the latest decoded telemetry snapshot.
 *
 * Collection is lazy: the upstream socket flow is collected only while at
 * least one subscriber observes [telemetry], and stops
 * [stopTimeoutMillis] after the last subscriber disappears. While
 * uncollected, connectivity must be treated by consumers as frozen at
 * [latest] — the hub never synthesizes a disconnected state, and absence
 * of emissions is not a signal.
 */
class TelemetryHub(
    udp: UdpSocketManager,
    scope: CoroutineScope,
    stopTimeoutMillis: Long = 5000,
) {
    val telemetry: SharedFlow<Telemetry> =
        udp.incoming.mapNotNull { datagram ->
            runCatching { TelemetryPacketCodec.decode(datagram.bytes) }.getOrNull()
        }.shareIn(scope, SharingStarted.WhileSubscribed(stopTimeoutMillis), replay = 1)

    /** Last decoded snapshot, or null before the first valid datagram. */
    val latest: Telemetry?
        get() = telemetry.replayCache.firstOrNull()
}
