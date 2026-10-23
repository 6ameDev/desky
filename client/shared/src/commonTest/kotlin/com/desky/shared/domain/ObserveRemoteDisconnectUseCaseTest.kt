package com.desky.shared.domain

import app.cash.turbine.test
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val EVICT_PSK_HEX = "00112233445566778899aabbccddeeff"

private fun evictHex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

private fun evictDevice(
    era: UShort = 1u,
    lastCounter: UInt = 100u,
    lastRobotCounter: UInt = 0u,
) = PairedDevice(
    id = "desky-192.168.4.1",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = lastCounter,
    alias = ROBOT_HOST,
    persistEvery = 128,
    era = era,
    lastRobotCounter = lastRobotCounter,
)

/** Recompute the trailing XOR after mutating a frame (so shape passes, HMAC must fail). */
private fun fixXor(frame: ByteArray): ByteArray {
    var xor = 0
    for (i in 0 until frame.size - 1) xor = xor xor (frame[i].toInt() and 0xFF)
    frame[frame.size - 1] = xor.toByte()
    return frame
}

/** `PAIR_OK` built with REAL crypto (hkdf + aesGcmEncrypt) for [clientNonce]. */
private suspend fun evictPairOkFor(clientNonce: ByteArray): ByteArray {
    val serverNonce = byteArrayOf(0x05, 0x06, 0x07, 0x08)
    val kek = hkdfSha256(clientNonce + serverNonce, "123456".encodeToByteArray(), "desky-pair-v1", 16)
    val nonce12 = serverNonce + clientNonce + ByteArray(4)
    val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + clientNonce + serverNonce
    val plain = evictHex(EVICT_PSK_HEX) + byteArrayOf(128.toByte())
    val (ct, tag) = aesGcmEncrypt(kek, nonce12, aad, plain)
    val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

class ObserveRemoteDisconnectUseCaseTest {

    private fun setup(device: PairedDevice? = evictDevice(), seedVault: Boolean = true): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore()
        if (device != null && seedVault) vault.seed(device.id, EVICT_PSK_HEX)
        val udp = FakeUdpSocketManager()
        val toRobot = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val useCase = ObserveRemoteDisconnectUseCase(
            udp,
            repo,
            GetPairPskUseCase(vault, repo),
            vault,
            toRobot,
            fromRobot,
        )
        return Setup(repo, vault, udp, toRobot, fromRobot, useCase)
    }

    private data class Setup(
        val repo: FakeDeviceRepository,
        val vault: FakePairSecretStore,
        val udp: FakeUdpSocketManager,
        val toRobot: ToRobotCounter,
        val fromRobot: FromRobotCounter,
        val useCase: ObserveRemoteDisconnectUseCase,
    )

    private fun datagram(bytes: ByteArray) = UdpDatagram(bytes, ROBOT_HOST, DEFAULT_UDP_PORT)

    @Test
    fun goodDisconnectClearsVaultRoomAndBothCountersWithNoReply() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device)
        // Both holders live: the send domain advanced, the receive domain
        // holds the watermark from the Room row.
        s.toRobot.ensure(device.id, device.lastCounter)
        // Core sends counter last+1 under the paired PSK (real crypto).
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 101u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            awaitItem()
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertNull(s.repo.observePaired().first(), "Room row cleared")
        assertNull(s.vault.loadPsk(device.id), "vault PSK cleared")
        assertNull(s.toRobot.current(), "send counter era reset")
        assertNull(s.fromRobot.current(), "receive counter peer reset")
        assertEquals(listOf(device.id to 101u), s.repo.robotCounterSaves, "accepted counter persisted before clear")
        assertTrue(s.udp.sent.isEmpty(), "no reply packet ever sent")
    }

    /**
     * Bench regression: evictions verify against the RECEIVE domain, so a
     * send counter advanced far past the eviction stamp must not starve it
     * (the old core stamped evictions as last-received+1 on the app send
     * domain — 0/3 arrived once the live send counter moved past).
     */
    @Test
    fun sendCounterAdvancesPastEvictionStampStillAccepted() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device)
        // Busy drive loop: the send domain runs 5000 frames past the stamp.
        s.toRobot.ensure(device.id, 0u)
        repeat(5000) { s.toRobot.next() }
        // The eviction stamp (5, on the robot's own sequence) is fresh
        // against the receive baseline 0 regardless of the send domain.
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 5u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            awaitItem()
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertNull(s.repo.observePaired().first(), "eviction accepted despite send domain at 5000")
        assertNull(s.vault.loadPsk(device.id))
        assertTrue(s.udp.sent.isEmpty())
    }

    @Test
    fun duplicateEvictionDropsAsStaleWithPairIntact() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device)
        // A previous accept consumed 101 (persisted to the row, like the
        // commit path does); the burst duplicate carries the same counter.
        s.fromRobot.ensure(device.id, 0u)
        s.fromRobot.consume(101u)
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 101u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertEquals(
            device.copy(lastRobotCounter = 101u),
            s.repo.observePaired().first(),
            "pair untouched (duplicate is stale, not a second eviction)",
        )
        assertEquals(EVICT_PSK_HEX, s.vault.loadPsk(device.id), "vault untouched")
        assertEquals(listOf(device.id to 101u), s.repo.robotCounterSaves, "no re-persist on a stale drop")
        assertTrue(s.udp.sent.isEmpty())
    }

    @Test
    fun staleCounterIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 100u)
        val s = setup(device)
        // Counter == receive sequence: correctly signed but not fresh (replay).
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 100u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertEquals(device, s.repo.observePaired().first(), "pair untouched")
        assertEquals(EVICT_PSK_HEX, s.vault.loadPsk(device.id), "vault untouched")
    }

    @Test
    fun badTagIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device)
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 101u)
        frame[7] = (frame[7] + 1).toByte()
        fixXor(frame)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertEquals(device, s.repo.observePaired().first(), "pair untouched")
        assertEquals(EVICT_PSK_HEX, s.vault.loadPsk(device.id), "vault untouched")
        assertTrue(s.udp.sent.isEmpty())
    }

    @Test
    fun eraMismatchIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device)
        // Correctly signed under the PSK but for the wrong era.
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 2u, 101u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertEquals(device, s.repo.observePaired().first(), "pair untouched")
        assertEquals(EVICT_PSK_HEX, s.vault.loadPsk(device.id), "vault untouched")
    }

    @Test
    fun unpairedIgnored() = runTest {
        val s = setup(device = null, seedVault = false)
        // Well-formed eviction bytes, but nothing is paired (no PSK to verify under).
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 101u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertNull(s.repo.observePaired().first())
        assertTrue(s.udp.sent.isEmpty())
    }

    @Test
    fun nonDisconnectFramesPassThroughUntouched() = runTest {
        val device = evictDevice()
        val s = setup(device)
        val telemetry = TelemetryPacketCodec.encode(
            Telemetry(0f, 0f, 100, cliff = false, driving = false, mode = RobotMode.MANUAL),
        )

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(telemetry))
            s.udp.inbound.emit(datagram(ByteArray(0)))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        assertEquals(device, s.repo.observePaired().first())
    }

    @Test
    fun missingVaultPskIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val s = setup(device, seedVault = false)
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 101u)

        s.useCase.observe().test {
            s.udp.inbound.emit(datagram(frame))
            expectNoEvents()
            cancelAndIgnoreRemainingEvents()
        }

        // Room row survives (nothing cleared without a verified eviction).
        assertEquals(device, s.repo.observePaired().first())
    }

    /**
     * Reset on pair success: a stale receive baseline (same peer id as the
     * previous pairing) returns to 0, so the robot's post-pair counter 1
     * reads fresh.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun pairSuccessResetsReceiveDomain() = runTest {
        val repo = FakeDeviceRepository(null)
        val vault = FakePairSecretStore()
        val udp = FakeUdpSocketManager()
        val fromRobot = FromRobotCounter(repo)
        fromRobot.ensure("desky-192.168.4.1", 41u)
        val pairUC = PairDeviceWithCodeUseCase(repo, udp, vault, fromRobot)

        val deferred = async { pairUC("123456") }
        runCurrent()
        val clientNonce = udp.sent[0].bytes.copyOfRange(7, 11)
        udp.inbound.emit(UdpDatagram(evictPairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        assertIs<Outcome.Success<PairedDevice>>(result)
        assertNull(fromRobot.current(), "pair success resets the receive domain")
        assertEquals(0u, repo.observePaired().first()!!.lastRobotCounter, "fresh row carries the 0 baseline")
    }

    /** Reset on local forget: the receive domain returns to the 0 baseline. */
    @Test
    fun forgetResetsReceiveDomain() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u, lastRobotCounter = 0u)
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(device.id, EVICT_PSK_HEX) }
        val udp = FakeUdpSocketManager()
        val fromRobot = FromRobotCounter(repo)
        fromRobot.ensure(device.id, 0u)
        fromRobot.consume(50u)

        val result = ForgetDeviceUseCase(
            repo,
            vault,
            udp,
            GetPairPskUseCase(vault, repo),
            ToRobotCounter(repo),
            fromRobot,
        )()

        assertIs<Outcome.Success<Unit>>(result)
        assertNull(fromRobot.current(), "local forget resets the receive domain")
    }

    @Test
    fun freshCounterMirrorsFirmwareIdiom() {
        assertTrue(ObserveRemoteDisconnectUseCase.isFreshCounter(0u, 1u))
        assertTrue(ObserveRemoteDisconnectUseCase.isFreshCounter(41u, 42u))
        assertTrue(ObserveRemoteDisconnectUseCase.isFreshCounter(0xFFFFFFFFu, 0u), "wrap is fresh")
        assertTrue(ObserveRemoteDisconnectUseCase.isFreshCounter(0xFFFFFFFFu, 1u))
        assertTrue(ObserveRemoteDisconnectUseCase.isFreshCounter(0u, 0x7FFFFFFFu))
        assertFalse(ObserveRemoteDisconnectUseCase.isFreshCounter(7u, 7u), "equal is stale")
        assertFalse(ObserveRemoteDisconnectUseCase.isFreshCounter(10u, 9u), "older is stale")
        assertFalse(ObserveRemoteDisconnectUseCase.isFreshCounter(1u, 0u))
        assertFalse(ObserveRemoteDisconnectUseCase.isFreshCounter(0u, 0x80000000u), "half-range ambiguous")
        assertFalse(ObserveRemoteDisconnectUseCase.isFreshCounter(0u, 0x80000001u))
        // The use-case idiom delegates to the receive-domain holder.
        assertTrue(FromRobotCounter.isFresh(41u, 42u))
        assertFalse(FromRobotCounter.isFresh(7u, 7u))
    }

    @Test
    fun constantTimeEqualsIsLengthSensitive() {
        assertTrue(ObserveRemoteDisconnectUseCase.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 3)))
        assertFalse(ObserveRemoteDisconnectUseCase.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 4)))
        assertFalse(ObserveRemoteDisconnectUseCase.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2)))
    }

    @Test
    fun decodeDisconnectRoundTrip() = runTest {
        val frame = encodeDisconnect(evictHex(EVICT_PSK_HEX), 1u, 100u)
        // Golden DISCONNECT from shared/protocol/README.md (era=1, counter=100).
        assertEquals(DISCONNECT_FRAME_HEX, frame.toHex())
        val decoded = decodeDisconnect(frame)!!
        assertEquals(1u.toUShort(), decoded.first)
        assertEquals(100u, decoded.second)
        assertEquals(DISCONNECT_TAG_HEX, decoded.third.toHex())
    }

    @Test
    fun decodeDisconnectRejectsShape() {
        assertNull(decodeDisconnect(ByteArray(13)))
        assertNull(decodeDisconnect(ByteArray(15)))
        val wrongHeader = encodeAliveForShape()
        assertNull(decodeDisconnect(wrongHeader))
        val corrupt = evictHex(DISCONNECT_FRAME_HEX).also { it[3] = (it[3] + 1).toByte() }
        assertNull(decodeDisconnect(corrupt), "bad XOR drops")
    }

    companion object {
        private const val DISCONNECT_FRAME_HEX = "56000100000064ca90ca03904070"
        private const val DISCONNECT_TAG_HEX = "ca90ca039040"

        private fun ByteArray.toHex(): String {
            val digits = "0123456789abcdef"
            val chars = CharArray(size * 2)
            for (i in indices) {
                val v = this[i].toInt() and 0xFF
                chars[i * 2] = digits[v ushr 4]
                chars[i * 2 + 1] = digits[v and 0x0F]
            }
            return chars.concatToString()
        }

        private fun encodeAliveForShape(): ByteArray {
            // 14B ALIVE frame: right size, wrong header for a DISCONNECT decode.
            val body = byteArrayOf(0x55.toByte(), 0, 1, 0, 0, 0, 100, 1, 2, 3, 4, 5, 6)
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and 0xFF)
            return body + byteArrayOf(xor.toByte())
        }
    }
}
