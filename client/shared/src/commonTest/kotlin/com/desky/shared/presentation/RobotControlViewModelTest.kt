package com.desky.shared.presentation

import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.ControlPacketCodec
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.DRIVE_V2_HEADER
import com.desky.shared.domain.DRIVE_V2_SIZE
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.GetBondPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.hexToBytes
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"

@OptIn(ExperimentalCoroutinesApi::class)
class RobotControlViewModelTest {

    private fun psk() = hexToBytes(PSK_HEX)

    private fun setup(
        repo: FakeDeviceRepository = FakeDeviceRepository(testDevice()),
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        vault: FakeBondSecretStore = FakeBondSecretStore().apply { seed(testDevice().id, PSK_HEX) },
        tickMs: Long = 20,
    ): Quad {
        val sendSigned = SendSignedControlUseCase(udp)
        val viewModel = RobotControlViewModel(
            sendSigned,
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(sendSigned),
            repo,
            GetBondPskUseCase(vault, repo),
            udp,
            tickMs,
        )
        return Quad(viewModel, repo, udp, vault)
    }

    private data class Quad(
        val vm: RobotControlViewModel,
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val vault: FakeBondSecretStore,
    )

    @Test
    fun stickUpdatesStateClamped() = runVmTest { main ->
        val (viewModel, _, _, _) = setup()
        main.runCurrent()

        viewModel.onStick(0.5f, -0.25f)
        assertEquals(0.5f, viewModel.state.value.throttle)
        assertEquals(-0.25f, viewModel.state.value.steering)

        viewModel.onStick(9f, -9f)
        assertEquals(1f, viewModel.state.value.throttle)
        assertEquals(-1f, viewModel.state.value.steering)
    }

    @Test
    fun sendLoopHeartbeatsSignedWithIncreasingCounters() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        // First frame goes out immediately: signed centered MANUAL heartbeat,
        // counter 1 (fresh pair resets Room lastCounter to 0).
        assertEquals(1, udp.sent.size)
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                ControlPacketCodec.encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 1u, psk()),
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
        // Every frame is a 16B DRIVE_V2 datagram with counters 1..6.
        udp.sent.forEachIndexed { i, frame ->
            assertEquals(DRIVE_V2_SIZE, frame.bytes.size, "frame $i")
            assertEquals(DRIVE_V2_HEADER, frame.bytes[0].toInt() and 0xFF, "frame $i")
        }
        val last = udp.sent.last()
        assertTrue(
            last.bytes.contentEquals(
                ControlPacketCodec.encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, -0.25f, 0), 6u, psk()),
            ),
        )
    }

    @Test
    fun vaultReadOncePerSession() = runVmTest { main ->
        val (_, _, udp, vault) = setup(tickMs = 20)
        main.runCurrent()
        main.advanceTimeBy(201)

        assertTrue(udp.sent.size >= 10)
        assertEquals(1, vault.loadCalls, "vault read once, session cached in memory")
        main.advanceTimeBy(201)
        assertEquals(1, vault.loadCalls)
    }

    @Test
    fun counterPersistsEvery128Sends() = runVmTest { main ->
        val deviceId = testDevice().id
        val (_, repo, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        // 129 frames total (t=0..2560): the 128th successful send persists.
        main.advanceTimeBy(2561)

        assertEquals(129, udp.sent.size)
        assertEquals(listOf(deviceId to 128u), repo.counterSaves)
    }

    @Test
    fun resumesCounterFromRoomRow() = runVmTest { main ->
        val device = testDevice().copy(lastCounter = 100u)
        val (viewModel, _, udp, _) = setup(repo = FakeDeviceRepository(device))
        main.runCurrent()

        assertEquals(1, udp.sent.size)
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                ControlPacketCodec.encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 101u, psk()),
            ),
        )
        assertNull(viewModel.state.value.error)
    }

    @Test
    fun missingPskErrorsWithZeroSends() = runVmTest { main ->
        // Paired, but the vault is empty: keyless bond, never legacy frames.
        val (viewModel, _, udp, vault) = setup(
            vault = FakeBondSecretStore(),
        )
        main.runCurrent()
        main.advanceTimeBy(101)

        assertTrue(udp.sent.isEmpty(), "no unsigned fallback, no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        assertEquals(1, vault.loadCalls, "missing PSK latched: vault read once, loop idles")
    }

    @Test
    fun telemetryUpdatesStateAndMode() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup()
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
        val (viewModel, _, _, _) = setup(repo = repo)
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
    fun emergencyStopLatchesAndKeepsSendingSignedEstop() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
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
        // Immediate signed estop frame via the dedicated use case
        // (counter 1 went out before the clear, so this is 2).
        val estop2 = ControlPacketCodec.encodeSigned(
            ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
            2u,
            psk(),
        )
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(udp.sent[0].bytes.contentEquals(estop2))

        // Moving the stick must NOT release the latch...
        viewModel.onStick(0.8f, -0.8f)
        assertTrue(viewModel.state.value.eStopped)

        // ...and the heartbeat loop keeps sending signed centered+ESTOP_FLAG
        // (+1 for advanceTimeBy's exclusive end boundary): counters 3,4,5.
        main.advanceTimeBy(61)
        val loopFrames = udp.sent.drop(1)
        assertTrue(loopFrames.size >= 3)
        loopFrames.forEachIndexed { i, frame ->
            val expected = ControlPacketCodec.encodeSigned(
                ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
                (3u + i.toUInt()),
                psk(),
            )
            assertTrue(frame.bytes.contentEquals(expected), "loop frame $i")
        }
        assertEquals(0x01, ESTOP_FLAG)
    }

    @Test
    fun unpairPausesSendLoopDropsCacheAndRepairReloads() = runVmTest { main ->
        val (_, repo, udp, vault) = setup(tickMs = 20)
        main.runCurrent()
        assertTrue(udp.sent.isNotEmpty())
        assertEquals(1, vault.loadCalls)

        // Forgetting the device mid-session must stop frames to the stale IP.
        repo.emit(null)
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(101)
        assertTrue(udp.sent.isEmpty())

        // Re-pairing resumes the heartbeat with a FRESH session load.
        repo.emit(testDevice())
        main.runCurrent()
        main.advanceTimeBy(21)
        assertTrue(udp.sent.isNotEmpty())
        assertEquals(2, vault.loadCalls, "cache dropped on unpair: re-pair re-reads the vault")
        // Fresh pair row resets the counter: first frame back is counter 1.
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                ControlPacketCodec.encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 1u, psk()),
            ),
        )
    }

    @Test
    fun releaseEmergencyClearsLatch() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertTrue(viewModel.state.value.eStopped)

        viewModel.onReleaseEmergency()
        assertFalse(viewModel.state.value.eStopped)

        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary. Counters 1 (loop)
        // and 2 (immediate estop) went out before the clear, so next is 3.
        main.advanceTimeBy(21)
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                ControlPacketCodec.encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 3u, psk()),
            ),
        )
    }
}
