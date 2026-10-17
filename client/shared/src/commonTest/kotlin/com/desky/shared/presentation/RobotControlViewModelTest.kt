package com.desky.shared.presentation

import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.ControlPacketCodec
import com.desky.shared.domain.ALIVE_HEADER
import com.desky.shared.domain.AliveUseCase
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.DISCONNECT_HEADER
import com.desky.shared.domain.DRIVE_V2_HEADER
import com.desky.shared.domain.DRIVE_V2_SIZE
import com.desky.shared.domain.DriveCounter
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.GetBondPskUseCase
import com.desky.shared.domain.UNBOND_HEADER
import com.desky.shared.domain.encodeAlive
import com.desky.shared.domain.encodeDisconnect
import com.desky.shared.domain.encodeSigned
import com.desky.shared.domain.encodeUnbond
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TelemetryPacketCodec
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.hexToBytes
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

    private fun psk() = hexToBytes(PSK_HEX)

    private fun setup(
        repo: FakeDeviceRepository = FakeDeviceRepository(testDevice()),
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        vault: FakeBondSecretStore = FakeBondSecretStore().apply { seed(testDevice().id, PSK_HEX) },
        tickMs: Long = 20,
        holder: DriveCounter = DriveCounter(repo),
        useAlive: Boolean = false,
        binder: FakeUdpSocketBinder = FakeUdpSocketBinder(),
        wireForget: Boolean = false,
    ): Quad {
        val sendSigned = SendSignedControlUseCase(udp)
        val getPsk = GetBondPskUseCase(vault, repo)
        val forgetUC = if (wireForget) ForgetDeviceUseCase(repo, vault, udp, getPsk, holder) else null
        val viewModel = RobotControlViewModel(
            sendSigned,
            ObserveTelemetryUseCase(udp),
            EmergencyStopUseCase(sendSigned),
            repo,
            getPsk,
            udp,
            holder,
            tickMs,
            aliveUC = if (useAlive) AliveUseCase(udp) else null,
            socketBinder = binder,
            forgetUC = forgetUC,
        )
        return Quad(viewModel, repo, udp, vault, holder, binder)
    }

    /**
     * Promotes the link to [Connectivity.Connected] with a neutral MANUAL
     * packet. Required before any user intent: sticks/e-stop/release are
     * gated to no-ops while [Connectivity.Disconnected]. Emits + pumps but
     * advances no virtual time, so the 2 s silence budget restarts here.
     */
    private suspend fun promote(udp: FakeUdpSocketManager) {
        val packet = Telemetry(
            pitchDeg = 0f,
            rollDeg = 0f,
            distanceMm = 900,
            cliff = false,
            driving = false,
            mode = RobotMode.MANUAL,
        )
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), "192.168.4.1", DEFAULT_UDP_PORT))
    }

    private data class Quad(
        val vm: RobotControlViewModel,
        val repo: FakeDeviceRepository,
        val udp: FakeUdpSocketManager,
        val vault: FakeBondSecretStore,
        val holder: DriveCounter,
        val binder: FakeUdpSocketBinder = FakeUdpSocketBinder(),
    )

    @Test
    fun stickUpdatesStateClamped() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup()
        main.runCurrent()
        promote(udp)
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
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 1u, psk()),
            ),
        )
        assertEquals("192.168.4.1", udp.sent[0].host)
        assertEquals(DEFAULT_UDP_PORT, udp.sent[0].port)

        // Sticks are gated while Disconnected: promote first.
        promote(udp)
        main.runCurrent()
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
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, -0.25f, 0), 6u, psk()),
            ),
        )
    }

    @Test
    fun vaultReadOncePerSession() = runVmTest { main ->
        val (_, _, udp, vault) = setup(tickMs = 20)
        main.runCurrent()
        main.advanceTimeBy(201)

        // Centered idle: 200 ms activity grace (t=0..160) then ~5 Hz, so 9
        // sends here — every one from the single cached session load.
        assertEquals(9, udp.sent.size)
        assertEquals(1, vault.loadCalls, "vault read once, session cached in memory")
        main.advanceTimeBy(201)
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
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "first frame goes out immediately")
        // Gated while Disconnected: promote up front (safe here — clock is
        // t=0, no loop tick due, so the pump sends nothing extra).
        promote(udp)
        main.runCurrent()

        // Centered idle: 9-frame grace (t=0..160) then heartbeats at
        // t=360,560,760,960 — 13 sends in 50 ticks, counters 1..13 gapless.
        main.advanceTimeBy(1000)
        assertEquals(13, udp.sent.size, "idle sends ~1 per 10 ticks")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 13u, psk()),
            ),
            "skipped ticks allocate no counters",
        )

        // Deflection resumes every tick immediately (the runCurrent also
        // fires the loop tick due exactly at the advanceTimeBy end boundary).
        viewModel.onStick(0.5f, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(101)
        assertEquals(5, udp.sent.size, "deflection sends every tick")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, 0f, 0), 19u, psk()),
            ),
        )

        // Recenter past the grace: 9-frame grace then heartbeats every 10th
        // tick — 13 sends in the next 50 ticks, counters 20..32 gapless.
        viewModel.onStick(0f, 0f)
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(1000)
        assertEquals(13, udp.sent.size, "recentered idle returns to ~1 per 10 ticks")
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 32u, psk()),
            ),
        )

        // Latched e-stop forces every tick (centered + ESTOP_FLAG): the
        // immediate frame is counter 33, the loop sends 34,35,36.
        // ~2.1 s since the initial promote: re-promote past the silence
        // budget first (safe — clock sits off-grid at t=2101, no loop tick
        // due, so the pump sends nothing extra).
        promote(udp)
        main.runCurrent()
        viewModel.onEmergencyStop()
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(61)
        assertEquals(3, udp.sent.size, "eStopped forces every tick")
        udp.sent.forEachIndexed { i, frame ->
            assertTrue(
                frame.bytes.contentEquals(
                    encodeSigned(
                        ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
                        (34u + i.toUInt()),
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
        // Deflected (full-rate): idle heartbeat would only send ~13 frames here.
        val (viewModel, repo, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        // Gated while Disconnected: promote first (pump init before the
        // emit — an emit before the first pump never reaches the collector).
        promote(udp)
        main.runCurrent()
        viewModel.onStick(0.5f, 0f)
        main.runCurrent()
        // 129 frames total (t=0..2560): the 128th allocation write-aheads to 256.
        main.advanceTimeBy(2561)

        assertEquals(129, udp.sent.size)
        assertEquals(listOf(deviceId to 256u), repo.counterSaves)
    }

    @Test
    fun resumesCounterFromRoomRow() = runVmTest { main ->
        val device = testDevice().copy(lastCounter = 100u)
        val (viewModel, _, udp, _) = setup(repo = FakeDeviceRepository(device))
        main.runCurrent()

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
        // Paired, but the vault is empty: keyless bond, never legacy frames.
        // Forget wired: the broken row converges via repo clear -> gate fires.
        val (viewModel, repo, udp, vault) = setup(
            vault = FakeBondSecretStore(),
            wireForget = true,
        )
        main.runCurrent()
        main.advanceTimeBy(101)
        main.runCurrent()

        assertTrue(udp.sent.isEmpty(), "no unsigned fallback, no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        // 2 vault reads: the session load (latched miss) + forget's UNBOND
        // probe (also a miss, send skipped) — both one-shot.
        assertEquals(2, vault.loadCalls, "session miss + forget UNBOND probe, then latched")
        assertNull(repo.observePaired().first(), "broken row converged: repo cleared")
        assertEquals(1, repo.clearCalls, "forget fired once")

        // Forget-once latch: a second window issues no further clears.
        main.advanceTimeBy(500)
        main.runCurrent()
        assertEquals(1, repo.clearCalls, "forget-once latch: no per-tick clears")
        assertEquals(2, vault.loadCalls, "still latched, no vault re-read")
    }

    /**
     * Tier-2 exhaustion surface: a session whose Room watermark sits at
     * gate-1 (PSK present — NOT the missing-PSK path) allocates nothing,
     * sends nothing (no UNBOND counter is consumed either), and surfaces
     * [Connectivity.Unpaired] + the repair copy. Vault still read once
     * (session cached, loop idles).
     */
    @Test
    fun sessionExpiredSurfacesRepair() = runVmTest { main ->
        val device = testDevice().copy(lastCounter = DriveCounter.EXHAUSTION_GATE - 1u)
        val (viewModel, repo, udp, vault) = setup(
            repo = FakeDeviceRepository(device),
            vault = FakeBondSecretStore().apply { seed(device.id, PSK_HEX) },
            wireForget = true,
        )
        main.runCurrent()
        main.advanceTimeBy(101)
        main.runCurrent()

        assertTrue(udp.sent.isEmpty(), "exhausted counter: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Session expired after long use — please re-pair", state.error)
        // 2 vault reads: the session load + forget's UNBOND probe (null
        // counter allocation skips the send) — both one-shot.
        assertEquals(2, vault.loadCalls, "session load + forget UNBOND probe, then latched")
        assertNull(repo.observePaired().first(), "exhausted session converged: repo cleared")
        assertEquals(1, repo.clearCalls, "forget fired once")

        // Forget-once latch: a second window issues no further clears.
        main.advanceTimeBy(500)
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
            vault = FakeBondSecretStore().apply { seed(device.id, "ZZZ") },
        )
        main.runCurrent()
        main.advanceTimeBy(101)

        assertTrue(udp.sent.isEmpty(), "undecodable vault hex: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        assertEquals(1, vault.loadCalls, "undecodable PSK latched: vault read once, loop idles")

        // Loop-death regression: a second window still ticks (latched, empty).
        main.advanceTimeBy(500)
        assertTrue(udp.sent.isEmpty(), "loop survived: still no sends")
        assertEquals(1, vault.loadCalls, "loop survived: still latched, no vault re-read")
        assertEquals("Please re-pair to drive", viewModel.state.value.error)

        // Re-pair recovery: unpair clears the latch, a good vault reloads.
        repo.emit(null)
        main.runCurrent()
        main.advanceTimeBy(101)
        vault.seed(device.id, PSK_HEX)
        repo.emit(testDevice())
        main.runCurrent()
        main.advanceTimeBy(21)
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
            vault = FakeBondSecretStore().apply { seed(device.id, "ZZZ") },
            useAlive = true,
        )
        main.runCurrent()
        main.advanceTimeBy(501)

        assertTrue(udp.sent.isEmpty(), "corrupt PSK: neither ALIVE nor drive sends")
        assertEquals(Connectivity.Unpaired, viewModel.state.value.connectivity)
        assertEquals("Please re-pair to drive", viewModel.state.value.error)
        assertEquals(1, vault.loadCalls, "session cached: vault read once, loop idles")

        // Second 500 ms window: still no ALIVE, still latched (loop alive).
        main.advanceTimeBy(501)
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
            vault = FakeBondSecretStore().apply { seed(device.id, "ZZZ") },
        )
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        udp.sent.clear()

        viewModel.onEmergencyStop()
        main.runCurrent()
        main.advanceTimeBy(101)

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
            vault = FakeBondSecretStore().apply { seed(device.id, "abcd") },
        )
        main.runCurrent()
        main.advanceTimeBy(101)

        assertTrue(udp.sent.isEmpty(), "short vault hex: no sends at all")
        val state = viewModel.state.value
        assertEquals(Connectivity.Unpaired, state.connectivity)
        assertEquals("Please re-pair to drive", state.error)
        assertEquals(1, vault.loadCalls, "short PSK latched: vault read once, loop idles")

        main.advanceTimeBy(500)
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
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), "192.168.4.1", DEFAULT_UDP_PORT))
        main.runCurrent()

        val state = viewModel.state.value
        assertEquals(Connectivity.Connected, state.connectivity)
        assertEquals(RobotMode.AUTONOMOUS, state.mode)
        assertEquals(packet, state.telemetry)
    }

    /**
     * M2: telemetry silence past [TELEMETRY_TIMEOUT_MS] demotes Connected to
     * Disconnected (virtual time: timeout / tickMs send-loop ticks); a fresh
     * packet promotes again. The last packet stays visible throughout.
     */
    @Test
    fun telemetrySilenceDemotesToDisconnectedAndRecoveryPromotes() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()

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
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        // ~1s of silence (50 ticks of the 100-tick / 2s budget): still linked.
        main.advanceTimeBy(1000)
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        // ~2.5s more silence (budget long exceeded): demoted, packet kept.
        main.advanceTimeBy(2500)
        val stale = viewModel.state.value
        assertEquals(Connectivity.Disconnected, stale.connectivity)
        assertEquals(packet, stale.telemetry)

        // Fresh packet promotes again.
        udp.inbound.emit(UdpDatagram(TelemetryPacketCodec.encode(packet), "192.168.4.1", DEFAULT_UDP_PORT))
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
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        promote(udp)
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
        val estop2 = encodeSigned(
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
            val expected = encodeSigned(
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
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 1u, psk()),
            ),
        )
    }

    @Test
    fun forgetAfterUnpersistedFramesUsesLiveCounter() = runVmTest { main ->
        // Field-proven regression: the Room watermark persists only every
        // 128 sends, but the robot enforces strictly-greater against its
        // LIVE counter. UNBOND must allocate from the shared holder (live),
        // never `device.lastCounter + 1` (watermark).
        val deviceId = testDevice().id
        val (viewModel, repo, udp, vault, holder) = setup(tickMs = 20)
        assertNull(viewModel.state.value.error)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        // Full-rate deflection: idle heartbeat would only send ~15 frames here.
        viewModel.onStick(0.5f, 0f)
        // 150 drive frames total (t=0 + 149 ticks): persist fires once at 128.
        main.advanceTimeBy(149 * 20 + 1)

        assertEquals(150, udp.sent.size)
        assertEquals(listOf(deviceId to 256u), repo.counterSaves)

        // Forget WITHOUT further persists: the holder still holds 150 live.
        val forget = ForgetDeviceUseCase(repo, vault, udp, GetBondPskUseCase(vault, repo), holder)
        val result = forget()

        assertIs<Outcome.Success<Unit>>(result, "forget result: $result")
        // 3x fire-and-forget UNBOND on top of the 150 drive frames.
        assertEquals(153, udp.sent.size)
        val unbonds = udp.sent.drop(150)
        assertEquals(3, unbonds.size)
        val expected = encodeUnbond(psk(), 151u)
        unbonds.forEachIndexed { i, frame ->
            assertEquals(UNBOND_HEADER, frame.bytes[0].toInt() and 0xFF, "unbond $i")
            assertTrue(frame.bytes.contentEquals(expected), "unbond $i carries live counter 151, not watermark 129")
        }
    }

    @Test
    fun releaseEmergencyClearsLatch() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        promote(udp)
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
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 3u, psk()),
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
        viewModel.onStick(0.01f, -0.015f)
        assertEquals(0f, viewModel.state.value.throttle)
        assertEquals(0f, viewModel.state.value.steering)

        // Rapid identical deflected calls: one effective update, state stable.
        viewModel.onStick(0.5f, 0.25f)
        val deflected = viewModel.state.value
        assertEquals(0.5f, deflected.throttle)
        assertEquals(0.25f, deflected.steering)
        repeat(20) { viewModel.onStick(0.5f, 0.25f) }
        assertEquals(deflected, viewModel.state.value)

        // Identical centered noise stays zero (no phantom deflection).
        viewModel.onStick(0f, 0f)
        repeat(20) { viewModel.onStick(0.005f, -0.005f) }
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
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "first frame goes out immediately")
        promote(udp)
        main.runCurrent()

        // Reach steady idle first (same 13-send shape as the heartbeat test).
        main.advanceTimeBy(1000)
        assertEquals(13, udp.sent.size)
        udp.sent.clear()

        // Spam sub-threshold noise, then measure a full steady-idle window.
        // Steady idle (no grace restart) is 5 heartbeats / 1000 ms; had the
        // noise stamped activity, the 9-frame grace would restart → 13.
        repeat(20) { viewModel.onStick(0.01f, -0.01f) }
        main.runCurrent()
        udp.sent.clear()
        repeat(10) { viewModel.onStick(0.015f, 0.015f) }
        main.advanceTimeBy(1000)
        assertEquals(5, udp.sent.size, "sub-threshold noise must not hold full-rate")

        // Sanity: identical deflected spam sends at full rate but adds no
        // extra frames beyond the loop ticks (5 ticks per 101 ms window).
        // ~2 s since the initial promote: re-promote past the silence budget.
        promote(udp)
        main.runCurrent()
        viewModel.onStick(0.5f, 0f)
        main.runCurrent()
        udp.sent.clear()
        repeat(20) { viewModel.onStick(0.5f, 0f) }
        main.advanceTimeBy(101)
        assertEquals(5, udp.sent.size, "deflection sends every tick, no extras per call")
    }

    /**
     * Background pause: [RobotControlViewModel.onBackground] stops sends
     * (no counter alloc) while keeping the tick delay; [onForeground]
     * resumes with gapless counters and no vault re-read. Virtual time,
     * full-rate deflection so idle gating cannot hide a leak.
     */
    @Test
    fun backgroundPausesSendsAndForegroundResumesGapless() = runVmTest { main ->
        val (viewModel, _, udp, vault) = setup(tickMs = 20)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "first frame goes out immediately")
        promote(udp)
        main.runCurrent()

        // Full-rate deflection: deterministic every-tick sends.
        viewModel.onStick(0.5f, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(101)
        assertEquals(5, udp.sent.size, "deflection sends every tick")
        // Counters so far: 1 (t=0) + 5 = 6; last is 6.
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, 0f, 0), 6u, psk()),
            ),
        )
        assertEquals(1, vault.loadCalls, "session cached")

        // Background: 25 ticks, zero sends, zero counter advance.
        viewModel.onBackground()
        main.runCurrent()
        udp.sent.clear()
        main.advanceTimeBy(500)
        assertTrue(udp.sent.isEmpty(), "background pauses sends")
        assertEquals(1, vault.loadCalls, "background holds session, no vault re-read")

        // Foreground resumes immediately with the next gapless counter (7).
        viewModel.onForeground()
        // No intermediate runCurrent/clear: measure the next window directly.
        main.advanceTimeBy(101)
        assertEquals(5, udp.sent.size, "foreground resumes every tick")
        assertTrue(
            udp.sent.first().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, 0f, 0), 7u, psk()),
            ),
            "no counter advance while backgrounded",
        )
        assertTrue(
            udp.sent.last().bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0.5f, 0f, 0), 11u, psk()),
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
        val (viewModel, repo, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "first frame goes out immediately")
        promote(udp)
        main.runCurrent()

        viewModel.onStick(0.5f, 0f)
        main.runCurrent()
        udp.sent.clear()
        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        main.advanceTimeBy(101)
        assertEquals(5, udp.sent.size)
        assertTrue(repo.counterSaves.isEmpty(), "N<128: no cadence persist yet")

        viewModel.onBackground()
        main.runCurrent()
        assertEquals(listOf(deviceId to 6u), repo.counterSaves)
    }

    /**
     * Phase-1 UI coalescing: a 15 Hz burst of pitch-only packets recomposes
     * at ~5 Hz (gated copies), while the link logic stays at 15 Hz
     * (Connected throughout) and the 2 s silence timeout still demotes with
     * last-packet retention.
     */
    @Test
    fun telemetryBurstCoalescesToUiRateButKeepsLinkAndTimeout() = runVmTest { main ->
        val (viewModel, _, udp, _) = setup(tickMs = 20)
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
            "192.168.4.1",
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
        repeat(9) { i ->
            main.advanceTimeBy(66)
            udp.inbound.emit(datagram(10f + i))
            main.runCurrent()
            observed += viewModel.state.value.telemetry?.pitchDeg
        }
        val uiChanges = observed.zipWithNext().count { (a, b) -> a != b }
        assertTrue(
            uiChanges < 9,
            "15 Hz pitch-only burst must coalesce: got $uiChanges UI updates for 9 packets ($observed)",
        )
        assertTrue(
            uiChanges <= 4,
            "expected ~5 Hz (~3 UI updates in ~600 ms), got $uiChanges ($observed)",
        )
        assertTrue(uiChanges >= 1, "burst must paint at least once per window ($observed)")
        assertEquals(
            Connectivity.Connected,
            viewModel.state.value.connectivity,
            "logic path stays Connected at 15 Hz even when UI copies are gated",
        )

        // Silence past the 2 s budget still demotes, last packet retained.
        main.advanceTimeBy(2500)
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
        val (viewModel, _, udp, _) = setup(tickMs = 20)
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
            "192.168.4.1",
            DEFAULT_UDP_PORT,
        )

        udp.inbound.emit(datagram())
        main.runCurrent()
        assertEquals(RobotMode.MANUAL, viewModel.state.value.mode)
        assertEquals(false, viewModel.state.value.telemetry?.cliff)

        // 1 tick later (well inside the 200 ms / 10-tick window): pitch-only
        // drift stays gated...
        main.advanceTimeBy(20)
        udp.inbound.emit(datagram(pitch = 2f))
        main.runCurrent()
        assertEquals(
            1f,
            viewModel.state.value.telemetry?.pitchDeg,
            "low-value drift inside the window must not recompose",
        )

        // ...but a mode flip in the same window paints immediately.
        main.advanceTimeBy(20)
        udp.inbound.emit(datagram(pitch = 3f, mode = RobotMode.AUTONOMOUS))
        main.runCurrent()
        assertEquals(RobotMode.AUTONOMOUS, viewModel.state.value.mode)
        assertEquals(3f, viewModel.state.value.telemetry?.pitchDeg)

        // Cliff edge: 1 tick after the mode paint (window just reset).
        main.advanceTimeBy(20)
        udp.inbound.emit(datagram(pitch = 4f, mode = RobotMode.AUTONOMOUS, cliff = true))
        main.runCurrent()
        assertEquals(true, viewModel.state.value.telemetry?.cliff)
        assertEquals(4f, viewModel.state.value.telemetry?.pitchDeg)

        // Driving edge: same story.
        main.advanceTimeBy(20)
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
        val (viewModel, _, udp, _) = setup(tickMs = 20)
        main.runCurrent()
        assertEquals(Connectivity.Disconnected, viewModel.state.value.connectivity)
        assertEquals(1, udp.sent.size, "loop heartbeats even while Disconnected")

        viewModel.onStick(0.5f, -0.25f)
        assertEquals(0f, viewModel.state.value.throttle, "stick gated: no state change")
        assertEquals(0f, viewModel.state.value.steering, "stick gated: no state change")

        viewModel.onEmergencyStop()
        main.runCurrent()
        assertFalse(viewModel.state.value.eStopped, "e-stop gated: no latch, no frame")

        viewModel.onReleaseEmergency()
        assertFalse(viewModel.state.value.eStopped)

        // +1 for advanceTimeBy's exclusive end boundary: ticks at 20..100.
        // Every frame is a centered MANUAL flags-0 heartbeat — nothing
        // deflected, nothing latched, no error surfaced.
        main.advanceTimeBy(101)
        assertEquals(6, udp.sent.size)
        udp.sent.forEachIndexed { i, frame ->
            assertEquals(DRIVE_V2_HEADER, frame.bytes[0].toInt() and 0xFF, "frame $i")
            assertEquals(0x00, frame.bytes[1].toInt() and 0xFF, "frame $i mode")
            assertEquals(0x80, frame.bytes[2].toInt() and 0xFF, "frame $i throttle centered")
            assertEquals(0x80, frame.bytes[3].toInt() and 0xFF, "frame $i steering centered")
            assertEquals(0x00, frame.bytes[4].toInt() and 0xFF, "frame $i flags clear")
        }
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
        val (viewModel, _, udp, vault) = setup(tickMs = 20, useAlive = true)
        main.runCurrent()

        // First paired tick: ALIVE(1) immediately, then the drive heartbeat(2).
        assertEquals(2, udp.sent.size, "session-ready tick sends ALIVE + drive")
        val firstAlive = udp.sent[0]
        assertEquals(ALIVE_HEADER, firstAlive.bytes[0].toInt() and 0xFF)
        assertTrue(
            firstAlive.bytes.contentEquals(encodeAlive(psk(), 0u, 1u)),
            "first ALIVE takes counter 1 under era 0",
        )
        assertTrue(
            udp.sent[1].bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 2u, psk()),
            ),
            "drive heartbeat follows on the same sequence",
        )
        assertEquals(1, vault.loadCalls, "ALIVE reuses the session cache: one vault read")

        // +1 boundary: ticks at 20..500. Second ALIVE lands at t=500
        // (25 ticks); its counter is dense (frame index + 1).
        main.advanceTimeBy(501)
        val alives = udp.sent.filter { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }
        assertEquals(2, alives.size, "2 Hz: t=0 and t=500")
        val secondIdx = udp.sent.indexOf(alives[1])
        assertTrue(
            alives[1].bytes.contentEquals(encodeAlive(psk(), 0u, (secondIdx + 1).toUInt())),
            "ALIVE counter stays dense with drive",
        )
        // Drive kept flowing underneath (idle-gated, but alive).
        assertTrue(udp.sent.any { (it.bytes[0].toInt() and 0xFF) == DRIVE_V2_HEADER })
        assertNull(viewModel.state.value.error, "ALIVE never touches the banner")

        // Next 500 ms window: third ALIVE at t=1000, still 2 Hz (+1 so the
        // boundary tick runs inside the window).
        main.advanceTimeBy(501)
        val alives3 = udp.sent.filter { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }
        assertEquals(3, alives3.size, "2 Hz: t=0, 500, 1000")
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
        val (viewModel, repo, udp, _) = setup(tickMs = 20, useAlive = true)
        main.runCurrent()
        assertEquals(2, udp.sent.size, "ALIVE(1) + drive(2) on the first tick")
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        viewModel.onBackground()
        main.runCurrent()
        // Flush (2 live allocs) + one DISCONNECT allocation sent twice.
        assertEquals(listOf(deviceId to 2u), repo.counterSaves, "flush persists the live watermark")
        assertEquals(4, udp.sent.size, "exactly 2 DISCONNECT frames on top")
        val goodbyes = udp.sent.drop(2)
        goodbyes.forEach { frame ->
            assertEquals(DISCONNECT_HEADER, frame.bytes[0].toInt() and 0xFF)
        }
        assertTrue(
            goodbyes[0].bytes.contentEquals(goodbyes[1].bytes),
            "byte-identical repeat: one allocation, like UNBOND",
        )
        assertTrue(
            goodbyes[0].bytes.contentEquals(encodeDisconnect(psk(), 0u, 3u)),
            "DISCONNECT consumes the next live counter under era 0",
        )

        // Paused: 25 ticks, zero sends, zero counter advance (+1 boundary
        // so the t=500 tick runs inside the window instead of leaking into
        // the resume measurement below).
        main.advanceTimeBy(501)
        assertEquals(4, udp.sent.size, "background pauses ALIVE and drive alike")

        // Foreground: next tick knocks ALIVE(4) immediately, drive(5) resumes.
        viewModel.onForeground()
        main.advanceTimeBy(21)
        assertEquals(6, udp.sent.size, "ALIVE + drive resume on the next tick")
        assertTrue(
            udp.sent[4].bytes.contentEquals(encodeAlive(psk(), 0u, 4u)),
            "immediate ALIVE, no counter advance while backgrounded",
        )
        assertTrue(
            udp.sent[5].bytes.contentEquals(
                encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f, 0), 5u, psk()),
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
        val (viewModel, _, udp, _) = setup(tickMs = 20, useAlive = true)
        main.runCurrent()
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)

        viewModel.onStick(0.5f, 0f)
        assertEquals(0.5f, viewModel.state.value.throttle, "connected sticks work")

        // ~2.5 s silence: demoted, but the ALIVE knock kept going out.
        main.advanceTimeBy(2500)
        assertEquals(Connectivity.Disconnected, viewModel.state.value.connectivity)
        val knocking = udp.sent.filter { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }
        assertTrue(knocking.size >= 4, "ALIVE knocks through the silence (got ${knocking.size})")

        // Gated while down (state frozen, no latch).
        viewModel.onStick(0.9f, 0.9f)
        assertEquals(0.5f, viewModel.state.value.throttle, "stick gated while Disconnected")
        assertEquals(0f, viewModel.state.value.steering, "stick gated while Disconnected")
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertFalse(viewModel.state.value.eStopped, "e-stop gated while Disconnected")

        // First telemetry re-enables: intents flow again immediately.
        promote(udp)
        main.runCurrent()
        assertEquals(Connectivity.Connected, viewModel.state.value.connectivity)
        viewModel.onStick(0.9f, 0.9f)
        assertEquals(0.9f, viewModel.state.value.throttle)
        assertEquals(0.9f, viewModel.state.value.steering)
        viewModel.onEmergencyStop()
        main.runCurrent()
        assertTrue(viewModel.state.value.eStopped, "e-stop works once re-enabled")
    }

    /**
     * AP rejoin: a `networkAvailable` event forces the `ALIVE` clock due so
     * the next tick knocks immediately (boot-ready treatment), then the
     * steady 2 Hz resumes; a flap burst collapses to one `ALIVE` (idempotent
     * due-flag — the Fake has no 1 s debounce, so 5 rapid emits must still
     * yield one knock; production collapses earlier in the binder).
     */
    @Test
    fun rejoinFiresImmediateAlive() = runVmTest { main ->
        val (_, _, udp, _, _, binder) = setup(tickMs = 20, useAlive = true)
        main.runCurrent()
        // t=0: ALIVE(1) + drive(2) on the first paired tick.
        assertEquals(2, udp.sent.size)
        fun aliveCount() = udp.sent.count { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }

        // Drift mid-interval (t=200): ALIVE clock not due (next due t=500).
        main.advanceTimeBy(200)
        assertEquals(1, aliveCount(), "mid-interval: no second ALIVE yet")

        // Rejoin: the next tick knocks immediately (within 1 tick).
        binder.emitNetworkAvailable()
        main.runCurrent()
        main.advanceTimeBy(21)
        assertEquals(2, aliveCount(), "rejoin forces immediate ALIVE within 1 tick")
        val alives = udp.sent.filter { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }
        val secondIdx = udp.sent.indexOf(alives[1])
        assertTrue(
            alives[1].bytes.contentEquals(encodeAlive(psk(), 0u, (secondIdx + 1).toUInt())),
            "rejoin ALIVE stays dense with drive",
        )

        // Steady 2 Hz resumes: next ALIVE ~500 ms after the rejoin knock
        // (+1 so the due tick runs inside the window).
        main.advanceTimeBy(501)
        assertEquals(3, aliveCount(), "2 Hz resumes after the rejoin knock")

        // Flap x5 within 1 s: a single ALIVE (burst collapses before the tick).
        udp.sent.clear()
        repeat(5) { binder.emitNetworkAvailable() }
        main.runCurrent()
        main.advanceTimeBy(21)
        val flapAlives = udp.sent.filter { (it.bytes[0].toInt() and 0xFF) == ALIVE_HEADER }
        assertEquals(1, flapAlives.size, "flap burst yields a single ALIVE")
    }
}
