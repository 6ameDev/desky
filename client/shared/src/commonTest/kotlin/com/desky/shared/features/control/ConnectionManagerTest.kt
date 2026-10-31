package com.desky.shared.features.control

import app.cash.turbine.test
import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.encodeDisconnect
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
import com.desky.shared.features.control.PairRefreshUseCase
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.features.session.SessionManager
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val BYTE_MASK = 0xFF
private const val ALIVE_HEADER = 0x55
private const val DISCONNECT_HEADER = 0x56
private const val PAIR_REFRESH_REQ_HEADER = 0x53

private fun pairedDevice() = PairedDevice(
    id = "desky-$ROBOT_HOST",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "token",
    era = 0u,
)

private fun headerOf(bytes: ByteArray) = bytes[0].toInt() and BYTE_MASK

class ConnectionManagerTest {

    private data class Setup(
        val repo: FakeDeviceRepository,
        val vault: FakePairSecretStore,
        val udp: FakeUdpSocketManager,
        val toRobot: ToRobotCounter,
        val manager: ConnectionManager,
        val sessions: SessionManager,
    )

    private fun CoroutineScope.setup(
        device: PairedDevice? = pairedDevice(),
        withPairRefresh: Boolean = false,
        isRefreshDue: suspend () -> Boolean = { false },
    ): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(pairedDevice().id, PSK_HEX) }
        val udp = FakeUdpSocketManager()
        val toRobot = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val getPsk = GetPairPskUseCase(vault, repo)
        val sessions = SessionManager(repo, getPsk, toRobot, ForgetDeviceUseCase(repo, vault, udp, getPsk, toRobot, fromRobot), this)
        val pairRefreshUC =
            if (withPairRefresh) PairRefreshUseCase(repo, vault, udp, getPsk, toRobot, fromRobot) else null
        val manager = ConnectionManager(
            deviceRepo = repo,
            sessionManager = sessions,
            observeTelemetry = ObserveTelemetryUseCase(udp),
            toRobotCounter = toRobot,
            aliveUC = AliveUseCase(udp),
            remoteDisconnectUC = ObserveRemoteDisconnectUseCase(udp, repo, getPsk, vault, toRobot, fromRobot),
            udp = udp,
            scope = this,
            pairRefreshUC = pairRefreshUC,
            aliveIntervalMs = 100,
            staleTimeoutMs = 100,
            deadTimeoutMs = 300,
            tickMs = 20,
            isRefreshDue = isRefreshDue,
        )
        return Setup(repo, vault, udp, toRobot, manager, sessions)
    }

    private fun telemetryDatagram() = UdpDatagram(
        TelemetryPacketCodec.encode(
            Telemetry(1f, 2f, 3, cliff = false, driving = true, mode = RobotMode.MANUAL),
        ),
        ROBOT_HOST,
        DEFAULT_UDP_PORT,
    )

    @Test
    fun aliveSentOnCadenceWhilePaired() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            val firstBurst = s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER }
            assertEquals(1, firstBurst)

            testScheduler.advanceTimeBy(550)
            val total = s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER }
            assertEquals(6, total)
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun noAliveWhenUnpairedThenKnocksOnPair() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup(device = null)
        try {
            testScheduler.advanceTimeBy(1000)
            assertTrue(s.udp.sent.isEmpty())
            assertEquals(Aliveness.Unpaired, s.manager.aliveness.value)

            s.repo.emit(pairedDevice())
            runCurrent()
            assertEquals(Aliveness.Stale, s.manager.aliveness.value)
            testScheduler.advanceTimeBy(200)
            assertTrue(s.udp.sent.any { headerOf(it.bytes) == ALIVE_HEADER })
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun watchdogConnectedStaleDeadOnSilence() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            assertEquals(Aliveness.Stale, s.manager.aliveness.value)

            s.udp.inbound.emit(telemetryDatagram())
            runCurrent()
            assertEquals(Aliveness.Connected, s.manager.aliveness.value)

            testScheduler.advanceTimeBy(110)
            assertEquals(Aliveness.Stale, s.manager.aliveness.value)

            testScheduler.advanceTimeBy(210)
            assertEquals(Aliveness.Dead, s.manager.aliveness.value)

            s.udp.inbound.emit(telemetryDatagram())
            runCurrent()
            assertEquals(Aliveness.Connected, s.manager.aliveness.value)
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun remoteDisconnectClearsPairAndStopsAlive() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice()
        val s = scope.setup(device = device)
        try {
            runCurrent()
            val sentBefore = s.udp.sent.size
            assertTrue(sentBefore > 0)

            s.udp.inbound.emit(UdpDatagram(encodeDisconnect(hexToBytes(PSK_HEX), 0u, 1u), ROBOT_HOST, DEFAULT_UDP_PORT))
            runCurrent()
            assertEquals(null, s.repo.observePaired().first())
            assertEquals(Aliveness.Unpaired, s.manager.aliveness.value)

            testScheduler.advanceTimeBy(500)
            assertEquals(sentBefore, s.udp.sent.size)
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun alivenessReplayedToLateSubscriber() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.udp.inbound.emit(telemetryDatagram())
            runCurrent()
            assertEquals(Aliveness.Connected, s.manager.aliveness.value)

            s.manager.aliveness.test {
                assertEquals(Aliveness.Connected, awaitItem())
                cancelAndIgnoreRemainingEvents()
            }
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun pairRefreshTriggeredWhenDue() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup(withPairRefresh = true, isRefreshDue = { true })
        try {
            runCurrent()

            testScheduler.advanceTimeBy(3500)
            val reqs = s.udp.sent.count { headerOf(it.bytes) == PAIR_REFRESH_REQ_HEADER }
            assertEquals(3, reqs)
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun backgroundSendsGoodbyeAndFreezesLoop() = runTest {        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            assertEquals(1, s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER })
            s.udp.inbound.emit(telemetryDatagram())
            runCurrent()
            assertEquals(Aliveness.Connected, s.manager.aliveness.value)

            s.manager.onBackground()
            runCurrent()
            testScheduler.advanceTimeBy(500)

            val disconnects = s.udp.sent.filter { headerOf(it.bytes) == DISCONNECT_HEADER }
            assertEquals(2, disconnects.size, "exactly one goodbye, sent twice byte-identical")
            assertContentEquals(disconnects[0].bytes, disconnects[1].bytes)
            assertEquals(
                1,
                s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER },
                "no ALIVE while backgrounded",
            )
            assertEquals(
                Aliveness.Connected,
                s.manager.aliveness.value,
                "watchdog frozen instead of demoted past the dead mark",
            )
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun backgroundFlushesPersistedCounter() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            assertTrue(s.repo.counterSaves.isEmpty(), "no persist before the cadence")
            s.manager.onBackground()
            runCurrent()
            runCurrent()
            assertTrue(s.repo.counterSaves.isNotEmpty(), "background persists the live send counter")
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun backgroundEvictsPeerResolutions() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.manager.onBackground()
            runCurrent()
            runCurrent()
            assertEquals(1, s.udp.evictCalls, "background drops cached DNS so return re-resolves")
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun foregroundKnocksAliveImmediately() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup()
        try {
            runCurrent()
            s.manager.onBackground()
            runCurrent()
            testScheduler.advanceTimeBy(500)
            assertEquals(1, s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER })

            s.manager.onForeground()
            runCurrent()
            testScheduler.advanceTimeBy(20)
            assertEquals(2, s.udp.sent.count { headerOf(it.bytes) == ALIVE_HEADER })
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }

    @Test
    fun backgroundWhenUnpairedIsNoOp() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val s = scope.setup(device = null)
        try {
            s.manager.onBackground()
            runCurrent()
            testScheduler.advanceTimeBy(500)
            assertTrue(s.udp.sent.isEmpty())

            s.manager.onForeground()
            runCurrent()
            testScheduler.advanceTimeBy(500)
            assertTrue(s.udp.sent.isEmpty())
            assertEquals(Aliveness.Unpaired, s.manager.aliveness.value)
        } finally {
            s.manager.stop(); s.sessions.stop()
        }
    }
}
