package com.desky.shared.domain

import com.desky.shared.presentation.FakeBondSecretStore
import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

private const val OLD_PSK_HEX = "00112233445566778899aabbccddeeff"
private const val NEXT_PSK_HEX = "df7441d0d71778b2dd10493962d45a5b"

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

/** Hand-written fake: records sends, replays queued inbound datagrams. */
private class FakeRekeyUdp : UdpSocketManager {
    val sent = mutableListOf<UdpDatagram>()
    val inbound = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = inbound

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sent += UdpDatagram(bytes, host, port)
    }

    override suspend fun close() = Unit
}

private fun bondedDevice(era: UShort = 0u, lastCounter: UInt = 99u) = PairedDevice(
    id = "desky-192.168.4.1",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = lastCounter,
    alias = ROBOT_HOST,
    persistEvery = 128,
    era = era,
)

/** `REKEY_ACK` for [era] under [newPsk] (real crypto), framed with XOR. */
private suspend fun rekeyAckFor(era: UShort, newPsk: ByteArray): ByteArray {
    val tag = rekeyAckTag(newPsk, era)
    val body = byteArrayOf(REKEY_ACK_HEADER.toByte(), ((era.toInt() shr 8) and 0xFF).toByte(), (era.toInt() and 0xFF).toByte()) + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

@OptIn(ExperimentalCoroutinesApi::class)
class RekeyUseCaseTest {

    private fun useCase(
        repo: FakeDeviceRepository,
        vault: FakeBondSecretStore,
        udp: FakeRekeyUdp,
        holder: DriveCounter,
        attemptTimeoutsMs: List<Long> = DEFAULT_REKEY_ATTEMPT_TIMEOUTS_MS,
    ) = RekeyUseCase(repo, vault, udp, GetBondPskUseCase(vault, repo), holder, attemptTimeoutsMs)

    @Test
    fun ackSuccessRotatesEraPskAndCounter() = runTest {
        val device = bondedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakeBondSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakeRekeyUdp()
        val holder = DriveCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder)() }
        runCurrent()

        // One REKEY_REQ: era 1 (0 + 1), counter 100 (99 + 1) under the old key.
        assertEquals(1, udp.sent.size, "exactly one REKEY_REQ before any reply")
        val req = udp.sent[0]
        assertEquals(ROBOT_HOST, req.host)
        assertEquals(REKEY_REQ_SIZE, req.bytes.size)
        assertContentEquals(hex("53000100000064fdfe277d16235a"), req.bytes)

        udp.inbound.emit(UdpDatagram(rekeyAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        assertIs<Outcome.Success<UShort>>(result)
        assertEquals(1u, result.data)
        // Vault holds the derived next PSK; Room holds the new era + the
        // bond baseline (0 — first post-rekey drive uses 1).
        assertEquals(NEXT_PSK_HEX, vault.loadPsk(device.id))
        assertEquals(listOf(Triple(device.id, 0u, 1u.toUShort())), repo.rekeySaves)
        // Holder reset to the new era at baseline: gapless resume at 1.
        assertEquals(1u, holder.currentEra())
        assertEquals(1u, holder.next())
    }

    @Test
    fun retriesReuseSameBytesThenSucceed() = runTest {
        val device = bondedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakeBondSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakeRekeyUdp()
        val holder = DriveCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(50, 50, 50))() }
        runCurrent()

        assertEquals(1, udp.sent.size)
        // Silence past the first budget: exactly one retry, byte-identical
        // (same counter — the robot's idempotent re-ACK path, not a fresh
        // allocation that would burn the counter sequence).
        advanceTimeBy(60)
        runCurrent()
        assertEquals(2, udp.sent.size, "one retry after silence")
        assertContentEquals(udp.sent[0].bytes, udp.sent[1].bytes)

        udp.inbound.emit(UdpDatagram(rekeyAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()
        assertIs<Outcome.Success<UShort>>(result)
        assertEquals(1u, result.data)
    }

    @Test
    fun wrongEraAckIsIgnored() = runTest {
        val device = bondedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakeBondSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakeRekeyUdp()
        val holder = DriveCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(50, 1000))() }
        runCurrent()

        // An ACK for a foreign era is not our reply: the exchange waits on.
        udp.inbound.emit(UdpDatagram(rekeyAckFor(2u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        runCurrent()
        assertTrue(deferred.isActive, "foreign-era ACK must not complete the exchange")

        udp.inbound.emit(UdpDatagram(rekeyAckFor(1u, hex(NEXT_PSK_HEX)), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()
        assertIs<Outcome.Success<UShort>>(result)
    }

    @Test
    fun tagMismatchFailsFast() = runTest {
        val device = bondedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakeBondSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakeRekeyUdp()
        val holder = DriveCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder)() }
        runCurrent()

        // Well-formed ACK (right era, valid checksum) but forged tag: fail
        // immediately — never retry a forgery, never persist.
        val body = byteArrayOf(REKEY_ACK_HEADER.toByte(), 0x00, 0x01) + ByteArray(6) { 0x00 }
        var xor = 0
        for (b in body) xor = xor xor (b.toInt() and 0xFF)
        udp.inbound.emit(UdpDatagram(body + byteArrayOf(xor.toByte()), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("invalid response"), "reason=${failure.reason}")
        assertEquals(1, udp.sent.size, "no retry after a forgery")
        assertEquals(OLD_PSK_HEX, vault.loadPsk(device.id), "vault keeps the old key")
        assertTrue(repo.rekeySaves.isEmpty(), "Room untouched")
    }

    /**
     * Timeout fallback (Tier-2 owns the gate): after all budgets expire with
     * silence, the failure surfaces the repair path — vault, Room, and the
     * holder era are untouched, so drive keeps using the old key until the
     * gate (where the "session expired — please re-pair" copy fires).
     */
    @Test
    fun timeoutFallsBackToTier2Repair() = runTest {
        val device = bondedDevice(era = 0u, lastCounter = 99u)
        val repo = FakeDeviceRepository(device)
        val vault = FakeBondSecretStore().also { it.seed(device.id, OLD_PSK_HEX) }
        val udp = FakeRekeyUdp()
        val holder = DriveCounter(repo)
        val deferred = async { useCase(repo, vault, udp, holder, attemptTimeoutsMs = listOf(50, 50))() }
        runCurrent()

        advanceTimeBy(200)
        val result = deferred.await()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("no response"), "reason=${failure.reason}")
        assertEquals(2, udp.sent.size, "both budgets sent")
        assertEquals(OLD_PSK_HEX, vault.loadPsk(device.id), "vault keeps the old key")
        assertTrue(repo.rekeySaves.isEmpty(), "Room untouched")
        assertEquals(0u, holder.currentEra(), "holder keeps the old era")
    }

    @Test
    fun noDeviceOrKeyFailsWithoutSending() = runTest {
        // Unpaired: nothing leaves the radio.
        val emptyRepo = FakeDeviceRepository(null)
        val emptyUdp = FakeRekeyUdp()
        val emptyVault = FakeBondSecretStore()
        val emptyHolder = DriveCounter(emptyRepo)
        val unpaired = async { useCase(emptyRepo, emptyVault, emptyUdp, emptyHolder)() }
        runCurrent()
        assertIs<Outcome.Failure>(unpaired.await())
        assertTrue(emptyUdp.sent.isEmpty())

        // Paired but keyless: same (the repair copy owns this path).
        val device = bondedDevice()
        val keylessRepo = FakeDeviceRepository(device)
        val keylessUdp = FakeRekeyUdp()
        val keylessVault = FakeBondSecretStore()
        val keylessHolder = DriveCounter(keylessRepo)
        val keyless = async { useCase(keylessRepo, keylessVault, keylessUdp, keylessHolder)() }
        runCurrent()
        assertIs<Outcome.Failure>(keyless.await())
        assertTrue(keylessUdp.sent.isEmpty())
    }
}
