package com.desky.shared.data

import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.ControlPacketCodec
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.withTimeoutOrNull

/**
 * UDP discovery for robots on the local subnet (T5 data layer).
 *
 * The robot only pins its unicast peer on a VALID 6-byte control packet (it
 * drops malformed datagrams), so discovery opens with one hello frame — the
 * centered [ControlCommand] (`MANUAL, 0, 0, flags 0`) — sent to the subnet
 * broadcast address on [DEFAULT_UDP_PORT]. Never empty/arbitrary bytes.
 *
 * Robots in AP mode additionally broadcast telemetry to `.255`, so listening
 * alone finds them; the hello additionally pins unicast for the control
 * stream that follows.
 *
 * Only senders of at least one decodable telemetry frame are reported, so
 * stray non-robot traffic on the port is ignored. Result order is
 * first-seen order.
 */
class UdpDeviceDiscovery(
    private val udp: UdpSocketManager,
) {
    suspend fun discover(timeoutMs: Long = 3000): List<String> {
        val hello = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0))
        udp.send(hello, BROADCAST_HOST, DEFAULT_UDP_PORT)
        val hosts = LinkedHashSet<String>()
        withTimeoutOrNull(timeoutMs) {
            udp.incoming.collect { datagram ->
                if (TelemetryPacketCodec.decode(datagram.bytes) != null) {
                    hosts.add(datagram.host)
                }
            }
        }
        return hosts.toList()
    }

    companion object {
        /** Subnet broadcast; the robot listens here per the wire brief. */
        const val BROADCAST_HOST = "255.255.255.255"
    }
}
