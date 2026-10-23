package com.desky.shared.domain

import app.cash.turbine.test
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val EVICT_PSK_HEX = "00112233445566778899aabbccddeeff"

private fun evictHex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

private fun evictDevice(era: UShort = 1u, lastCounter: UInt = 100u) = PairedDevice(
    id = "desky-192.168.4.1",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = lastCounter,
    alias = ROBOT_HOST,
    persistEvery = 128,
    era = era,
)

/** Recompute the trailing XOR after mutating a frame (so shape passes, HMAC must fail). */
private fun fixXor(frame: ByteArray): ByteArray {
    var xor = 0
    for (i in 0 until frame.size - 1) xor = xor xor (frame[i].toInt() and 0xFF)
    frame[frame.size - 1] = xor.toByte()
    return frame
}

class ObserveRemoteDisconnectUseCaseTest {

    private fun setup(device: PairedDevice? = evictDevice(), seedVault: Boolean = true): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore()
        if (device != null && seedVault) vault.seed(device.id, EVICT_PSK_HEX)
        val udp = FakeUdpSocketManager()
        val holder = DriveCounter(repo)
        val useCase = ObserveRemoteDisconnectUseCase(udp, repo, GetPairPskUseCase(vault, repo), vault, holder)
        return Setup(repo, vault, udp, holder, useCase)
    }

    private data class Setup(
        val repo: FakeDeviceRepository,
        val vault: FakePairSecretStore,
        val udp: FakeUdpSocketManager,
        val holder: DriveCounter,
        val useCase: ObserveRemoteDisconnectUseCase,
    )

    private fun datagram(bytes: ByteArray) = UdpDatagram(bytes, ROBOT_HOST, DEFAULT_UDP_PORT)

    @Test
    fun goodDisconnectClearsVaultRoomAndCounterWithNoReply() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u)
        val s = setup(device)
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
        assertNull(s.holder.current(), "counter era reset")
        assertTrue(s.udp.sent.isEmpty(), "no reply packet ever sent")
    }

    @Test
    fun badTagIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u)
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
    fun staleCounterIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u)
        val s = setup(device)
        // Counter == live sequence: correctly signed but not fresh (replay).
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
    fun eraMismatchIgnored() = runTest {
        val device = evictDevice(era = 1u, lastCounter = 100u)
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
        val device = evictDevice(era = 1u, lastCounter = 100u)
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
