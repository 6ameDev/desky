package com.desky.shared.presentation

import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.ControlPacketCodec
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendRobotControlCommandUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.UdpDatagram
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class RobotControlViewModelTest {

    private fun setup(
        repo: FakeDeviceRepository = FakeDeviceRepository(testDevice()),
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        tickMs: Long = 20,
    ): Triple<RobotControlViewModel, FakeDeviceRepository, FakeUdpSocketManager> {
        val viewModel = RobotControlViewModel(
            SendRobotControlCommandUseCase(udp),
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(udp),
            repo,
            udp,
            tickMs,
        )
        return Triple(viewModel, repo, udp)
    }

    @Test
    fun stickUpdatesStateClamped() = runVmTest { main ->
        val (viewModel, _, _) = setup()
        main.runCurrent()

        viewModel.onStick(0.5f, -0.25f)
        assertEquals(0.5f, viewModel.state.value.throttle)
        assertEquals(-0.25f, viewModel.state.value.steering)

        viewModel.onStick(9f, -9f)
        assertEquals(1f, viewModel.state.value.throttle)
        assertEquals(-1f, viewModel.state.value.steering)
    }

    @Test
    fun sendLoopHeartbeatsOnVirtualTime() = runVmTest { main ->
        val (viewModel, _, udp) = setup(tickMs = 20)
        main.runCurrent()
        // First frame goes out immediately: centered MANUAL heartbeat.
        assertEquals(1, udp.sent.size)
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0)),
            ),
        )
        assertEquals("192.168.4.1", udp.sent[0].host)
        assertEquals(DEFAULT_UDP_PORT, udp.sent[0].port)

        viewModel.onStick(0.5f, -0.25f)
        // +1 past the last tick: advanceTimeBy() does NOT run tasks due
        // exactly at the end of the window (exclusive boundary).
        main.advanceTimeBy(101)

        // Sends at t=20,40,60,80,100 on top of the t=0 frame.
        assertEquals(6, udp.sent.size)
        val last = udp.sent.last()
        assertTrue(
            last.bytes.contentEquals(
                ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0.5f, -0.25f, 0)),
            ),
        )
    }

    @Test
    fun telemetryUpdatesStateAndMode() = runVmTest { main ->
        val (viewModel, _, udp) = setup()
        main.runCurrent()
        assertEquals(Connectivity.Disconnected, viewModel.state.value.connectivity)

        val packet = Telemetry(
            pitchDeg = 3.1f,
            rollDeg = -1.2f,
            distanceMm = 900,
            cliff = false,
            driving = true,
            mode = RobotMode.AUTONOMOUS,
        )
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), "192.168.4.1", DEFAULT_UDP_PORT))
        main.runCurrent()

        val state = viewModel.state.value
        assertEquals(Connectivity.Connected, state.connectivity)
        assertEquals(RobotMode.AUTONOMOUS, state.mode)
        assertEquals(packet, state.telemetry)
    }

    @Test
    fun unpairedShowsUntilTelemetry() = runVmTest { main ->
        val repo = FakeDeviceRepository(null)
        val (viewModel, _, _) = setup(repo = repo)
        main.runCurrent()
        assertEquals(Connectivity.Unpaired, viewModel.state.value.connectivity)

        repo.emit(testDevice())
        // Bounded pump only: pairing starts the infinite heartbeat loop,
        // which advanceUntilIdle() could never drain.
        main.runCurrent()
        // Paired, but no telemetry yet: still Unpaired, never Disconnected.
        assertEquals(Connectivity.Unpaired, viewModel.state.value.connectivity)
    }

    @Test
    fun emergencyStopLatchesAndKeepsSendingEstop() = runVmTest { main ->
        val (viewModel, _, udp) = setup(tickMs = 20)
        main.runCurrent()
        viewModel.onStick(0.5f, 0.5f)
        main.runCurrent()
        udp.sent.clear()

        viewModel.onEmergencyStop()
        main.runCurrent()

        val latched = viewModel.state.value
        assertTrue(latched.eStopped)
        // Sticks center on estop.
        assertEquals(0f, latched.throttle)
        assertEquals(0f, latched.steering)
        // Immediate estop frame via the dedicated use case.
        val estopBytes = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG))
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(udp.sent[0].bytes.contentEquals(estopBytes))

        // Moving the stick must NOT release the latch...
        viewModel.onStick(0.8f, -0.8f)
        assertTrue(viewModel.state.value.eStopped)

        // ...and the heartbeat loop keeps sending centered+ESTOP_FLAG
        // (+1 for advanceTimeBy's exclusive end boundary).
        main.advanceTimeBy(61)
        val loopFrames = udp.sent.drop(1)
        assertTrue(loopFrames.size >= 3)
        for (frame in loopFrames) {
            assertTrue(frame.bytes.contentEquals(estopBytes))
        }
        assertEquals(0x01, ESTOP_FLAG)
    }

    @Test
    fun unpairPausesSendLoopAndRepairResumes() = runVmTest { main ->
        val (_, repo, udp) = setup(tickMs = 20)
        main.runCurrent()
        assertTrue(udp.sent.isNotEmpty())

        // Forgetting the device mid-session must stop frames to the stale IP.
        repo.emit(null)
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(101)
        assertTrue(udp.sent.isEmpty())

        // Re-pairing resumes the heartbeat.
        repo.emit(testDevice())
        main.runCurrent()
        main.advanceTimeBy(21)
        assertTrue(udp.sent.isNotEmpty())
    }

    @Test
    fun releaseEmergencyClearsLatch() = runVmTest { main ->
        val (viewModel, _, udp) = setup(tickMs = 20)
        main.runCurrent()
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertTrue(viewModel.state.value.eStopped)

        viewModel.onReleaseEmergency()
        assertFalse(viewModel.state.value.eStopped)

        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary.
        main.advanceTimeBy(21)
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0)),
            ),
        )
    }
}
