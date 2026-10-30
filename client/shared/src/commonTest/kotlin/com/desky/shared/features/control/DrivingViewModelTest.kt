package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.DRIVE_HEADER
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.MANUAL_RELEASE_HEADER
import com.desky.shared.core.MANUAL_TAKE_HEADER
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.encodeSigned
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
import com.desky.shared.features.control.ReleaseManualUseCase
import com.desky.shared.features.control.TakeManualUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import com.desky.shared.presentation.runVmTest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val BYTE_MASK = 0xFF
private const val DRIVE_COUNTER_AT = 5

@OptIn(ExperimentalCoroutinesApi::class)
class DrivingViewModelTest {

    private fun pairedDevice() = PairedDevice(
        id = "desky-$ROBOT_HOST",
        ip = ROBOT_HOST,
        port = DEFAULT_UDP_PORT,
        authToken = "token",
        era = 0u,
    )

    private data class Setup(
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val drive: DriveController,
        val manager: ConnectionManager,
        val vm: DrivingViewModel,
    )

    private fun CoroutineScope.setup(device: PairedDevice? = pairedDevice()): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(pairedDevice().id, PSK_HEX) }
        val udp = FakeUdpSocketManager()
        val toRobot = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val getPsk = GetPairPskUseCase(vault, repo)
        val manager = ConnectionManager(
            deviceRepo = repo,
            getPairPsk = getPsk,
            observeTelemetry = ObserveTelemetryUseCase(udp),
            toRobotCounter = toRobot,
            aliveUC = AliveUseCase(udp),
            remoteDisconnectUC = ObserveRemoteDisconnectUseCase(udp, repo, getPsk, vault, toRobot, fromRobot),
            scope = this,
            aliveIntervalMs = 100,
            staleTimeoutMs = 100,
            deadTimeoutMs = 300,
            tickMs = 20,
        )
        val drive = DriveController(
            deviceRepo = repo,
            getPairPsk = getPsk,
            toRobotCounter = toRobot,
            sendUC = SendSignedControlUseCase(udp),
            takeManualUC = TakeManualUseCase(udp),
            releaseManualUC = ReleaseManualUseCase(udp),
            scope = this,
            tickMs = 20,
            idleHeartbeatMs = 100,
        )
        val vm = DrivingViewModel(drive, manager, TelemetryHub(udp, this))
        return Setup(repo, udp, drive, manager, vm)
    }

    private fun headerOf(bytes: ByteArray) = bytes[0].toInt() and BYTE_MASK

    private fun counterOf(bytes: ByteArray, at: Int): UInt {
        val b0 = bytes[at].toInt() and BYTE_MASK
        val b1 = bytes[at + 1].toInt() and BYTE_MASK
        val b2 = bytes[at + 2].toInt() and BYTE_MASK
        val b3 = bytes[at + 3].toInt() and BYTE_MASK
        return ((b0 shl 24) or (b1 shl 16) or (b2 shl 8) or b3).toUInt()
    }

    private fun telemetryDatagram() = UdpDatagram(
        TelemetryPacketCodec.encode(
            Telemetry(1f, 2f, 3, cliff = false, driving = true, mode = RobotMode.MANUAL, manual = true),
        ),
        ROBOT_HOST,
        DEFAULT_UDP_PORT,
    )

    @Test
    fun activationBindsAndStreamsStickInput() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            val takes = s.udp.sent.filter { headerOf(it.bytes) == MANUAL_TAKE_HEADER }
            assertEquals(1, takes.size, "entry binds: exactly one take")

            s.vm.activate()
            main.runCurrent()
            assertEquals(
                1,
                s.udp.sent.count { headerOf(it.bytes) == MANUAL_TAKE_HEADER },
                "repeat activation while bound is a no-op",
            )

            s.vm.onStick(0.5f, -0.25f)
            assertEquals(0.5f, s.vm.state.value.throttle)
            assertEquals(-0.25f, s.vm.state.value.steering)
            s.vm.onStick(0.01f, 0.01f)
            assertEquals(0f, s.vm.state.value.throttle, "sub-threshold snaps to center")
            s.vm.onStick(0.5f, -0.25f)

            val sentBefore = s.udp.sent.size
            main.advanceTimeBy(101)
            val fresh = s.udp.sent.drop(sentBefore).filter { headerOf(it.bytes) == DRIVE_HEADER }
            assertTrue(fresh.isNotEmpty(), "bound streams drive frames")
            val expected = ControlCommand(RobotMode.MANUAL, 0.5f, -0.25f)
            val psk = hexToBytes(PSK_HEX)
            fresh.forEach { frame ->
                assertTrue(
                    frame.bytes.contentEquals(
                        encodeSigned(expected, counterOf(frame.bytes, DRIVE_COUNTER_AT), psk),
                    ),
                    "drive carries the stick input",
                )
            }
        } finally {
            s.manager.stop()
            s.drive.stop()
        }
    }

    /**
     * Background round-trip without navigation: deactivation releases and
     * the next activation takes MANUAL again. Simulates app-background then
     * foreground return while the screen stays visible.
     */
    @Test
    fun backgroundReturnRetakesManual() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(1, takes(s.udp).size, "first activation takes once")

            s.vm.deactivate()
            main.runCurrent()
            assertEquals(
                2,
                s.udp.sent.count { headerOf(it.bytes) == MANUAL_RELEASE_HEADER },
                "deactivation releases best-effort 2x",
            )

            s.vm.deactivate()
            main.runCurrent()
            assertEquals(
                2,
                s.udp.sent.count { headerOf(it.bytes) == MANUAL_RELEASE_HEADER },
                "repeat deactivation is a no-op",
            )

            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(2, takes(s.udp).size, "return re-takes MANUAL")
        } finally {
            s.manager.stop()
            s.drive.stop()
        }
    }

    private fun takes(udp: FakeUdpSocketManager) =
        udp.sent.filter { headerOf(it.bytes) == MANUAL_TAKE_HEADER }

    @Test
    fun collectsAlivenessAndTelemetry() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            assertEquals(Aliveness.Stale, s.vm.state.value.aliveness)
            assertNull(s.vm.state.value.telemetry)

            s.udp.inbound.emit(telemetryDatagram())
            main.runCurrent()
            assertEquals(Aliveness.Connected, s.vm.state.value.aliveness)
            assertEquals(true, s.vm.state.value.telemetry?.driving)

            s.repo.emit(null)
            main.runCurrent()
            assertEquals(Aliveness.Unpaired, s.vm.state.value.aliveness)
        } finally {
            s.manager.stop()
            s.drive.stop()
        }
    }

    @Test
    fun exitHandshakeUnbindsOnlyOnConfirm() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertTrue(s.udp.sent.any { headerOf(it.bytes) == MANUAL_TAKE_HEADER })

            s.vm.requestExit()
            main.runCurrent()
            assertEquals(true, s.vm.state.value.pendingExit)
            assertTrue(
                s.udp.sent.none { headerOf(it.bytes) == MANUAL_RELEASE_HEADER },
                "request alone never releases",
            )

            s.vm.dismissExit()
            assertFalse(s.vm.state.value.pendingExit)
            assertTrue(s.udp.sent.none { headerOf(it.bytes) == MANUAL_RELEASE_HEADER })

            s.vm.requestExit()
            s.vm.confirmExit()
            main.runCurrent()
            assertFalse(s.vm.state.value.pendingExit)
            val releases = s.udp.sent.filter { headerOf(it.bytes) == MANUAL_RELEASE_HEADER }
            assertEquals(2, releases.size, "confirm unbinds: best-effort 2x release")

            s.udp.sent.clear()
            main.advanceTimeBy(1000)
            val drives = s.udp.sent.filter { headerOf(it.bytes) == DRIVE_HEADER }
            assertTrue(drives.isNotEmpty(), "idle heartbeat preserved after confirm")
            assertTrue(drives.size < 50, "idle throttles below full rate")
            val zeroed = ControlCommand(RobotMode.MANUAL, 0f, 0f)
            val psk = hexToBytes(PSK_HEX)
            drives.forEach { frame ->
                assertTrue(
                    frame.bytes.contentEquals(
                        encodeSigned(zeroed, counterOf(frame.bytes, DRIVE_COUNTER_AT), psk),
                    ),
                    "idle frames zeroed after confirm",
                )
            }
        } finally {
            s.manager.stop()
            s.drive.stop()
        }
    }
}
