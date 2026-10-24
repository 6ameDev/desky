package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.EXPR_LIST_REQ_HEADER
import com.desky.shared.domain.EXPR_TRIGGER_HEADER
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ExpressionEntry
import com.desky.shared.domain.ExprListRespCodec
import com.desky.shared.domain.FetchExpressionsUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.ToRobotCounter
import com.desky.shared.domain.TriggerExpressionUseCase
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.hexToBytes
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

    private fun setup(
        tickMs: Long = 20,
        fetchTimeoutMs: Long = 200,
    ): Setup {
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
            fetchExpressionsUC = FetchExpressionsUseCase(udp, fetchTimeoutMs),
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

    private fun reqs(udp: FakeUdpSocketManager) =
        udp.sent.filter { (it.bytes[0].unsigned()) == EXPR_LIST_REQ_HEADER }

    private fun trigs(udp: FakeUdpSocketManager) =
        udp.sent.filter { (it.bytes[0].unsigned()) == EXPR_TRIGGER_HEADER }

    private val catalog = listOf(
        ExpressionEntry(id = 3, flags = 1, name = "Wave"),
        ExpressionEntry(id = 7, flags = 0, name = "Nod"),
    )

    private suspend fun answerFetch(s: Setup) {
        s.udp.inbound.emit(
            UdpDatagram(
                ExprListRespCodec.encode(1, catalog),
                "192.168.4.1",
                DEFAULT_UDP_PORT,
            ),
        )
    }

    @Test
    fun fetchOnEntryCachesCatalogWithSingleSend() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        assertEquals(1, reqs(s.udp).size, "one EXPR_LIST_REQ on screen entry")
        assertEquals(14, reqs(s.udp)[0].bytes.size)
        assertTrue(s.vm.state.value.expressionsLoading)

        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)
        assertFalse(s.vm.state.value.expressionsLoading)
        assertEquals(1, reqs(s.udp).size, "no retry while the entry fetch is answered")
    }

    @Test
    fun cacheSurvivesTicksWithoutRefetch() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)

        // Many idle ticks: the lifecycle cache holds, no refetch fires.
        main.advanceTimeBy(2000)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)
        assertEquals(1, reqs(s.udp).size, "cached for the lifecycle until return")
    }

    @Test
    fun refetchOnReturn() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        answerFetch(s)
        main.runCurrent()
        assertEquals(1, reqs(s.udp).size)

        // Leaving and returning to the screen refetches once (heals loss).
        s.vm.onForeground()
        main.advanceTimeBy(21)
        main.runCurrent()
        main.runCurrent()
        assertEquals(2, reqs(s.udp).size, "return refetches exactly once")
        assertTrue(s.vm.state.value.expressionsLoading)
        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)
        assertFalse(s.vm.state.value.expressionsLoading)
    }

    @Test
    fun failedFetchStaysSilentAndKeepsCache() = runVmTest { main ->
        val s = setup(fetchTimeoutMs = 200)
        main.runCurrent()
        main.runCurrent()
        assertEquals(1, reqs(s.udp).size)

        // No answer: the wait times out silently — no error banner, no crash.
        main.advanceTimeBy(500)
        main.runCurrent()
        assertTrue(s.vm.state.value.expressions.isEmpty())
        assertFalse(s.vm.state.value.expressionsLoading)
        assertNull(s.vm.state.value.error, "fetch loss never touches the error banner")
        assertEquals(1, reqs(s.udp).size, "single send per entry, no retry")
    }

    @Test
    fun triggerSendsOnceWhileManual() = runVmTest { main ->
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        promote(s.udp, manual = true)
        main.runCurrent()
        assertTrue(s.vm.state.value.manualMode)
        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)

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
        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)

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
        answerFetch(s)
        main.runCurrent()
        assertEquals(catalog, s.vm.state.value.expressions)
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
        answerFetch(s)
        main.runCurrent()

        s.vm.onTriggerExpression(9)
        main.runCurrent()
        main.runCurrent()
        assertTrue(trigs(s.udp).isEmpty(), "stale taps never reach the wire")
        assertNull(s.vm.state.value.error)
    }

    @Test
    fun unpairThenRepairRefetchesFresh() = runVmTest { main ->
        val device = testDevice()
        val s = setup()
        main.runCurrent()
        main.runCurrent()
        answerFetch(s)
        main.runCurrent()
        assertEquals(1, reqs(s.udp).size)

        // Unpair pauses; a repair fetches the (possibly new) catalog again.
        s.repo.emit(null)
        main.advanceTimeBy(21)
        main.runCurrent()
        s.repo.emit(device)
        main.advanceTimeBy(21)
        main.runCurrent()
        main.runCurrent()
        assertEquals(2, reqs(s.udp).size, "repair refetches on the next session")
    }

    @Test
    fun legacyGraphWithoutUseCasesLeavesPickerEmpty() = runVmTest { main ->
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
        assertTrue(vm.state.value.expressions.isEmpty())
        assertFalse(vm.state.value.expressionsLoading)
        vm.onTriggerExpression(3)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty())
    }
}
