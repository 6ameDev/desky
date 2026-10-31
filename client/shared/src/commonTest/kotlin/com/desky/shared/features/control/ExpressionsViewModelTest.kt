package com.desky.shared.features.control

import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.EXPR_TRIGGER_HEADER
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
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
import com.desky.shared.features.control.PICKER_EXPRESSIONS
import com.desky.shared.features.control.TakeManualUseCase
import com.desky.shared.features.control.TriggerExpressionUseCase
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.features.session.SessionManager
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import com.desky.shared.presentation.runVmTest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val BYTE_MASK = 0xFF
private const val HAPPY_EXPRESSION_ID = 1
private const val UNKNOWN_EXPRESSION_ID = 5

@OptIn(ExperimentalCoroutinesApi::class)
class ExpressionsViewModelTest {

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
        val sessions: SessionManager,
        val vm: ExpressionsViewModel,
    )

    private fun CoroutineScope.setup(
        device: PairedDevice? = pairedDevice(),
        withTrigger: Boolean = true,
    ): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(pairedDevice().id, PSK_HEX) }
        val udp = FakeUdpSocketManager()
        val toRobot = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val getPsk = GetPairPskUseCase(vault, repo)
        val sessions = SessionManager(repo, getPsk, toRobot, ForgetDeviceUseCase(repo, vault, udp, getPsk, toRobot, fromRobot), this)
        val manager = ConnectionManager(
            deviceRepo = repo,
            sessionManager = sessions,
            observeTelemetry = ObserveTelemetryUseCase(udp),
            toRobotCounter = toRobot,
            aliveUC = AliveUseCase(udp),
            remoteDisconnectUC = ObserveRemoteDisconnectUseCase(udp, repo, getPsk, vault, toRobot, fromRobot),
            udp = udp,
            scope = this,
            aliveIntervalMs = 100,
            staleTimeoutMs = 100,
            deadTimeoutMs = 300,
            tickMs = 20,
        )
        val vm = ExpressionsViewModel(
            repo,
            sessions,
            if (withTrigger) TriggerExpressionUseCase(udp) else null,
            TakeManualUseCase(udp),
            ReleaseManualUseCase(udp),
            manager,
            TelemetryHub(udp, this),
        )
        return Setup(repo, udp, manager, sessions, vm)
    }

    private fun telemetryDatagram(manual: Boolean) = UdpDatagram(
        TelemetryPacketCodec.encode(
            Telemetry(1f, 2f, 3, cliff = false, driving = false, mode = RobotMode.MANUAL, manual = manual),
        ),
        ROBOT_HOST,
        DEFAULT_UDP_PORT,
    )

    private fun trigs(udp: FakeUdpSocketManager) =
        udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == EXPR_TRIGGER_HEADER }

    private suspend fun promote(main: kotlinx.coroutines.test.TestCoroutineScheduler, s: Setup, manual: Boolean) {
        main.runCurrent()
        s.udp.inbound.emit(telemetryDatagram(manual))
        main.runCurrent()
    }

    @Test
    fun listsPickerAndCollectsAliveness() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            assertEquals(PICKER_EXPRESSIONS, s.vm.state.value.expressions)
            main.runCurrent()
            assertEquals(Aliveness.Stale, s.vm.state.value.aliveness)

            s.udp.inbound.emit(telemetryDatagram(manual = true))
            main.runCurrent()
            assertEquals(Aliveness.Connected, s.vm.state.value.aliveness)
            assertEquals(true, s.vm.state.value.telemetry?.manual)
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun triggerSendsOnceWhileManual() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            promote(main, s, manual = true)

            s.vm.onTriggerExpression(HAPPY_EXPRESSION_ID)
            main.runCurrent()
            main.runCurrent()
            val sent = trigs(s.udp)
            assertEquals(1, sent.size, "single send per tap")
            assertEquals(HAPPY_EXPRESSION_ID, sent[0].bytes[1].toInt() and BYTE_MASK)
            assertEquals(HAPPY_EXPRESSION_ID, s.vm.state.value.lastTriggeredId)
            assertNull(s.vm.state.value.error)
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun triggerGatedOutsideManual() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            promote(main, s, manual = false)
            assertEquals(Aliveness.Connected, s.vm.state.value.aliveness)

            s.vm.onTriggerExpression(HAPPY_EXPRESSION_ID)
            main.runCurrent()
            main.runCurrent()
            assertTrue(trigs(s.udp).isEmpty(), "no trigger without the MANUAL hold")
            assertNull(s.vm.state.value.lastTriggeredId)
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun triggerGatedWhileLinkDown() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            assertEquals(Aliveness.Stale, s.vm.state.value.aliveness)

            s.vm.onTriggerExpression(HAPPY_EXPRESSION_ID)
            main.runCurrent()
            main.runCurrent()
            assertTrue(trigs(s.udp).isEmpty(), "no trigger while link-down")
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun triggerUnknownIdRejectedSilently() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            promote(main, s, manual = true)

            s.vm.onTriggerExpression(UNKNOWN_EXPRESSION_ID)
            main.runCurrent()
            main.runCurrent()
            assertTrue(trigs(s.udp).isEmpty(), "stale taps never reach the wire")
            assertNull(s.vm.state.value.error)
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun triggerFailureSurfacesError() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            promote(main, s, manual = true)

            s.udp.sendError = IllegalStateException("dead socket")
            s.vm.onTriggerExpression(HAPPY_EXPRESSION_ID)
            main.runCurrent()
            main.runCurrent()
            assertTrue(trigs(s.udp).isEmpty())
            assertNotNull(s.vm.state.value.error)
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun exitHandshakeTransitions() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            assertFalse(s.vm.state.value.pendingExit)

            s.vm.requestExit()
            assertTrue(s.vm.state.value.pendingExit)

            s.vm.dismissExit()
            assertFalse(s.vm.state.value.pendingExit)

            s.vm.requestExit()
            s.vm.confirmExit()
            main.runCurrent()
            assertFalse(s.vm.state.value.pendingExit)
            assertTrue(trigs(s.udp).isEmpty(), "exit never transmits")
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun activationTakesManualOnce() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            main.runCurrent()
            assertTrue(takes(s.udp).isEmpty(), "no take before entry")

            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(1, takes(s.udp).size, "activation takes MANUAL exactly once")

            s.vm.activate()
            main.runCurrent()
            assertEquals(1, takes(s.udp).size, "repeat activation while entered is a no-op")

            s.vm.confirmExit()
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(2, takes(s.udp).size, "re-activation after exit takes again")
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    /**
     * Background round-trip without navigation: deactivation re-arms and
     * the next activation takes MANUAL again.
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
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(2, takes(s.udp).size, "return re-takes MANUAL")
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    @Test
    fun exitReleasesManualAfterTake() = runVmTest { main ->
        val scope = CoroutineScope(StandardTestDispatcher(main))
        val s = scope.setup()
        try {
            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(1, takes(s.udp).size, "enter takes once")

            s.vm.deactivate()
            main.runCurrent()
            main.runCurrent()
            val releases = s.udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == MANUAL_RELEASE_HEADER }
            assertEquals(2, releases.size, "exit releases best-effort 2x")
            assertContentEquals(releases[0].bytes, releases[1].bytes, "byte-identical repeat: one allocation")

            s.vm.activate()
            main.runCurrent()
            main.runCurrent()
            assertEquals(2, takes(s.udp).size, "re-activation after exit takes again")
        } finally {
            s.manager.stop()
            s.sessions.stop()
        }
    }

    private fun takes(udp: FakeUdpSocketManager) =
        udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == MANUAL_TAKE_HEADER }
}
