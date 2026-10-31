package com.desky.shared.features.control

import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.Outcome
import com.desky.shared.core.PAIR_REFRESH_ACK_HEADER
import com.desky.shared.core.PAIR_REFRESH_REQ_SIZE
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.UdpDatagram
import com.desky.shared.core.UdpSocketManager
import com.desky.shared.core.pairRefreshAckTag
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val OLD_PSK_HEX = "00112233445566778899aabbccddeeff"
private const val NEXT_PSK_HEX = "2ecca11e7638cb50bd4c9dd96ff9da1b"
private const val HEX_RADIX = 16
private const val BYTE_MASK = 0xFF
private const val ERA_HIGH_BYTE_SHIFT = 8

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(HEX_RADIX).toByte()
    }
}

/** Hand-written fake: records sends, replays queued inbound datagrams. */
private class FakePairRefreshUdp : UdpSocketManager {
    val sent = mutableListOf<UdpDatagram>()
    val inbound = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = inbound

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sent += UdpDatagram(bytes, host, port)
    }

    override suspend fun evictAll() = Unit

    override suspend fun close() = Unit
}

private fun pairedDevice(era: UShort = 0u, lastCounter: UInt = 99u) = PairedDevice(
    id = "desky-$ROBOT_HOST",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = lastCounter,
    alias = ROBOT_HOST,
    persistEvery = 128,
    era = era,
)

/** `PAIR_REFRESH_ACK` for [era] under [newPsk] (real crypto), framed with XOR. */
private suspend fun pairRefreshAckFor(era: UShort, newPsk: ByteArray): ByteArray {
    val tag = pairRefreshAckTag(newPsk, era)
    val body = byteArrayOf(PAIR_REFRESH_ACK_HEADER.toByte(), ((era.toInt() shr ERA_HIGH_BYTE_SHIFT) and BYTE_MASK).toByte(), (era.toInt() and BYTE_MASK).toByte()) + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
    return body + byteArrayOf(xor.toByte())
}

@OptIn(ExperimentalCoroutinesApi::class)
class PairRefreshUseCaseTest {

    private companion object {
        const val RETRY_BUDGET_MS = 50L
        const val EXTENDED_RETRY_BUDGET_MS = 1000L
        const val FIRST_BUDGET_LAPSE_MS = 60L
        const val ALL_BUDGETS_LAPSE_MS = 200L
        const val ACK_TAG_SIZE = 6
    }

    private fun useCase(
        repo: FakeDeviceRepository,
        vault: FakePairSecretStore,
        udp: FakePairRefreshUdp,
        holder: ToRobotCounter,
        fromRobot: FromRobotCounter = FromRobotCounter(repo),
        attemptTimeoutsMs: List<Long> = DEFAULT_PAIR_REFRESH_ATTEMPT_TIMEOUTS_MS,
    ) = PairRefreshUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), holder, fromRobot, attemptTimeoutsMs)

    @Test
    fun ackSuccessRotatesEraPskAndCounter() = runTest {
        val device = pairedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakePairRefreshUdp()
        val holder = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        fromRobot.ensure(device.id, 41u)
        val deferred = async { useCase(repo, vault, udp, holder, fromRobot)() }
        runCurrent()

        // One PAIR_REFRESH_REQ: era 1 (0 + 1), counter 100 (99 + 1) under the old key.
        assertEquals(1, udp.sent.size, "exactly one PAIR_REFRESH_REQ before any reply")
        val req = udp.sent[0]
        assertEquals(ROBOT_HOST, req.host)
        assertEquals(PAIR_REFRESH_REQ_SIZE, req.bytes.size)
        assertContentEquals(hex("53000100000064ecac8eb7c93cba"), req.bytes)

        udp.inbound.emit(UdpDatagram(pairRefreshAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        assertIs<Outcome.Success<UShort>>(result)
        assertEquals(1u, result.data)
        // Vault holds the derived next PSK; Room holds the new era + the
        // pair baseline (0 — the first post-pair-refresh drive uses 1).
        assertEquals(NEXT_PSK_HEX, vault.loadPsk(device.id))
        assertEquals(listOf(Triple(device.id, 0u, 1u.toUShort())), repo.pairRefreshSaves)
        // Holder reset to the new era at baseline: gapless resume at 1.
        assertEquals(1u, holder.currentEra())
        assertEquals(1u, holder.next())
        // Receive domain reset to the 0 baseline (the robot restarts its
        // own send sequence with the new era); Room persists the same.
        assertNull(fromRobot.current())
        assertEquals(0u, repo.observePaired().first()?.lastRobotCounter)
    }

    @Test
    fun retriesReuseSameBytesThenSucceed() = runTest {
        val device = pairedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakePairRefreshUdp()
        val holder = ToRobotCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(RETRY_BUDGET_MS, RETRY_BUDGET_MS, RETRY_BUDGET_MS))() }
        runCurrent()

        assertEquals(1, udp.sent.size)
        // Silence past the first budget: exactly one retry, byte-identical
        // (same counter — the robot's idempotent re-ACK path, not a fresh
        // allocation that would burn the counter sequence).
        advanceTimeBy(FIRST_BUDGET_LAPSE_MS)
        runCurrent()
        assertEquals(2, udp.sent.size, "one retry after silence")
        assertContentEquals(udp.sent[0].bytes, udp.sent[1].bytes)

        udp.inbound.emit(UdpDatagram(pairRefreshAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()
        assertIs<Outcome.Success<UShort>>(result)
        assertEquals(1u, result.data)
    }

    @Test
    fun wrongEraAckIsIgnored() = runTest {
        val device = pairedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakePairRefreshUdp()
        val holder = ToRobotCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(RETRY_BUDGET_MS, EXTENDED_RETRY_BUDGET_MS))() }
        runCurrent()

        // An ACK for a foreign era is not our reply: the exchange waits on.
        udp.inbound.emit(UdpDatagram(pairRefreshAckFor(2u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        runCurrent()
        assertTrue(deferred.isActive, "foreign-era ACK must not complete the exchange")

        udp.inbound.emit(UdpDatagram(pairRefreshAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()
        assertIs<Outcome.Success<UShort>>(result)
    }

    @Test
    fun tagMismatchFailsFast() = runTest {
        val device = pairedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakePairRefreshUdp()
        val holder = ToRobotCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder)() }
        runCurrent()

        // Well-formed ACK (right era, valid checksum) but forged tag: fail
        // immediately — never retry a forgery, never persist.
        val body = byteArrayOf(PAIR_REFRESH_ACK_HEADER.toByte(), 0x00, 0x01) + ByteArray(ACK_TAG_SIZE) { 0x00 }
        var xor = 0
        for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
        udp.inbound.emit(UdpDatagram(body + byteArrayOf(xor.toByte()), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("invalid response"), "reason=${failure.reason}")
        assertEquals(1, udp.sent.size, "no retry after a forgery")
        assertEquals(OLD_PSK_HEX, vault.loadPsk(device.id), "vault keeps the old key")
        assertTrue(repo.pairRefreshSaves.isEmpty(), "Room untouched")
    }

    /**
     * Timeout fallback (Tier-2 owns the gate): after all budgets expire with
     * silence, the failure surfaces the repair path — vault, Room, and the
     * holder era are untouched, so drive keeps using the old key until the
     * gate (where the "session expired — please re-pair" copy fires).
     */
    @Test
    fun timeoutFallsBackToTier2Repair() = runTest {
        val device = pairedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakePairRefreshUdp()
        val holder = ToRobotCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(RETRY_BUDGET_MS, RETRY_BUDGET_MS))() }
        runCurrent()

        advanceTimeBy(ALL_BUDGETS_LAPSE_MS)
        val result = deferred.await()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("no response"), "reason=${failure.reason}")
        assertEquals(2, udp.sent.size, "both budgets sent")
        assertEquals(OLD_PSK_HEX, vault.loadPsk(device.id), "vault keeps the old key")
        assertTrue(repo.pairRefreshSaves.isEmpty(), "Room untouched")
        assertEquals(0u, holder.currentEra(), "holder keeps the old era")
    }

    @Test
    fun noDeviceOrKeyFailsWithoutSending() = runTest {
        // Unpaired: nothing leaves the radio.
        val emptyRepo = FakeDeviceRepository(null)
        val emptyUdp = FakePairRefreshUdp()
        val emptyVault = FakePairSecretStore()
        val emptyHolder = ToRobotCounter(emptyRepo)
        val unpaired = async { useCase(emptyRepo, emptyVault, emptyUdp, emptyHolder)() }
        runCurrent()
        assertIs<Outcome.Failure>(unpaired.await())
        assertTrue(emptyUdp.sent.isEmpty())

        // Paired but keyless: same (the repair copy owns this path).
        val device = pairedDevice()
        val keylessRepo = FakeDeviceRepository(device)
        val keylessUdp = FakePairRefreshUdp()
        val keylessVault = FakePairSecretStore()
        val keylessHolder = ToRobotCounter(keylessRepo)
        val keyless = async { useCase(keylessRepo, keylessVault, keylessUdp, keylessHolder)() }
        runCurrent()
        assertIs<Outcome.Failure>(keyless.await())
        assertTrue(keylessUdp.sent.isEmpty())
    }
}
