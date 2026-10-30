package com.desky.shared.features.control

import app.cash.turbine.test
import com.desky.shared.core.ControlCommand
import com.desky.shared.core.ControlPacketCodec
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.UdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.onCompletion
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

private class NoopSendUdpSocketManager(
    override val incoming: Flow<UdpDatagram>,
) : UdpSocketManager {
    override suspend fun send(bytes: ByteArray, host: String, port: Int) = Unit
    override suspend fun close() = Unit
}

class TelemetryHubTest {

    private val t1 = Telemetry(12.3f, -4.5f, 1234, cliff = false, driving = true, mode = RobotMode.MANUAL)
    private val t2 = Telemetry(-9.9f, 0f, 42, cliff = true, driving = false, mode = RobotMode.EMERGENCY)

    private fun datagram(bytes: ByteArray) = UdpDatagram(bytes, "test", 3333)

    private fun TestScope.hubScope() = CoroutineScope(UnconfinedTestDispatcher(testScheduler))

    @Test
    fun firstSubscriberStartsCollectionAndGarbageIsDropped() = runTest {
        val incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
        val hub = TelemetryHub(NoopSendUdpSocketManager(incoming), hubScope())
        assertNull(hub.latest)

        hub.telemetry.test {
            incoming.emit(datagram(TelemetryPacketCodec.encode(t1)))
            assertEquals(t1, awaitItem())
            assertEquals(t1, hub.latest)

            incoming.emit(datagram(ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f))))
            incoming.emit(datagram(ByteArray(0)))
            incoming.emit(datagram(TelemetryPacketCodec.encode(t2)))
            assertEquals(t2, awaitItem())
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun lateSubscriberReplaysLatest() = runTest {
        val incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
        val hub = TelemetryHub(NoopSendUdpSocketManager(incoming), hubScope())
        val firstSubscriber = CoroutineScope(UnconfinedTestDispatcher(testScheduler))

        val job = firstSubscriber.launch { hub.telemetry.collect {} }
        incoming.emit(datagram(TelemetryPacketCodec.encode(t1)))
        incoming.emit(datagram(TelemetryPacketCodec.encode(t2)))

        hub.telemetry.test {
            assertEquals(t2, awaitItem())
            assertEquals(t2, hub.latest)
            cancelAndIgnoreRemainingEvents()
        }
        job.cancel()
    }

    @Test
    fun upstreamStopsAfterTimeoutWithZeroSubscribers() = runTest {
        var collections = 0
        val incoming = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
        val counting = incoming
            .onStart { collections++ }
            .onCompletion { collections-- }
        val hub = TelemetryHub(NoopSendUdpSocketManager(counting), hubScope(), stopTimeoutMillis = 10)

        assertEquals(0, collections)
        val job = launch { hub.telemetry.collect {} }
        testScheduler.advanceUntilIdle()
        assertEquals(1, collections)

        job.cancel()
        testScheduler.advanceTimeBy(100)
        testScheduler.advanceUntilIdle()
        assertEquals(0, collections)
        assertNull(hub.latest)
    }
}
