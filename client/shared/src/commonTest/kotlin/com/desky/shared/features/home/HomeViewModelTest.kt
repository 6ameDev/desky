package com.desky.shared.features.home

import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.UdpDatagram
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.ConnectionManager
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
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
import kotlin.test.assertNull

private const val PSK_HEX = "00112233445566778899aabbccddeeff"

@OptIn(ExperimentalCoroutinesApi::class)
class HomeViewModelTest {

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
        val manager: ConnectionManager,
        val vm: HomeViewModel,
    )

    private fun CoroutineScope.setup(device: PairedDevice? = null): Setup {
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
        return Setup(repo, udp, manager, HomeViewModel(repo, manager))
    }

    private fun telemetryDatagram(manual: Boolean = false) = UdpDatagram(
        TelemetryPacketCodec.encode(
            Telemetry(1f, 2f, 3, cliff = false, driving = false, mode = RobotMode.MANUAL, manual = manual),
        ),
        ROBOT_HOST,
        DEFAULT_UDP_PORT,
    )

    @Test
    fun startsUnpairedWithStaticEntries() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            assertNull(s.vm.state.value.pairedDevice)
            assertEquals(Aliveness.Unpaired, s.vm.state.value.aliveness)
            assertEquals(listOf(HomeEntry.Drive, HomeEntry.Expressions, HomeEntry.Settings), s.vm.state.value.entries)
        } finally {
            s.manager.stop()
        }
    }

    @Test
    fun collectsPairedDeviceAndAliveness() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            s.repo.emit(pairedDevice())
            main.runCurrent()
            assertEquals(pairedDevice(), s.vm.state.value.pairedDevice)
            assertEquals(Aliveness.Stale, s.vm.state.value.aliveness)

            s.udp.inbound.emit(telemetryDatagram())
            main.runCurrent()
            assertEquals(Aliveness.Connected, s.vm.state.value.aliveness)

            s.repo.emit(null)
            main.runCurrent()
            assertNull(s.vm.state.value.pairedDevice)
            assertEquals(Aliveness.Unpaired, s.vm.state.value.aliveness)
        } finally {
            s.manager.stop()
        }
    }
}
