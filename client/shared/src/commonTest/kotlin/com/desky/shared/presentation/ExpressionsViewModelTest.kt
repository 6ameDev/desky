package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.EXPR_TRIGGER_HEADER
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.RobotMode
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

private fun Byte.unsigned(): Int = this.toInt() and 0xFF

@OptIn(ExperimentalCoroutinesApi::class)
class ExpressionsViewModelTest {

    private data class Setup(
        val vm: RobotControlViewModel,
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val holder: ToRobotCounter,
    )

    private fun setup(tickMs: Long = 20): Setup {
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
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), "192.168.4.1", DEFAULT_UDP_PORT))
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
        assertTrue(s.vm.state.value.expressions.none { it.id == 3 || it.id == 4 }, "blink/alert left the picker")

        main.advanceTimeBy(2000)
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
        s.vm.onTriggerExpression(3)
        main.runCurrent()
        main.runCurrent()
        val sent = trigs(s.udp)
        assertEquals(1, sent.size, "single send per tap")
        val frame = sent[0].bytes
        assertEquals(15, frame.size)
        assertEquals(0x5B, frame[0].unsigned())
        assertEquals(3, frame[1].unsigned())
        assertEquals("192.168.4.1", sent[0].host)
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

        s.vm.onTriggerExpression(3)
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

        s.vm.onTriggerExpression(3)
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

        s.vm.onTriggerExpression(5)
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
            20,
            socketBinder = FakeUdpSocketBinder(),
        )
        main.runCurrent()
        main.advanceTimeBy(500)
        main.runCurrent()
        assertEquals(PICKER_EXPRESSIONS, vm.state.value.expressions)
        vm.onTriggerExpression(3)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty())
    }
}
