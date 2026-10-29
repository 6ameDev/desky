package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.EXPR_TRIGGER_HEADER
import com.desky.shared.domain.EXPR_TRIGGER_SIZE
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.ROBOT_HOST
import com.desky.shared.domain.PICKER_EXPRESSIONS
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.ToRobotCounter
import com.desky.shared.domain.TriggerExpressionUseCase
import com.desky.shared.domain.UdpDatagram
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val BYTE_MASK = 0xFF

private fun Byte.unsigned(): Int = this.toInt() and BYTE_MASK

@OptIn(ExperimentalCoroutinesApi::class)
class ExpressionsViewModelTest {

    private companion object {
        const val TEST_TICK_MS = 20L
        const val PICKER_STABILITY_ADVANCE_MS = 2000L
        const val LEGACY_GRAPH_SETTLE_MS = 500L
        const val BLINK_EXPRESSION_ID = 3
        const val ALERT_EXPRESSION_ID = 4
        const val UNKNOWN_EXPRESSION_ID = 5
    }

    private data class Setup(
        val vm: RobotControlViewModel,
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val holder: ToRobotCounter,
    )

    private fun setup(tickMs: Long = TEST_TICK_MS): Setup {
        val repo = FakeDeviceRepository(testDevice())
        val udp = FakeUdpSocketManager()
        val vault = FakePairSecretStore().apply { seed(testDevice().id, PSK_HEX) }
        val holder = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val sendSigned = SendSignedControlUseCase(udp)
        val vm = RobotControlViewModel(
            sendSigned,
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(sendSigned),
            repo,
            GetPairPskUseCase(vault, repo),
            udp,
            holder,
            fromRobot,
            tickMs,
            socketBinder = FakeUdpSocketBinder(),
            triggerExpressionUC = TriggerExpressionUseCase(udp),
        )
        return Setup(vm, repo, udp, holder)
    }

    private suspend fun promote(udp: FakeUdpSocketManager, manual: Boolean = true) {
        val packet = Telemetry(
            pitchDeg = 0f,
            rollDeg = 0f,
            distanceMm = 900,
            cliff = false,
            driving = false,
            mode = RobotMode.MANUAL,
            manual = manual,
        )
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), ROBOT_HOST, DEFAULT_UDP_PORT))
    }

    private fun trigs(udp: FakeUdpSocketManager) =
        udp.sent.filter { (it.bytes[0].unsigned()) == EXPR_TRIGGER_HEADER }

    @Test
    fun pickerListsStaticCatalogSynchronously() = runVmTest { main ->
        val s = setup()
        // No fetch, no wait: the hardcoded picker is in state from init.
        assertEquals(PICKER_EXPRESSIONS, s.vm.state.value.expressions)
        assertEquals(
            listOf("Neutral", "happy", "sad", "curious", "surprised", "angry", "scared", "excited", "confused"),
            s.vm.state.value.expressions.map { it.name },
        )
        assertTrue(s.vm.state.value.expressions.none { it.id == BLINK_EXPRESSION_ID || it.id == ALERT_EXPRESSION_ID }, "blink/alert left the picker")

        main.advanceTimeBy(PICKER_STABILITY_ADVANCE_MS)
        main.runCurrent()
        assertEquals(PICKER_EXPRESSIONS, s.vm.state.value.expressions)
        assertTrue(trigs(s.udp).isEmpty(), "listing never transmits")
    }

    @Test
    fun triggerSendsOnceWhileManual() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        promote(s.udp, manual = true)
        main.runCurrent()
        assertTrue(s.vm.state.value.manualMode)

        // Blink left the picker but validation stays open: still triggers.
        s.vm.onTriggerExpression(BLINK_EXPRESSION_ID)
        main.runCurrent()
        main.runCurrent()
        val sent = trigs(s.udp)
        assertEquals(1, sent.size, "single send per tap")
        val frame = sent[0].bytes
        assertEquals(EXPR_TRIGGER_SIZE, frame.size)
        assertEquals(EXPR_TRIGGER_HEADER, frame[0].unsigned())
        assertEquals(BLINK_EXPRESSION_ID, frame[1].unsigned())
        assertEquals(ROBOT_HOST, sent[0].host)
        assertEquals(DEFAULT_UDP_PORT, sent[0].port)
    }

    @Test
    fun triggerGatedOutsideManual() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        // Linked but MANUAL not reported: the picker is inert.
        promote(s.udp, manual = false)
        main.runCurrent()
        assertEquals(Connectivity.Connected, s.vm.state.value.connectivity)
        assertFalse(s.vm.state.value.manualMode)

        s.vm.onTriggerExpression(BLINK_EXPRESSION_ID)
        main.runCurrent()
        main.runCurrent()
        assertTrue(trigs(s.udp).isEmpty(), "no trigger without the MANUAL hold")
    }

    @Test
    fun triggerGatedWhileDisconnected() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        assertEquals(Connectivity.Disconnected, s.vm.state.value.connectivity)

        s.vm.onTriggerExpression(BLINK_EXPRESSION_ID)
        main.runCurrent()
        main.runCurrent()
        assertTrue(trigs(s.udp).isEmpty(), "no trigger while link-down")
    }

    @Test
    fun triggerUnknownIdRejectedSilently() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        promote(s.udp, manual = true)
        main.runCurrent()

        s.vm.onTriggerExpression(UNKNOWN_EXPRESSION_ID)
        main.runCurrent()
        main.runCurrent()
        assertTrue(trigs(s.udp).isEmpty(), "stale taps never reach the wire")
        assertNull(s.vm.state.value.error)
    }

    @Test
    fun legacyGraphWithoutTriggerListsStaticButSendsNothing() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice())
        val udp = FakeUdpSocketManager()
        val vault = FakePairSecretStore().apply { seed(testDevice().id, PSK_HEX) }
        val sendSigned = SendSignedControlUseCase(udp)
        val vm = RobotControlViewModel(
            sendSigned,
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(sendSigned),
            repo,
            GetPairPskUseCase(vault, repo),
            udp,
            ToRobotCounter(repo),
            FromRobotCounter(repo),
            TEST_TICK_MS,
            socketBinder = FakeUdpSocketBinder(),
        )
        main.runCurrent()
        main.advanceTimeBy(LEGACY_GRAPH_SETTLE_MS)
        main.runCurrent()
        assertEquals(PICKER_EXPRESSIONS, vm.state.value.expressions)
        vm.onTriggerExpression(BLINK_EXPRESSION_ID)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty())
    }
}
