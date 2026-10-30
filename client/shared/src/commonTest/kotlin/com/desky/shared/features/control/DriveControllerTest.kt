package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.DRIVE_HEADER
import com.desky.shared.core.MANUAL_RELEASE_HEADER
import com.desky.shared.core.MANUAL_TAKE_HEADER
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotMode
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.encodeManualRelease
import com.desky.shared.core.encodeManualTake
import com.desky.shared.core.encodeSigned
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val BYTE_MASK = 0xFF
private const val DRIVE_COUNTER_AT = 5
private const val SESSION_COUNTER_AT = 3

private fun pairedDevice() = PairedDevice(
    id = "desky-$ROBOT_HOST",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "token",
    era = 0u,
)

private fun psk() = hexToBytes(PSK_HEX)

private fun headerOf(bytes: ByteArray) = bytes[0].toInt() and BYTE_MASK

private fun counterOf(bytes: ByteArray, at: Int): UInt {
    val b0 = bytes[at].toInt() and BYTE_MASK
    val b1 = bytes[at + 1].toInt() and BYTE_MASK
    val b2 = bytes[at + 2].toInt() and BYTE_MASK
    val b3 = bytes[at + 3].toInt() and BYTE_MASK
    return ((b0 shl 24) or (b1 shl 16) or (b2 shl 8) or b3).toUInt()
}

@OptIn(ExperimentalCoroutinesApi::class)
class DriveControllerTest {

    private data class Setup(
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val controller: DriveController,
    )

    private fun CoroutineScope.setup(
        device: PairedDevice? = pairedDevice(),
        tickMs: Long = 20,
        idleHeartbeatMs: Long = 100,
    ): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(pairedDevice().id, PSK_HEX) }
        val udp = FakeUdpSocketManager()
        val holder = ToRobotCounter(repo)
        val controller = DriveController(
            deviceRepo = repo,
            getPairPsk = GetPairPskUseCase(vault, repo),
            toRobotCounter = holder,
            sendUC = SendSignedControlUseCase(udp),
            takeManualUC = TakeManualUseCase(udp),
            releaseManualUC = ReleaseManualUseCase(udp),
            scope = this,
            tickMs = tickMs,
            idleHeartbeatMs = idleHeartbeatMs,
        )
        return Setup(repo, udp, controller)
    }

    @Test
    fun idleZeroedFramesWhileUnbound() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            testScheduler.advanceTimeBy(1001)

            assertTrue(s.udp.sent.isNotEmpty(), "failsafe heartbeat preserved while unbound")
            assertTrue(s.udp.sent.size < 50, "idle throttles below full rate, got ${s.udp.sent.size}")
            s.udp.sent.forEachIndexed { i, frame ->
                assertEquals(DRIVE_HEADER, headerOf(frame.bytes), "frame $i")
                val counter = counterOf(frame.bytes, DRIVE_COUNTER_AT)
                assertEquals((i + 1).toUInt(), counter, "skipped ticks allocate no counters")
                assertTrue(
                    frame.bytes.contentEquals(
                        encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f), counter, psk()),
                    ),
                    "frame $i zeroed",
                )
            }
        } finally {
            s.controller.stop()
        }
    }

    @Test
    fun takeOnBindThenFullRateInputFrames() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.controller.bind { 0.5f to -0.25f }
            runCurrent()

            val takes = s.udp.sent.filter { headerOf(it.bytes) == MANUAL_TAKE_HEADER }
            assertEquals(1, takes.size, "exactly one take per bind")
            val takeCounter = counterOf(takes[0].bytes, SESSION_COUNTER_AT)
            assertTrue(
                takes[0].bytes.contentEquals(encodeManualTake(psk(), 0u, takeCounter)),
                "take consumes the shared counter",
            )

            val sentBefore = s.udp.sent.size
            testScheduler.advanceTimeBy(101)
            val fresh = s.udp.sent.drop(sentBefore)
            assertEquals(5, fresh.size, "bound streams every tick")
            val expected = ControlCommand(RobotMode.MANUAL, 0.5f, -0.25f)
            var prev = takeCounter
            fresh.forEachIndexed { i, frame ->
                assertEquals(DRIVE_HEADER, headerOf(frame.bytes), "fresh $i")
                val counter = counterOf(frame.bytes, DRIVE_COUNTER_AT)
                assertTrue(counter > prev, "take precedes drive")
                prev = counter
                assertTrue(
                    frame.bytes.contentEquals(encodeSigned(expected, counter, psk())),
                    "fresh $i carries the bound input",
                )
            }
        } finally {
            s.controller.stop()
        }
    }

    @Test
    fun releaseOnUnbindBackToIdle() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.controller.bind { 0.5f to 0f }
            runCurrent()
            testScheduler.advanceTimeBy(101)
            assertTrue(
                s.udp.sent.any { headerOf(it.bytes) == DRIVE_HEADER },
                "drive flows while bound",
            )

            s.controller.unbind()
            runCurrent()

            val releases = s.udp.sent.filter { headerOf(it.bytes) == MANUAL_RELEASE_HEADER }
            assertEquals(2, releases.size, "best-effort 2x repeat")
            assertTrue(
                releases[0].bytes.contentEquals(releases[1].bytes),
                "byte-identical repeat: one allocation",
            )
            assertTrue(
                releases[0].bytes.contentEquals(
                    encodeManualRelease(psk(), 0u, counterOf(releases[0].bytes, SESSION_COUNTER_AT)),
                ),
            )

            s.udp.sent.clear()
            testScheduler.advanceTimeBy(1000)
            assertTrue(s.udp.sent.isNotEmpty(), "heartbeat preserved after unbind")
            assertTrue(s.udp.sent.size < 50, "idle throttles, got ${s.udp.sent.size}")
            val zeroed = ControlCommand(RobotMode.MANUAL, 0f, 0f)
            val counters = s.udp.sent.map { frame ->
                assertEquals(DRIVE_HEADER, headerOf(frame.bytes))
                val counter = counterOf(frame.bytes, DRIVE_COUNTER_AT)
                assertTrue(
                    frame.bytes.contentEquals(encodeSigned(zeroed, counter, psk())),
                    "idle frames zeroed after unbind",
                )
                counter
            }
            counters.zipWithNext { a, b -> assertEquals(a + 1u, b, "gapless after unbind") }
        } finally {
            s.controller.stop()
        }
    }

    @Test
    fun releaseFailureStillNavigatesToIdle() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.controller.bind { 0.5f to 0f }
            runCurrent()
            testScheduler.advanceTimeBy(101)
            assertTrue(s.udp.sent.any { headerOf(it.bytes) == DRIVE_HEADER })

            s.udp.sendError = IllegalStateException("dead socket")
            s.controller.unbind()
            runCurrent()
            val frozen = s.udp.sent.size
            testScheduler.advanceTimeBy(200)
            assertEquals(frozen, s.udp.sent.size, "failed sends transmit nothing")

            s.udp.sendError = null
            s.udp.sent.clear()
            testScheduler.advanceTimeBy(500)
            assertTrue(s.udp.sent.isNotEmpty(), "loop survives the failure")
            val zeroed = ControlCommand(RobotMode.MANUAL, 0f, 0f)
            s.udp.sent.forEach { frame ->
                assertEquals(DRIVE_HEADER, headerOf(frame.bytes))
                assertTrue(
                    frame.bytes.contentEquals(
                        encodeSigned(zeroed, counterOf(frame.bytes, DRIVE_COUNTER_AT), psk()),
                    ),
                    "idle zeroed despite the failed release",
                )
            }
        } finally {
            s.controller.stop()
        }
    }

    @Test
    fun stopCancelsAllLoops() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.controller.bind { 0.5f to 0f }
            runCurrent()
            testScheduler.advanceTimeBy(101)
            assertTrue(s.udp.sent.isNotEmpty(), "drive flows before stop")

            s.controller.stop()
            val frozen = s.udp.sent.size
            testScheduler.advanceTimeBy(1000)
            runCurrent()
            assertEquals(frozen, s.udp.sent.size, "no sends after stop")
        } finally {
            s.controller.stop()
        }
    }
}
