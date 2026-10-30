package com.desky.shared.presentation

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.ControlPacketCodec
import com.desky.shared.core.ALIVE_HEADER
import com.desky.shared.core.AliveUseCase
import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.DISCONNECT_HEADER
import com.desky.shared.core.DRIVE_HEADER
import com.desky.shared.core.DRIVE_SIZE
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.ESTOP_FLAG
import com.desky.shared.core.EmergencyStopUseCase
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.core.UNPAIR_HEADER
import com.desky.shared.core.MANUAL_TAKE_HEADER
import com.desky.shared.core.MANUAL_RELEASE_HEADER
import com.desky.shared.core.encodeAlive
import com.desky.shared.core.encodeDisconnect
import com.desky.shared.core.encodeManualTake
import com.desky.shared.core.encodeManualRelease
import com.desky.shared.core.encodeSigned
import com.desky.shared.core.encodeUnpair
import com.desky.shared.core.ObserveTelemetryUseCase
import com.desky.shared.core.Outcome
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.ReleaseManualUseCase
import com.desky.shared.core.RobotMode
import com.desky.shared.core.SendSignedControlUseCase
import com.desky.shared.core.TakeManualUseCase
import com.desky.shared.core.Telemetry
import com.desky.shared.core.TelemetryPacketCodec
import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.hexToBytes
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"

@OptIn(ExperimentalCoroutinesApi::class)
class RobotControlViewModelTest {

    private companion object {
        const val BYTE_MASK = 0xFF
        const val TEST_TICK_MS = 20L
        const val ONE_TICK_WINDOW_MS = 21L
        const val THREE_TICK_WINDOW_MS = 61L
        const val FIVE_TICK_WINDOW_MS = 101L
        const val TEN_TICK_WINDOW_MS = 201L
        const val MID_INTERVAL_MS = 200L
        const val HALF_SECOND_WINDOW_MS = 500L
        const val HALF_SECOND_WINDOW_PLUS_ONE_MS = 501L
        const val IDLE_WINDOW_MS = 1000L
        const val LONG_SILENCE_WINDOW_MS = 2500L
        const val CHUNK_WINDOW_MS = 981L
        const val SHORT_CHUNK_WINDOW_MS = 599L
        const val LONG_CHUNK_WINDOW_MS = 1019L
        const val BURST_SPACING_MS = 66L
        const val FULL_RATE_SENDS = 5
        const val FLAP_BURST_COUNT = 5
        const val REDUNDANT_INPUT_COUNT = 20
        const val NOISE_SPAM_COUNT = 10
        const val IDLE_WINDOW_SENDS = 12
        const val POST_RECENTER_SENDS = 13
        const val CADENCE_FRAME_TOTAL = 128
        const val DRIVE_FRAME_TOTAL = 149
        const val DRIVE_PLUS_UNPAIR_TOTAL = 152
        const val INITIAL_GRACE_SENDS = 8
        const val ESTOP_LOOP_SENDS = 3
        const val GOODBYE_SEND_TOTAL = 3
        const val EXPECTED_ALIVE_TOTAL = 3
        const val RESUME_ALIVE_INDEX = 3
        const val RESUME_DRIVE_INDEX = 4
        const val MIN_ALIVE_KNOCKS = 4
        const val BURST_PACKET_COUNT = 9
        const val MAX_BURST_UI_UPDATES = 4
        const val DEFLECTED_STICK = 0.5f
        const val TEST_STEERING = 0.25f
        const val STICK_NOISE_TINY = 0.005f
        const val STICK_NOISE_SMALL = 0.01f
        const val STICK_NOISE_LARGE = 0.015f
        const val ESTOP_LATCH_DEFLECTION = 0.8f
        const val RECONNECT_DEFLECTION = 0.9f
        const val OVER_RANGE_STICK = 9f
        const val BURST_BASE_PITCH = 10f
    }

    private fun psk() = hexToBytes(PSK_HEX)

    private fun setup(
        repo: FakeDeviceRepository = FakeDeviceRepository(testDevice()),
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        vault: FakePairSecretStore = FakePairSecretStore().apply { seed(testDevice().id, PSK_HEX) },
        tickMs: Long = 20,
        holder: ToRobotCounter = ToRobotCounter(repo),
        fromRobot: FromRobotCounter = FromRobotCounter(repo),
        useAlive: Boolean = false,
        binder: FakeUdpSocketBinder = FakeUdpSocketBinder(),
        wireForget: Boolean = false,
        wireManual: Boolean = false,
    ): Quad {
        val sendSigned = SendSignedControlUseCase(udp)
        val getPsk = GetPairPskUseCase(vault, repo)
        val forgetUC = if (wireForget) ForgetDeviceUseCase(repo, vault, udp, getPsk, holder, fromRobot) else null
        val viewModel = RobotControlViewModel(
            sendSigned,
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(sendSigned),
            repo,
            getPsk,
            udp,
            holder,
            fromRobot,
            tickMs,
            aliveUC = if (useAlive) AliveUseCase(udp) else null,
            socketBinder = binder,
            forgetUC = forgetUC,
            takeManualUC = if (wireManual) TakeManualUseCase(udp) else null,
            releaseManualUC = if (wireManual) ReleaseManualUseCase(udp) else null,
        )
        return Quad(viewModel, repo, udp, vault, holder, fromRobot, binder)
    }

    /**
     * Promotes the link to [Connectivity.Connected] with a neutral MANUAL
     * packet. Required before any user intent: sticks/e-stop/release are
     * gated to no-ops while [Connectivity.Disconnected]. Emits + pumps but
     * advances no virtual time, so the 2 s silence budget restarts here.
     * [manual] carries the telemetry MANUAL hold bit: true (default) makes
     * the drive loop transmit; false promotes the link while keeping drive
     * gated (paired/linked but not drivable).
     */
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

    private data class Quad(
        val vm: RobotControlViewModel,
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val vault: FakePairSecretStore,
        val holder: ToRobotCounter,
        val fromRobot: FromRobotCounter = FromRobotCounter(repo),
        val binder: FakeUdpSocketBinder = FakeUdpSocketBinder(),
    )

    @Test
    fun stickUpdatesStateClamped() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup()
        main.runCurrent()
        promote(udp)
        main.runCurrent()

        viewModel.onStick(DEFLECTED_STICK, -TEST_STEERING)
        assertEquals(DEFLECTED_STICK, viewModel.state.value.throttle)
        assertEquals(-TEST_STEERING, viewModel.state.value.steering)

        viewModel.onStick(OVER_RANGE_STICK, -OVER_RANGE_STICK)
        assertEquals(1f, viewModel.state.value.throttle)
        assertEquals(-1f, viewModel.state.value.steering)
    }

    @Test
    fun sendLoopHeartbeatsSignedWithIncreasingCounters() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        // Pre-MANUAL the loop only probes session health (no counter
        // allocated, nothing transmitted): paired no longer implies drivable.
        assertTrue(udp.sent.isEmpty(), "no drive sends before MANUAL is reported")
        // Sticks are gated while Disconnected: promote first (with the MANUAL
        // hold, so the loop transmits from the next tick).
        promote(udp)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "promotion alone sends nothing; the loop samples")
        viewModel.onStick(DEFLECTED_STICK, -TEST_STEERING)
        // +1 past the last tick: advanceTimeBy() does NOT run tasks due
        // exactly at the end of the window (exclusive boundary).
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)

        // Sends at t=20,40,60,80,100 (the pre-MANUAL tick allocates nothing,
        // so the first drive still carries counter 1).
        assertEquals(FULL_RATE_SENDS, udp.sent.size)
        // Every frame is a 16B DRIVE datagram with counters 1..5.
        udp.sent.forEachIndexed { i, frame ->
            assertEquals(DRIVE_SIZE, frame.bytes.size, "frame $i")
            assertEquals(DRIVE_HEADER, frame.bytes[0].toInt() and BYTE_MASK, "frame $i")
        }
        assertEquals(ROBOT_HOST, udp.sent[0].host)
        assertEquals(DEFAULT_UDP_PORT, udp.sent[0].port)
        val last = udp.sent.last()
        assertTrue(
            last.bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, -TEST_STEERING, 0), 5u, psk()),
            ),
        )
    }

    @Test
    fun vaultReadOncePerSession() = runVmTest { main ->
        val (_, _, udp, vault) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        // Pre-MANUAL the gated tick probes (one vault read, session cached)
        // but transmits nothing.
        assertTrue(udp.sent.isEmpty())
        assertEquals(1, vault.loadCalls, "gated probe reads the vault once, session cached in memory")
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(TEN_TICK_WINDOW_MS)

        // Centered idle: grace (t=20..160, 8 sends, counters 1..8 — the gated
        // pre-MANUAL tick runs the idle clock but allocates nothing) — every
        // one from the single cached session load.
        assertEquals(INITIAL_GRACE_SENDS, udp.sent.size)
        assertEquals(1, vault.loadCalls, "vault read once, session cached in memory")
        main.advanceTimeBy(TEN_TICK_WINDOW_MS)
        assertEquals(1, vault.loadCalls)
    }

    /**
     * Idle heartbeat: centered sticks drop to ~5 Hz (one send per 10 ticks)
     * after the 200 ms activity grace, with gapless counters (skipped ticks
     * allocate nothing); any deflection or latched e-stop restores every-tick
     * sends immediately. 200 ms << 500 ms robot failsafe, so no safety loss.
     */
    @Test
    fun idleHeartbeatThrottlesAndActivityRestoresFullRate() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")
        // Gated while Disconnected: promote up front (safe here — clock is
        // t=0, no loop tick due, so the pump sends nothing extra).
        promote(udp)
        main.runCurrent()

        // Centered idle: grace (t=20..160, 8 sends — the gated pre-MANUAL tick
        // runs the idle clock but allocates nothing) then heartbeats at
        // t=360,560,760,960 — 12 sends in 50 ticks, counters 1..12 gapless.
        main.advanceTimeBy(IDLE_WINDOW_MS)
        assertEquals(IDLE_WINDOW_SENDS, udp.sent.size, "idle sends ~1 per 10 ticks")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 12u, psk()),
            ),
            "skipped ticks allocate no counters",
        )

        // Deflection resumes every tick immediately (the runCurrent also
        // fires the loop tick due exactly at the advanceTimeBy end boundary).
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "deflection sends every tick")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 18u, psk()),
            ),
        )

        // Recenter past the grace: 9-frame grace then heartbeats every 10th
        // tick — 13 sends in the next 50 ticks, counters 19..31 gapless.
        // Re-promote first: the window runs ~1 s past the last promote and
        // the 2 s silence budget must not demote mid-window (ticks would
        // gate drive while Disconnected).
        viewModel.onStick(0f, 0f)
        main.runCurrent()
        udp.sent.clear()
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(IDLE_WINDOW_MS)
        assertEquals(POST_RECENTER_SENDS, udp.sent.size, "recentered idle returns to ~1 per 10 ticks")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 31u, psk()),
            ),
        )

        // Latched e-stop forces every tick (centered + ESTOP_FLAG): the
        // immediate frame is counter 32, the loop sends 33,34,35.
        // ~1 s since the recenter re-promote: re-promote past the silence
        // budget first (safe — clock sits off-grid, no loop tick
        // due, so the pump sends nothing extra).
        promote(udp)
        main.runCurrent()
        viewModel.onEmergencyStop()
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(THREE_TICK_WINDOW_MS)
        assertEquals(ESTOP_LOOP_SENDS, udp.sent.size, "eStopped forces every tick")
        udp.sent.forEachIndexed { i, frame ->
            assertTrue(
                frame.bytes.contentEquals(
                    encodeSigned(
                        ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
                        (33u + i.toUInt()),
                        psk(),
                    ),
                ),
                "estop loop frame $i",
            )
        }
    }

    @Test
    fun counterPersistsEvery128Sends() = runVmTest { main ->
        val deviceId = testDevice().id
        // Deflected (full-rate): idle heartbeat would only send ~12 frames here.
        val (viewModel, repo, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        // Gated while Disconnected: promote first (pump init before the
        // emit — an emit before the first pump never reaches the collector).
        promote(udp)
        main.runCurrent()
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        // 128 frames total (the gated pre-MANUAL tick allocates nothing, then
        // t=20..2560): the 128th allocation write-aheads to 256. Chunked with
        // re-promotes: a single promote at t=0 would demote past the 2 s
        // silence budget mid-window (ticks gate drive while Disconnected).
        // Chunk ends sit off-grid, so each pump runs no loop tick.
        main.advanceTimeBy(CHUNK_WINDOW_MS)
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(CHUNK_WINDOW_MS)
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(SHORT_CHUNK_WINDOW_MS)

        assertEquals(CADENCE_FRAME_TOTAL, udp.sent.size)
        assertEquals(listOf(deviceId to 256u), repo.counterSaves)
    }

    @Test
    fun resumesCounterFromRoomRow() = runVmTest { main ->
        val device = testDevice().copy(lastCounter = 100u)
        val (viewModel, _, udp, _) = setup(repo = FakeDeviceRepository(device))
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")

        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)

        assertEquals(1, udp.sent.size)
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 101u, psk()),
            ),
        )
        assertNull(viewModel.state.value.error)
    }

    @Test
    fun missingPskErrorsWithZeroSends() = runVmTest { main ->
        // Paired, but the vault is empty: keyless pair, never legacy frames.
        // Forget wired: the broken row converges via repo clear -> gate fires.
        val (viewModel, repo, udp, vault) = setup(
            vault = FakePairSecretStore(),
            wireForget = true,
        )
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        main.runCurrent()

        assertTrue(udp.sent.isEmpty(), "no unsigned fallback, no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        // 2 vault reads: the session load (latched miss) + forget's UNPAIR
        // probe (also a miss, send skipped) — both one-shot.
        assertEquals(2, vault.loadCalls, "session miss + forget UNPAIR probe, then latched")
        assertNull(repo.observePaired().first(), "broken row converged: repo cleared")
        assertEquals(1, repo.clearCalls, "forget fired once")

        // Forget-once latch: a second window issues no further clears.
        main.advanceTimeBy(HALF_SECOND_WINDOW_MS)
        main.runCurrent()
        assertEquals(1, repo.clearCalls, "forget-once latch: no per-tick clears")
        assertEquals(2, vault.loadCalls, "still latched, no vault re-read")
    }

    /**
     * Tier-2 exhaustion surface: a session whose Room watermark sits at
     * gate-1 (PSK present — NOT the missing-PSK path) allocates nothing,
     * sends nothing (no UNPAIR counter is consumed either), and surfaces
     * [Connectivity.Unpaired] + the repair copy. Vault still read once
     * (session cached, loop idles).
     */
    @Test
    fun sessionExpiredSurfacesRepair() = runVmTest { main ->
        val device = testDevice().copy(lastCounter = ToRobotCounter.EXHAUSTION_GATE - 1u)
        val (viewModel, repo, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakePairSecretStore().apply { seed(device.id, PSK_HEX) },
            wireForget = true,
        )
        main.runCurrent()
        // Pre-MANUAL the gated tick only probes (no counter allocated, so the
        // fail-closed gate cannot refuse yet): promote, then the first
        // drivable tick refuses and surfaces the repair copy.
        assertTrue(udp.sent.isEmpty())
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        main.runCurrent()

        assertTrue(udp.sent.isEmpty(), "exhausted counter: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Session expired after long use — please re-pair", state.error)
        // 2 vault reads: the session load + forget's UNPAIR probe (null
        // counter allocation skips the send) — both one-shot.
        assertEquals(2, vault.loadCalls, "session load + forget UNPAIR probe, then latched")
        assertNull(repo.observePaired().first(), "exhausted session converged: repo cleared")
        assertEquals(1, repo.clearCalls, "forget fired once")

        // Forget-once latch: a second window issues no further clears.
        main.advanceTimeBy(HALF_SECOND_WINDOW_MS)
        main.runCurrent()
        assertEquals(1, repo.clearCalls, "forget-once latch: no per-tick clears")
    }

    /**
     * Vault-hex guard: at-rest corruption ("ZZZ" — non-hex) reuses the
     * missing-PSK path (Unpaired + repair copy, zero sends, one vault read)
     * instead of throwing inside the send loop and killing the coroutine.
     * The second advance proves the loop survived (still latched, still
     * empty — a dead loop would never re-tick, an unlatched loop would
     * re-read the vault). Re-pair with a good vault recovers (loadCalls==2).
     */
    @Test
    fun corruptVaultPskIdlesWithRepairCopy() = runVmTest { main ->
        val device = testDevice()
        val (viewModel, repo, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakePairSecretStore().apply { seed(device.id, "ZZZ") },
        )
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)

        assertTrue(udp.sent.isEmpty(), "undecodable vault hex: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        assertEquals(1, vault.loadCalls, "undecodable PSK latched: vault read once, loop idles")

        // Loop-death regression: a second window still ticks (latched, empty).
        main.advanceTimeBy(HALF_SECOND_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "loop survived: still no sends")
        assertEquals(1, vault.loadCalls, "loop survived: still latched, no vault re-read")
        assertEquals("Please re-pair to drive", viewModel.state.value.error)

        // Re-pair recovery: unpair clears the latch, a good vault reloads.
        repo.emit(null)
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        vault.seed(device.id, PSK_HEX)
        repo.emit(testDevice())
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isNotEmpty(), "re-pair after corrupt vault resumes sends")
        assertEquals(2, vault.loadCalls, "unpair cleared latch: re-pair re-reads the vault")
    }

    /**
     * Vault-hex guard, `ALIVE` variant: with [useAlive] a corrupt vault sends
     * neither `ALIVE` nor drive frames (zero sends of either type), latches
     * the vault read, and the 2 Hz loop survives the second window.
     * `ALIVE` skips silently — the drive section below owns the repair copy.
     */
    @Test
    fun corruptVaultPskSkipsAliveSilently() = runVmTest { main ->
        val device = testDevice()
        val (viewModel, _, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakePairSecretStore().apply { seed(device.id, "ZZZ") },
            useAlive = true,
        )
        main.runCurrent()
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)

        assertTrue(udp.sent.isEmpty(), "corrupt PSK: neither ALIVE nor drive sends")
        assertEquals(Connectivity.Unpaired, viewModel.state.value.connectivity)
        assertEquals("Please re-pair to drive", viewModel.state.value.error)
        assertEquals(1, vault.loadCalls, "session cached: vault read once, loop idles")

        // Second 500 ms window: still no ALIVE, still latched (loop alive).
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)
        assertTrue(udp.sent.isEmpty(), "loop survived: still no ALIVE")
        assertEquals(1, vault.loadCalls, "loop survived: still latched")
    }

    /**
     * Vault-hex guard, e-stop variant: the immediate e-stop frame skips
     * silently on a corrupt vault (no throw, no send) and the error stays
     * loop-owned (the immediate path never surfaces the missing-PSK copy —
     * only exhaustion surfaces there).
     */
    @Test
    fun estopOnCorruptVaultSkipsSilently() = runVmTest { main ->
        val device = testDevice()
        val (viewModel, _, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakePairSecretStore().apply { seed(device.id, "ZZZ") },
        )
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        udp.sent.clear()

        viewModel.onEmergencyStop()
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)

        assertTrue(viewModel.state.value.eStopped, "latch held even with no session")
        assertTrue(udp.sent.isEmpty(), "corrupt vault: e-stop sends nothing, immediate or loop")
        assertEquals(Connectivity.Unpaired, viewModel.state.value.connectivity)
        assertEquals("Please re-pair to drive", viewModel.state.value.error)
        assertEquals(1, vault.loadCalls, "vault read once: immediate + loop share the latch")
    }

    /**
     * Vault-hex guard, valid-hex-wrong-length ("abcd" -> 2 bytes): rejected
     * before sessionPsk caching exactly like non-hex — Unpaired + repair
     * copy, zero sends, one vault read, loop survives.
     */
    @Test
    fun shortVaultPskIdlesWithRepairCopy() = runVmTest { main ->
        val device = testDevice()
        val (viewModel, _, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakePairSecretStore().apply { seed(device.id, "abcd") },
        )
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)

        assertTrue(udp.sent.isEmpty(), "short vault hex: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        assertEquals(1, vault.loadCalls, "short PSK latched: vault read once, loop idles")

        main.advanceTimeBy(HALF_SECOND_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "loop survived: still no sends")
        assertEquals(1, vault.loadCalls, "loop survived: still latched, no vault re-read")
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
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.runCurrent()

        val state = viewModel.state.value
        assertEquals(Connectivity.Connected, state.connectivity)
        assertEquals(RobotMode.AUTONOMOUS, state.mode)
        assertEquals(packet, state.telemetry)
        assertFalse(state.manualMode, "packet carries no MANUAL hold bit")
    }

    /**
     * M2: telemetry silence past [TELEMETRY_TIMEOUT_MS] demotes Connected to
     * Disconnected (virtual time: timeout / tickMs send-loop ticks); a fresh
     * packet promotes again. The last packet stays visible throughout.
     */
    @Test
    fun telemetrySilenceDemotesToDisconnectedAndRecoveryPromotes() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()

        val packet = Telemetry(
            pitchDeg = 3.1f,
            rollDeg = -1.2f,
            distanceMm = 900,
            cliff = false,
            driving = true,
            mode = RobotMode.AUTONOMOUS,
        )
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        // ~1s of silence (50 ticks of the 100-tick / 2s budget): still linked.
        main.advanceTimeBy(IDLE_WINDOW_MS)
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        // ~2.5s more silence (budget long exceeded): demoted, packet kept.
        main.advanceTimeBy(LONG_SILENCE_WINDOW_MS)
        val stale = viewModel.state.value
        assertEquals(Connectivity.Disconnected, stale.connectivity)
        assertEquals(packet, stale.telemetry)

        // Fresh packet promotes again.
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
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
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        viewModel.onStick(DEFLECTED_STICK, DEFLECTED_STICK)
        main.runCurrent()
        udp.sent.clear()

        viewModel.onEmergencyStop()
        main.runCurrent()

        val latched = viewModel.state.value
        assertTrue(latched.eStopped)
        // Sticks center on estop.
        assertEquals(0f, latched.throttle)
        assertEquals(0f, latched.steering)
        // Immediate signed estop frame via the dedicated use case (the gated
        // pre-MANUAL tick allocates nothing, so this is counter 1).
        val estop1 = encodeSigned(
            ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
            1u,
            psk(),
        )
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(udp.sent[0].bytes.contentEquals(estop1))

        // Moving the stick must NOT release the latch...
        viewModel.onStick(ESTOP_LATCH_DEFLECTION, -ESTOP_LATCH_DEFLECTION)
        assertTrue(viewModel.state.value.eStopped)

        // ...and the heartbeat loop keeps sending signed centered+ESTOP_FLAG
        // (+1 for advanceTimeBy's exclusive end boundary): counters 2,3,4.
        main.advanceTimeBy(THREE_TICK_WINDOW_MS)
        val loopFrames = udp.sent.drop(1)
        assertTrue(loopFrames.size >= ESTOP_LOOP_SENDS)
        loopFrames.forEachIndexed { i, frame ->
            val expected = encodeSigned(
                ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
                (2u + i.toUInt()),
                psk(),
            )
            assertTrue(frame.bytes.contentEquals(expected), "loop frame $i")
        }
        assertEquals(0x01, ESTOP_FLAG)
    }

    @Test
    fun unpairPausesSendLoopDropsCacheAndRepairReloads() = runVmTest { main ->
        val (_, repo, udp, vault) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isNotEmpty(), "MANUAL reported: drive flows")
        assertEquals(1, vault.loadCalls)

        // Forgetting the device mid-session must stop frames to the stale IP.
        repo.emit(null)
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isEmpty())

        // Re-pairing resumes the heartbeat with a FRESH session load.
        repo.emit(testDevice())
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isNotEmpty())
        assertEquals(2, vault.loadCalls, "cache dropped on unpair: re-pair re-reads the vault")
        // Fresh pair row resets the counter: first frame back is counter 1.
        assertTrue(
            udp.sent[0].bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 1u, psk()),
            ),
        )
    }

    @Test
    fun forgetAfterUnpersistedFramesUsesLiveCounter() = runVmTest { main ->
        // Field-proven regression: the Room watermark persists only every
        // 128 sends, but the robot enforces strictly-greater against its
        // LIVE counter. UNPAIR must allocate from the shared holder (live),
        // never `device.lastCounter + 1` (watermark).
        val deviceId = testDevice().id
        val (viewModel, repo, udp, vault, holder) = setup(tickMs = TEST_TICK_MS)
        assertNull(viewModel.state.value.error)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        // Full-rate deflection: idle heartbeat would only send ~15 frames here.
        viewModel.onStick(DEFLECTED_STICK, 0f)
        // 149 drive frames total (the gated pre-MANUAL tick allocates nothing,
        // then t=20..2980): persist fires once at 128. Chunked with
        // re-promotes like counterPersistsEvery128Sends (chunk ends off-grid).
        main.advanceTimeBy(CHUNK_WINDOW_MS)
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(CHUNK_WINDOW_MS)
        promote(udp)
        main.runCurrent()
        main.advanceTimeBy(LONG_CHUNK_WINDOW_MS)

        assertEquals(DRIVE_FRAME_TOTAL, udp.sent.size)
        assertEquals(listOf(deviceId to 256u), repo.counterSaves)

        // Forget WITHOUT further persists: the holder still holds 149 live.
        val forget = ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), holder, FromRobotCounter(repo))
        val result = forget()

        assertIs<Outcome.Success<Unit>>(result, "forget result: $result")
        // 3x fire-and-forget UNPAIR on top of the 149 drive frames.
        assertEquals(DRIVE_PLUS_UNPAIR_TOTAL, udp.sent.size)
        val unpairs = udp.sent.drop(DRIVE_FRAME_TOTAL)
        assertEquals(ForgetDeviceUseCase.MAX_UNPAIR_SENDS, unpairs.size)
        val expected = encodeUnpair(psk(), 150u)
        unpairs.forEachIndexed { i, frame ->
            assertEquals(UNPAIR_HEADER, frame.bytes[0].toInt() and BYTE_MASK, "unpair $i")
            assertTrue(frame.bytes.contentEquals(expected), "unpair $i carries live counter 150, not watermark 129")
        }
    }

    @Test
    fun releaseEmergencyClearsLatch() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertTrue(viewModel.state.value.eStopped)

        viewModel.onReleaseEmergency()
        assertFalse(viewModel.state.value.eStopped)

        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary. The gated pre-MANUAL
        // tick allocates nothing, so the immediate estop took counter 1 and
        // the next loop frame is 2.
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isNotEmpty())
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 2u, psk()),
            ),
        )
    }

    /**
     * Joystick deadband + distinct: sub-threshold noise snaps to centered,
     * and rapid identical calls collapse to a single effective state update
     * (no extra sends — onStick itself never transmits, the loop samples).
     */
    @Test
    fun stickDeadbandSnapsNoiseAndDistinctSkipsDuplicates() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup()
        main.runCurrent()
        promote(udp)
        main.runCurrent()

        // Tiny noise (< 0.02) snaps to zero, never latching a deflection.
        viewModel.onStick(STICK_NOISE_SMALL, -STICK_NOISE_LARGE)
        assertEquals(0f, viewModel.state.value.throttle)
        assertEquals(0f, viewModel.state.value.steering)

        // Rapid identical deflected calls: one effective update, state stable.
        viewModel.onStick(DEFLECTED_STICK, TEST_STEERING)
        val deflected = viewModel.state.value
        assertEquals(DEFLECTED_STICK, deflected.throttle)
        assertEquals(TEST_STEERING, deflected.steering)
        repeat(REDUNDANT_INPUT_COUNT) { viewModel.onStick(DEFLECTED_STICK, TEST_STEERING) }
        assertEquals(deflected, viewModel.state.value)

        // Identical centered noise stays zero (no phantom deflection).
        viewModel.onStick(0f, 0f)
        repeat(REDUNDANT_INPUT_COUNT) { viewModel.onStick(STICK_NOISE_TINY, -STICK_NOISE_TINY) }
        assertEquals(0f, viewModel.state.value.throttle)
        assertEquals(0f, viewModel.state.value.steering)
    }

    /**
     * Sub-threshold stick noise between ticks must not hold the radio at
     * full rate: after reaching steady idle, spamming noise keeps the ~5 Hz
     * heartbeat (13 sends / 1000 ms at tickMs=20), not 50 Hz.
     */
    @Test
    fun stickNoiseDoesNotHoldFullRate() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")
        promote(udp)
        main.runCurrent()

        // Reach steady idle first (same 12-send shape as the heartbeat test:
        // gated pre-MANUAL tick runs the clock but allocates nothing).
        main.advanceTimeBy(IDLE_WINDOW_MS)
        assertEquals(IDLE_WINDOW_SENDS, udp.sent.size)
        udp.sent.clear()

        // Spam sub-threshold noise, then measure a full steady-idle window.
        // Steady idle (no grace restart) is 5 heartbeats / 1000 ms; had the
        // noise stamped activity, the 9-frame grace would restart → 13.
        repeat(REDUNDANT_INPUT_COUNT) { viewModel.onStick(STICK_NOISE_SMALL, -STICK_NOISE_SMALL) }
        main.runCurrent()
        udp.sent.clear()
        repeat(NOISE_SPAM_COUNT) { viewModel.onStick(STICK_NOISE_LARGE, STICK_NOISE_LARGE) }
        main.advanceTimeBy(IDLE_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "sub-threshold noise must not hold full-rate")

        // Sanity: identical deflected spam sends at full rate but adds no
        // extra frames beyond the loop ticks (5 ticks per 101 ms window).
        // ~2 s since the initial promote: re-promote past the silence budget.
        promote(udp)
        main.runCurrent()
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        udp.sent.clear()
        repeat(REDUNDANT_INPUT_COUNT) { viewModel.onStick(DEFLECTED_STICK, 0f) }
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "deflection sends every tick, no extras per call")
    }

    /**
     * Background pause: [RobotControlViewModel.onBackground] stops sends
     * (no counter alloc) while keeping the tick delay; [onForeground]
     * resumes with gapless counters and no vault re-read. Virtual time,
     * full-rate deflection so idle gating cannot hide a leak.
     */
    @Test
    fun backgroundPausesSendsAndForegroundResumesGapless() = runVmTest { main ->
        val (viewModel, _, udp, vault) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")
        promote(udp)
        main.runCurrent()

        // Full-rate deflection: deterministic every-tick sends.
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "deflection sends every tick")
        // Counters so far: 5 (the gated pre-MANUAL tick allocates nothing).
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 5u, psk()),
            ),
        )
        assertEquals(1, vault.loadCalls, "session cached")

        // Background: 25 ticks, zero sends, zero counter advance.
        viewModel.onBackground()
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(HALF_SECOND_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "background pauses sends")
        assertEquals(1, vault.loadCalls, "background holds session, no vault re-read")

        // Foreground resumes immediately with the next gapless counter (6).
        viewModel.onForeground()
        // No intermediate runCurrent/clear: measure the next window directly.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "foreground resumes every tick")
        assertTrue(
            udp.sent.first().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 6u, psk()),
            ),
            "no counter advance while backgrounded",
        )
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 10u, psk()),
            ),
        )
        assertEquals(1, vault.loadCalls, "resume needs no vault re-read")
    }

    /**
     * Layer A flush: driving N<128 (no cadence persist yet), then
     * [RobotControlViewModel.onBackground] fire-and-forget flushes the
     * watermark — one extra Room write with the live lastSent.
     */
    @Test
    fun backgroundFlushesWatermark() = runVmTest { main ->
        val deviceId = testDevice().id
        val (viewModel, repo, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")
        promote(udp)
        main.runCurrent()

        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size)
        assertTrue(repo.counterSaves.isEmpty(), "N<128: no cadence persist yet")

        viewModel.onBackground()
        main.runCurrent()
        assertEquals(listOf(deviceId to 5u), repo.counterSaves)
    }

    /**
     * Phase-1 UI coalescing: a 15 Hz burst of pitch-only packets recomposes
     * at ~5 Hz (gated copies), while the link logic stays at 15 Hz
     * (Connected throughout) and the 2 s silence timeout still demotes with
     * last-packet retention.
     */
    @Test
    fun telemetryBurstCoalescesToUiRateButKeepsLinkAndTimeout() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()

        fun datagram(pitch: Float) = UdpDatagram(
            TelemetryPacketCodec.encode(
                Telemetry(
                    pitchDeg = pitch,
                    rollDeg = 0f,
                    distanceMm = 900,
                    cliff = false,
                    driving = true,
                    mode = RobotMode.MANUAL,
                ),
            ),
            ROBOT_HOST,
            DEFAULT_UDP_PORT,
        )

        // First packet paints immediately (promotion + due clock).
        udp.inbound.emit(datagram(0f))
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
        assertEquals(0f, viewModel.state.value.telemetry?.pitchDeg)

        // 15 Hz burst: 9 pitch-only packets ~66 ms apart (~594 ms total).
        // UI window is 200 ms (10 ticks), so at most ~3-4 gated copies.
        val observed = mutableListOf(viewModel.state.value.telemetry?.pitchDeg)
        repeat(BURST_PACKET_COUNT) { i ->
            main.advanceTimeBy(BURST_SPACING_MS)
            udp.inbound.emit(datagram(BURST_BASE_PITCH + i))
            main.runCurrent()
            observed += viewModel.state.value.telemetry?.pitchDeg
        }
        val uiChanges = observed.zipWithNext().count { (a, b) -> a != b }
        assertTrue(
            uiChanges < BURST_PACKET_COUNT,
            "15 Hz pitch-only burst must coalesce: got $uiChanges UI updates for 9 packets ($observed)",
        )
        assertTrue(
            uiChanges <= MAX_BURST_UI_UPDATES,
            "expected ~5 Hz (~3 UI updates in ~600 ms), got $uiChanges ($observed)",
        )
        assertTrue(uiChanges >= 1, "burst must paint at least once per window ($observed)")
        assertEquals(
            Connectivity.Connected,
            viewModel.state.value.connectivity,
            "logic path stays Connected at 15 Hz even when UI copies are gated",
        )

        // Silence past the 2 s budget still demotes, last packet retained.
        main.advanceTimeBy(LONG_SILENCE_WINDOW_MS)
        main.runCurrent()
        val stale = viewModel.state.value
        assertEquals(Connectivity.Disconnected, stale.connectivity)
        assertEquals(
            viewModel.state.value.telemetry,
            stale.telemetry,
            "last packet stays visible on Disconnected",
        )
        kotlin.test.assertNotNull(stale.telemetry)
    }

    /**
     * High-value edges (mode / cliff / driving flips) promote immediately
     * even inside the 200 ms UI window, while a pitch-only drift in the same
     * window stays gated.
     */
    @Test
    fun telemetryModeCliffDrivingEdgesPromoteImmediately() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()

        fun datagram(
            pitch: Float = 1f,
            mode: RobotMode = RobotMode.MANUAL,
            cliff: Boolean = false,
            driving: Boolean = true,
        ) = UdpDatagram(
            TelemetryPacketCodec.encode(
                Telemetry(
                    pitchDeg = pitch,
                    rollDeg = 0f,
                    distanceMm = 900,
                    cliff = cliff,
                    driving = driving,
                    mode = mode,
                ),
            ),
            ROBOT_HOST,
            DEFAULT_UDP_PORT,
        )

        udp.inbound.emit(datagram())
        main.runCurrent()
        assertEquals(RobotMode.MANUAL, viewModel.state.value.mode)
        assertEquals(false, viewModel.state.value.telemetry?.cliff)

        // 1 tick later (well inside the 200 ms / 10-tick window): pitch-only
        // drift stays gated...
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(datagram(pitch = 2f))
        main.runCurrent()
        assertEquals(
            1f,
            viewModel.state.value.telemetry?.pitchDeg,
            "low-value drift inside the window must not recompose",
        )

        // ...but a mode flip in the same window paints immediately.
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(datagram(pitch = 3f, mode = RobotMode.AUTONOMOUS))
        main.runCurrent()
        assertEquals(RobotMode.AUTONOMOUS, viewModel.state.value.mode)
        assertEquals(3f, viewModel.state.value.telemetry?.pitchDeg)

        // Cliff edge: 1 tick after the mode paint (window just reset).
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(datagram(pitch = 4f, mode = RobotMode.AUTONOMOUS, cliff = true))
        main.runCurrent()
        assertEquals(true, viewModel.state.value.telemetry?.cliff)
        assertEquals(4f, viewModel.state.value.telemetry?.pitchDeg)

        // Driving edge: same story.
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(
            datagram(pitch = 5f, mode = RobotMode.AUTONOMOUS, cliff = true, driving = false),
        )
        main.runCurrent()
        assertEquals(false, viewModel.state.value.telemetry?.driving)
        assertEquals(5f, viewModel.state.value.telemetry?.pitchDeg)
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
    }

    /**
     * Disconnected gating (locked decision b): while
     * [Connectivity.Disconnected], sticks, e-stop, and release are all
     * no-ops — no state change, no immediate frame, latched error kept.
     * The drive send loop itself keeps heartbeating centered underneath
     * (failsafe intact).
     */
    @Test
    fun disconnectedGatesSticksAndEstop() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        assertEquals(Connectivity.Disconnected, viewModel.state.value.connectivity)
        assertTrue(udp.sent.isEmpty(), "pre-MANUAL the loop probes but transmits nothing")

        viewModel.onStick(DEFLECTED_STICK, -TEST_STEERING)
        assertEquals(0f, viewModel.state.value.throttle, "stick gated: no state change")
        assertEquals(0f, viewModel.state.value.steering, "stick gated: no state change")

        viewModel.onEmergencyStop()
        main.runCurrent()
        assertFalse(viewModel.state.value.eStopped, "e-stop gated: no latch, no frame")

        viewModel.onReleaseEmergency()
        assertFalse(viewModel.state.value.eStopped)

        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        // No drive frame goes out while Disconnected (and MANUAL was never
        // reported): the loop only probes session health underneath.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "gated ticks transmit nothing")
        assertNull(viewModel.state.value.error, "gating keeps the banner untouched")
    }

    /**
     * `ALIVE` keepalive: immediate on the first paired tick (session ready),
     * then steady 2 Hz on its own tick clock — independent of the drive
     * idle heartbeat. Both frames share one dense counter sequence with
     * drive (frame index + 1 == counter, zero skips).
     */
    @Test
    fun aliveImmediateThenSteadyTwoHz() = runVmTest { main ->
        val (viewModel, _, udp, vault) = setup(tickMs = TEST_TICK_MS, useAlive = true)
        main.runCurrent()

        // First paired tick: ALIVE(1) immediately (drive stays gated —
        // MANUAL is not reported yet, and the gated tick allocates nothing).
        assertEquals(1, udp.sent.size, "session-ready tick sends ALIVE only, drive gated pre-MANUAL")
        val firstAlive = udp.sent[0]
        assertEquals(ALIVE_HEADER, firstAlive.bytes[0].toInt() and BYTE_MASK)
        assertTrue(
            firstAlive.bytes.contentEquals(encodeAlive(psk(), 0u, 1u)),
            "first ALIVE takes counter 1 under era 0",
        )
        assertEquals(1, vault.loadCalls, "ALIVE reuses the session cache: one vault read")

        // MANUAL reported: drive joins the same dense sequence (counter 2+).
        promote(udp)
        main.runCurrent()

        // +1 boundary: ticks at 20..500. Second ALIVE lands at t=500
        // (25 ticks); its counter is dense (frame index + 1).
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)
        val alives = udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }
        assertEquals(2, alives.size, "2 Hz: t=0 and t=500")
        val secondIdx = udp.sent.indexOf(alives[1])
        assertTrue(
            alives[1].bytes.contentEquals(encodeAlive(psk(), 0u, (secondIdx + 1).toUInt())),
            "ALIVE counter stays dense with drive",
        )
        // Drive kept flowing underneath (idle-gated, but alive).
        assertTrue(udp.sent.any { (it.bytes[0].toInt() and BYTE_MASK) == DRIVE_HEADER })
        assertNull(viewModel.state.value.error, "ALIVE never touches the banner")

        // Next 500 ms window: third ALIVE at t=1000, still 2 Hz (+1 so the
        // boundary tick runs inside the window).
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)
        val alives3 = udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }
        assertEquals(EXPECTED_ALIVE_TOTAL, alives3.size, "2 Hz: t=0, 500, 1000")
        val thirdIdx = udp.sent.indexOf(alives3[2])
        assertTrue(
            alives3[2].bytes.contentEquals(encodeAlive(psk(), 0u, (thirdIdx + 1).toUInt())),
            "third ALIVE stays dense",
        )
    }

    /**
     * Background sends `DISCONNECT` 2x byte-identical (one shared-counter
     * allocation) then pauses all sends; foreground forces the `ALIVE`
     * clock due so the next tick knocks immediately with gapless counters.
     */
    @Test
    fun backgroundSendsDisconnectThenPausesAndForegroundKnocksAlive() = runVmTest { main ->
        val deviceId = testDevice().id
        val (viewModel, repo, udp, _) = setup(tickMs = TEST_TICK_MS, useAlive = true)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "ALIVE(1) on the first tick; drive gated pre-MANUAL")
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        viewModel.onBackground()
        main.runCurrent()
        // Flush (1 live alloc: the ALIVE) + one DISCONNECT allocation sent twice.
        assertEquals(listOf(deviceId to 1u), repo.counterSaves, "flush persists the live watermark")
        assertEquals(GOODBYE_SEND_TOTAL, udp.sent.size, "exactly 2 DISCONNECT frames on top")
        val goodbyes = udp.sent.drop(1)
        goodbyes.forEach { frame ->
            assertEquals(DISCONNECT_HEADER, frame.bytes[0].toInt() and BYTE_MASK)
        }
        assertTrue(
            goodbyes[0].bytes.contentEquals(goodbyes[1].bytes),
            "byte-identical repeat: one allocation, like UNPAIR",
        )
        assertTrue(
            goodbyes[0].bytes.contentEquals(encodeDisconnect(psk(), 0u, 2u)),
            "DISCONNECT consumes the next live counter under era 0",
        )

        // Paused: 25 ticks, zero sends, zero counter advance (+1 boundary
        // so the t=500 tick runs inside the window instead of leaking into
        // the resume measurement below).
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)
        assertEquals(GOODBYE_SEND_TOTAL, udp.sent.size, "background pauses ALIVE and drive alike")

        // Foreground: next tick knocks ALIVE(3) immediately, drive(4) resumes.
        viewModel.onForeground()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "ALIVE + drive resume on the next tick")
        assertTrue(
            udp.sent[RESUME_ALIVE_INDEX].bytes.contentEquals(encodeAlive(psk(), 0u, 3u)),
            "immediate ALIVE, no counter advance while backgrounded",
        )
        assertTrue(
            udp.sent[RESUME_DRIVE_INDEX].bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 4u, psk()),
            ),
            "drive resumes gapless",
        )
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
    }

    /**
     * Link-loss round trip: silence past [TELEMETRY_TIMEOUT_MS] demotes to
     * [Connectivity.Disconnected] (intents gate, `ALIVE` keeps knocking),
     * and the first telemetry re-enables via the existing promotion path.
     */
    @Test
    fun telemetryDemoteGatesAndReenables() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS, useAlive = true)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        viewModel.onStick(DEFLECTED_STICK, 0f)
        assertEquals(DEFLECTED_STICK, viewModel.state.value.throttle, "connected sticks work")

        // ~2.5 s silence: demoted, but the ALIVE knock kept going out.
        main.advanceTimeBy(LONG_SILENCE_WINDOW_MS)
        assertEquals(Connectivity.Disconnected, viewModel.state.value.connectivity)
        val knocking = udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }
        assertTrue(knocking.size >= MIN_ALIVE_KNOCKS, "ALIVE knocks through the silence (got ${knocking.size})")

        // Gated while down (state frozen, no latch).
        viewModel.onStick(RECONNECT_DEFLECTION, RECONNECT_DEFLECTION)
        assertEquals(DEFLECTED_STICK, viewModel.state.value.throttle, "stick gated while Disconnected")
        assertEquals(0f, viewModel.state.value.steering, "stick gated while Disconnected")
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertFalse(viewModel.state.value.eStopped, "e-stop gated while Disconnected")

        // First telemetry re-enables: intents flow again immediately.
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
        viewModel.onStick(RECONNECT_DEFLECTION, RECONNECT_DEFLECTION)
        assertEquals(RECONNECT_DEFLECTION, viewModel.state.value.throttle)
        assertEquals(RECONNECT_DEFLECTION, viewModel.state.value.steering)
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertTrue(viewModel.state.value.eStopped, "e-stop works once re-enabled")
    }

    /**
     * Network rejoin: a `networkAvailable` event forces the `ALIVE` clock due so
     * the next tick knocks immediately (boot-ready treatment), then the
     * steady 2 Hz resumes; a flap burst collapses to one `ALIVE` (idempotent
     * due-flag — the Fake has no 1 s debounce, so 5 rapid emits must still
     * yield one knock; production collapses earlier in the binder).
     */
    @Test
    fun rejoinFiresImmediateAlive() = runVmTest { main ->
        val (_, _, udp, _, _, _, binder) = setup(tickMs = TEST_TICK_MS, useAlive = true)
        main.runCurrent()
        // t=0: ALIVE(1) on the first paired tick (drive gated pre-MANUAL).
        assertEquals(1, udp.sent.size)
        fun aliveCount() = udp.sent.count { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }

        // Drift mid-interval (t=200): ALIVE clock not due (next due t=500).
        main.advanceTimeBy(MID_INTERVAL_MS)
        assertEquals(1, aliveCount(), "mid-interval: no second ALIVE yet")

        // Rejoin: the next tick knocks immediately (within 1 tick).
        binder.emitNetworkAvailable()
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertEquals(2, aliveCount(), "rejoin forces immediate ALIVE within 1 tick")
        val alives = udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }
        val secondIdx = udp.sent.indexOf(alives[1])
        assertTrue(
            alives[1].bytes.contentEquals(encodeAlive(psk(), 0u, (secondIdx + 1).toUInt())),
            "rejoin ALIVE stays dense with drive",
        )

        // Steady 2 Hz resumes: next ALIVE ~500 ms after the rejoin knock
        // (+1 so the due tick runs inside the window).
        main.advanceTimeBy(HALF_SECOND_WINDOW_PLUS_ONE_MS)
        assertEquals(EXPECTED_ALIVE_TOTAL, aliveCount(), "2 Hz resumes after the rejoin knock")

        // Flap x5 within 1 s: a single ALIVE (burst collapses before the tick).
        udp.sent.clear()
        repeat(FLAP_BURST_COUNT) { binder.emitNetworkAvailable() }
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        val flapAlives = udp.sent.filter { (it.bytes[0].toInt() and BYTE_MASK) == ALIVE_HEADER }
        assertEquals(1, flapAlives.size, "flap burst yields a single ALIVE")
    }

    /**
     * Manual gating: paired + linked but no MANUAL hold transmits nothing —
     * the loop only probes session health (no counter allocated). The first
     * MANUAL report flips [RobotControlUiState.manualMode] and drive resumes
     * with counter 1 (dense — gated ticks consume nothing).
     */
    @Test
    fun driveGatedUntilManualReported() = runVmTest { main ->
        val (viewModel, _, udp, vault) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "pre-link the loop probes but transmits nothing")

        // Linked, but the robot holds NORMAL: Connected, not drivable.
        promote(udp, manual = false)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
        assertFalse(viewModel.state.value.manualMode)
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertTrue(udp.sent.isEmpty(), "linked-but-not-manual transmits no drive frames")
        assertEquals(1, vault.loadCalls, "gated probe caches the session: vault read once")

        // MANUAL reported: the hold surfaces in state and drive resumes.
        viewModel.onStick(DEFLECTED_STICK, 0f)
        promote(udp, manual = true)
        main.runCurrent()
        assertTrue(viewModel.state.value.manualMode, "MANUAL hold surfaced in UI state")
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        assertEquals(FULL_RATE_SENDS, udp.sent.size, "deflection sends every tick once MANUAL is reported")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 5u, psk()),
            ),
            "gated ticks allocate nothing: first drive still counter 1",
        )
    }

    /**
     * `MANUAL` hold edge promotes the UI copy immediately, like the
     * mode/cliff/driving edges — even inside the 200 ms coalescing window.
     */
    @Test
    fun manualHoldEdgePromotesImmediately() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()

        fun datagram(pitch: Float, manual: Boolean) = UdpDatagram(
            TelemetryPacketCodec.encode(
                Telemetry(
                    pitchDeg = pitch,
                    rollDeg = 0f,
                    distanceMm = 900,
                    cliff = false,
                    driving = true,
                    mode = RobotMode.MANUAL,
                    manual = manual,
                ),
            ),
            ROBOT_HOST,
            DEFAULT_UDP_PORT,
        )

        udp.inbound.emit(datagram(1f, manual = false))
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
        assertFalse(viewModel.state.value.manualMode)

        // Pitch-only drift one tick later stays gated...
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(datagram(2f, manual = false))
        main.runCurrent()
        assertEquals(1f, viewModel.state.value.telemetry?.pitchDeg)
        assertFalse(viewModel.state.value.manualMode)

        // ...but the MANUAL hold edge in the same window paints immediately.
        main.advanceTimeBy(TEST_TICK_MS)
        udp.inbound.emit(datagram(3f, manual = true))
        main.runCurrent()
        assertTrue(viewModel.state.value.manualMode, "MANUAL hold edge promotes immediately")
        assertEquals(3f, viewModel.state.value.telemetry?.pitchDeg)
        assertEquals(true, viewModel.state.value.telemetry?.manual)
    }

    /**
     * Take: one `MANUAL_TAKE` per user action on the shared counter, then
     * drive resumes dense once the robot reports the hold.
     */
    @Test
    fun takeManualSendsSingleFrameThenDriveResumesDense() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS, wireManual = true)
        main.runCurrent()
        promote(udp, manual = false)
        main.runCurrent()
        assertFalse(viewModel.state.value.manualMode)

        viewModel.onTakeManual()
        main.runCurrent()

        assertEquals(1, udp.sent.size, "entry is deliberate: exactly one frame per action")
        val take = udp.sent[0]
        assertEquals(MANUAL_TAKE_HEADER, take.bytes[0].toInt() and BYTE_MASK)
        assertTrue(
            take.bytes.contentEquals(encodeManualTake(psk(), 0u, 1u)),
            "take consumes the shared send counter under the current era",
        )

        // Robot reports the hold: drive resumes on the same sequence.
        viewModel.onStick(DEFLECTED_STICK, 0f)
        promote(udp, manual = true)
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, DEFLECTED_STICK, 0f, 0), 2u, psk()),
            ),
            "drive continues gapless after take",
        )
    }

    /**
     * Take is a no-op while already MANUAL (repeats add nothing — no counter
     * consumed) and while Disconnected.
     */
    @Test
    fun takeManualNoopWhenAlreadyManualOrDisconnected() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS, wireManual = true)
        main.runCurrent()
        // Disconnected: no-op, no frame, no error.
        viewModel.onTakeManual()
        main.runCurrent()
        assertTrue(udp.sent.isEmpty())
        assertNull(viewModel.state.value.error)

        // Already MANUAL: no-op, drive keeps flowing underneath.
        promote(udp, manual = true)
        main.runCurrent()
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        udp.sent.clear()
        viewModel.onTakeManual()
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.none { (it.bytes[0].toInt() and BYTE_MASK) == MANUAL_TAKE_HEADER })
        assertTrue(udp.sent.isNotEmpty(), "drive unaffected by the redundant take")
    }

    /**
     * Release: one shared-counter allocation, `MANUAL_RELEASE` 2x
     * byte-identical best-effort. Disconnected release is a no-op.
     */
    @Test
    fun releaseManualSendsTwiceByteIdentical() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS, wireManual = true)
        main.runCurrent()
        // Disconnected: no-op.
        viewModel.onReleaseManual()
        main.runCurrent()
        assertTrue(udp.sent.isEmpty())

        promote(udp, manual = true)
        main.runCurrent()
        viewModel.onReleaseManual()
        main.runCurrent()

        // The gated pre-MANUAL tick allocates nothing, so release takes
        // counter 1; the loop's drive tick has not run yet (no time advanced).
        assertEquals(2, udp.sent.size, "best-effort 2x repeat")
        udp.sent.forEach { frame ->
            assertEquals(MANUAL_RELEASE_HEADER, frame.bytes[0].toInt() and BYTE_MASK)
        }
        assertTrue(
            udp.sent[0].bytes.contentEquals(encodeManualRelease(psk(), 0u, 1u)),
            "release consumes the shared send counter under the current era",
        )
        assertTrue(
            udp.sent[0].bytes.contentEquals(udp.sent[1].bytes),
            "byte-identical repeat: one allocation",
        )
        assertNull(viewModel.state.value.error, "best-effort release never touches the banner")
    }

    /**
     * Take failure surfaces (entry was an explicit user action); release
     * swallows (best-effort — the robot's own command-stale exit covers it).
     */
    @Test
    fun takeFailureSurfacesWhileReleaseStaysBestEffort() = runVmTest { main ->
        val (takeVm, _, takeUdp, _) = setup(tickMs = TEST_TICK_MS, wireManual = true)
        takeUdp.sendError = IllegalStateException("dead socket")
        main.runCurrent()
        promote(takeUdp, manual = false)
        main.runCurrent()
        takeVm.onTakeManual()
        main.runCurrent()
        val takeError = takeVm.state.value.error
        assertTrue(takeError != null && takeError.contains("manual take"), "take error: $takeError")
        assertTrue(takeUdp.sent.isEmpty())

        val (relVm, _, relUdp, _) = setup(tickMs = TEST_TICK_MS, wireManual = true)
        relUdp.sendError = IllegalStateException("dead socket")
        main.runCurrent()
        promote(relUdp, manual = true)
        main.runCurrent()
        relVm.onReleaseManual()
        main.runCurrent()
        assertNull(relVm.state.value.error, "best-effort release stays silent")
        assertTrue(relUdp.sent.isEmpty())
    }

    /**
     * Legacy graphs (null take/release use cases): the intents are inert —
     * no frames, no crash — while the MANUAL gate still applies to drive.
     */
    @Test
    fun takeReleaseInertWithoutUseCases() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        promote(udp, manual = true)
        main.runCurrent()
        viewModel.onTakeManual()
        viewModel.onReleaseManual()
        main.runCurrent()
        main.advanceTimeBy(ONE_TICK_WINDOW_MS)
        assertTrue(udp.sent.none {
            val h = it.bytes[0].toInt() and BYTE_MASK
            h == MANUAL_TAKE_HEADER || h == MANUAL_RELEASE_HEADER
        })
        assertTrue(udp.sent.isNotEmpty(), "drive still flows under MANUAL without the use cases")
    }

    /**
     * No wedge on a dead endpoint: while sends fail the banner latches but
     * the loop keeps ticking, and once the link is back frames resume to
     * the same hostname (re-resolved per send) with the banner cleared.
     */
    @Test
    fun transientSendFailureLatchesBannerThenResumesToHostname() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = TEST_TICK_MS)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        viewModel.onStick(DEFLECTED_STICK, 0f)
        main.runCurrent()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        main.runCurrent()
        assertTrue(udp.sent.isNotEmpty(), "drive flows before the outage")
        assertNull(viewModel.state.value.error)

        // Outage (robot rebooting onto a new address): every send throws.
        udp.sendError = IllegalStateException("dead socket")
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        main.runCurrent()
        val outageError = viewModel.state.value.error
        assertTrue(
            outageError != null && outageError.contains("Failed to send control command"),
            "outage latches the banner: $outageError",
        )

        // Link back: the loop resumes to the same hostname, banner cleared.
        udp.sendError = null
        udp.sent.clear()
        main.advanceTimeBy(FIVE_TICK_WINDOW_MS)
        main.runCurrent()
        assertTrue(udp.sent.isNotEmpty(), "sends resume after the outage")
        udp.sent.forEach { assertEquals(ROBOT_HOST, it.host) }
        assertNull(viewModel.state.value.error, "recovery clears the banner")
    }
}
