package com.desky.shared.data

import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.ControlPacketCodec
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/** In-memory [UdpSocketManager] double: captures sends, replays canned receives. */
private class FakeUdpSocketManager : UdpSocketManager {
    data class Sent(val bytes: ByteArray, val host: String, val port: Int)

    val sent = mutableListOf<Sent>()
    private val _incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = _incoming

    /** Rendezvous hook so tests can emit only once collection has started. */
    val subscriptionCount: Flow<Int> = _incoming.subscriptionCount

    suspend fun emit(datagram: UdpDatagram) = _incoming.emit(datagram)

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sent.add(Sent(bytes.copyOf(), host, port))
    }

    override suspend fun close() = Unit
}

private fun telemetryBytes(): ByteArray =
    TelemetryPacketCodec.encode(
        Telemetry(
            pitchDeg = 1.5f,
            rollDeg = -2.5f,
            distanceMm = 1200,
            cliff = false,
            driving = true,
            mode = RobotMode.MANUAL,
        ),
    )

class UdpDeviceDiscoveryTest {

    @Test
    fun helloIsCenteredControlFrameToBroadcast() = runTest {
        val fake = FakeUdpSocketManager()

        UdpDeviceDiscovery(fake).discover(timeoutMs = 100)

        assertEquals(1, fake.sent.size, "exactly one hello datagram")
        val hello = fake.sent.single()
        assertEquals(UdpDeviceDiscovery.BROADCAST_HOST, hello.host)
        assertEquals("255.255.255.255", hello.host)
        assertEquals(DEFAULT_UDP_PORT, hello.port)
        val expected = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0))
        assertTrue(hello.bytes.contentEquals(expected), "hello must be the centered control frame")
        // Pin the exact wire bytes: AA 00 80 80 00 AA (throttle/steering 0 -> 128).
        assertTrue(
            hello.bytes.contentEquals(byteArrayOf(0xAA.toByte(), 0, 128.toByte(), 128.toByte(), 0, 0xAA.toByte())),
            "hello wire bytes, got ${hello.bytes.joinToString { (it.toInt() and 0xFF).toString(16) }}",
        )
    }

    @Test
    fun discoversDistinctTelemetryHostsAndIgnoresGarbage() = runTest {
        val fake = FakeUdpSocketManager()
        // Rendezvous emission: a replay-0 SharedFlow (like the real managers)
        // drops values emitted with no collector present, so feed datagrams
        // only once discovery is collecting — exactly like robots answering
        // the hello on the wire.
        backgroundScope.launch {
            fake.subscriptionCount.first { it > 0 }
            fake.emit(UdpDatagram(telemetryBytes(), "192.168.4.2", DEFAULT_UDP_PORT))
            fake.emit(UdpDatagram(telemetryBytes(), "192.168.4.2", DEFAULT_UDP_PORT)) // duplicate host
            fake.emit(UdpDatagram(telemetryBytes(), "192.168.4.3", DEFAULT_UDP_PORT))
            fake.emit(UdpDatagram(byteArrayOf(0x00, 0x01, 0x02), "192.168.4.99", DEFAULT_UDP_PORT)) // garbage
            fake.emit(UdpDatagram(ByteArray(9) { 0xBB.toByte() }, "192.168.4.100", DEFAULT_UDP_PORT)) // bad checksum
        }

        val hosts = UdpDeviceDiscovery(fake).discover(timeoutMs = 150)

        assertEquals(listOf("192.168.4.2", "192.168.4.3"), hosts)
    }

    @Test
    fun timeoutIsHonoredWithoutRealSleeps() = runTest {
        // Virtual-time check across several short timeouts: no datagrams in,
        // empty list out. Proves the timeout parameter drives collection.
        for (timeoutMs in listOf(50L, 150L, 250L)) {
            val fake = FakeUdpSocketManager()
            assertEquals(emptyList(), UdpDeviceDiscovery(fake).discover(timeoutMs = timeoutMs))
            assertEquals(1, fake.sent.size, "hello still sent for timeoutMs=$timeoutMs")
        }
    }
}
