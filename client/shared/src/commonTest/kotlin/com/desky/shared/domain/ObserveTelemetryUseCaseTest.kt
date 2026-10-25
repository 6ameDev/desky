package com.desky.shared.domain

import app.cash.turbine.test
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.flowOf
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlinx.coroutines.test.runTest

/** Fake transport: scripted inbound flow plus a record of outbound sends. */
private class FakeUdpSocketManager(
    override val incoming: Flow<UdpDatagram>,
) : UdpSocketManager {
    val sent = mutableListOf<UdpDatagram>()
    var sendError: Throwable? = null

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sendError?.let { throw it }
        sent += UdpDatagram(bytes, host, port)
    }

    override suspend fun close() = Unit
}

class ObserveTelemetryUseCaseTest {

    private val t1 = Telemetry(12.3f, -4.5f, 1234, cliff = false, driving = true, mode = RobotMode.MANUAL)
    private val t2 = Telemetry(-9.9f, 0f, 42, cliff = true, driving = false, mode = RobotMode.EMERGENCY)

    private fun datagram(bytes: ByteArray) = UdpDatagram(bytes, ROBOT_HOST, DEFAULT_UDP_PORT)

    @Test
    fun garbageDatagramsAreDroppedNeverCrash() = runTest {
        val incoming = MutableSharedFlow<UdpDatagram>()
        val useCase = ObserveTelemetryUseCase(FakeUdpSocketManager(incoming))

        val good1 = TelemetryPacketCodec.encode(t1)
        val wrongHeader = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f))
        val badChecksum = TelemetryPacketCodec.encode(t2).also { it[8] = (it[8] + 1).toByte() }
        val good2 = TelemetryPacketCodec.encode(t2)

        useCase().test {
            incoming.emit(datagram(good1))
            incoming.emit(datagram(wrongHeader))
            incoming.emit(datagram(badChecksum))
            incoming.emit(datagram(ByteArray(0)))
            incoming.emit(datagram(good2))

            assertEquals(t1, awaitItem())
            assertEquals(t2, awaitItem())
            // No third item: all garbage was dropped.
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun completesWhenSocketFlowCloses() = runTest {
        val good1 = TelemetryPacketCodec.encode(t1)
        val garbage = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f))
        val good2 = TelemetryPacketCodec.encode(t2)
        val fake = FakeUdpSocketManager(flowOf(datagram(good1), datagram(garbage), datagram(good2)))

        ObserveTelemetryUseCase(fake)().test {
            assertEquals(t1, awaitItem())
            assertEquals(t2, awaitItem())
            awaitComplete()
        }
    }
}
