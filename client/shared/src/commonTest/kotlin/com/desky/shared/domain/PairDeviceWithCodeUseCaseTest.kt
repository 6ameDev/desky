package com.desky.shared.domain

import com.desky.shared.presentation.FakePairSecretStore

import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest

private val CODE = "123456".encodeToByteArray()
private val SERVER_NONCE = byteArrayOf(0x05, 0x06, 0x07, 0x08)
private const val PSK_HEX = "00112233445566778899aabbccddeeff"

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

/** Hand-written fake: records sends, replays queued inbound datagrams. */
private class FakePairingUdp : UdpSocketManager {
    val sent = mutableListOf<UdpDatagram>()
    val inbound = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = inbound
    var sendError: Throwable? = null

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sendError?.let { throw it }
        sent += UdpDatagram(bytes, host, port)
    }

    override suspend fun close() = Unit
}

/** `PAIR_OK` built with REAL crypto (hkdf + aesGcmEncrypt) for [clientNonce]. */
private suspend fun pairOkFor(
    clientNonce: ByteArray,
    code: ByteArray = CODE,
    serverNonce: ByteArray = SERVER_NONCE,
    psk: ByteArray = hex(PSK_HEX),
    /**
     * Interval byte appended to the GCM plaintext (v1.1 39B frame).
     * Null builds the legacy v1 38B frame (no interval byte — old robot).
     */
    persistEvery: Int? = 128,
): ByteArray {
    val kek = hkdfSha256(clientNonce + serverNonce, code, "desky-pair-v1", 16)
    val nonce12 = serverNonce + clientNonce + ByteArray(4)
    val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + clientNonce + serverNonce
    val plain = if (persistEvery == null) psk else psk + byteArrayOf((persistEvery and 0xFF).toByte())
    val (ct, tag) = aesGcmEncrypt(kek, nonce12, aad, plain)
    return framePairOk(serverNonce, ct, tag)
}

private fun framePairOk(serverNonce: ByteArray, ct: ByteArray, tag: ByteArray): ByteArray {
    val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

private fun pairErr(reason: Int, retryS: Int): ByteArray {
    val body = byteArrayOf(PAIR_ERR_HEADER.toByte(), reason.toByte(), retryS.toByte())
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

@OptIn(ExperimentalCoroutinesApi::class)
class PairDeviceWithCodeUseCaseTest {

    private fun repo() = mock<DeviceRepository>()

    private fun useCase(
        deviceRepo: DeviceRepository,
        udp: FakePairingUdp,
        vault: FakePairSecretStore = FakePairSecretStore(),
        fromRobot: FromRobotCounter = FromRobotCounter(deviceRepo),
        attemptTimeoutsMs: List<Long> = DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS,
    ) = PairDeviceWithCodeUseCase(deviceRepo, udp, vault, fromRobot, attemptTimeoutsMs)

    /** Starts pairing and pumps until the initial PAIR_REQ is on the wire. */
    private fun TestScope.startPairing(
        udp: FakePairingUdp,
        useCase: PairDeviceWithCodeUseCase,
        code: String = "123456",
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): Deferred<Outcome<PairedDevice>> {
        val deferred = async { useCase(code, onProgress) }
        runCurrent()
        return deferred
    }

    /**
     * Replies to the in-flight pairing: pumps until PAIR_REQ is sent, queues
     * [reply] (the collector is active by then — a hot flow with no replay
     * drops emissions nobody is listening for, like the real socket), and
     * awaits the outcome.
     */
    private suspend fun TestScope.pairWithReply(
        udp: FakePairingUdp,
        useCase: PairDeviceWithCodeUseCase,
        reply: ByteArray,
        code: String = "123456",
    ): Outcome<PairedDevice> {
        val deferred = startPairing(udp, useCase, code)
        assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")
        udp.inbound.emit(UdpDatagram(reply, ROBOT_HOST, DEFAULT_UDP_PORT))
        return deferred.await()
    }

    private fun clientNonceOf(udp: FakePairingUdp): ByteArray {
        assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")
        val req = udp.sent[0]
        assertEquals(ROBOT_HOST, req.host)
        assertEquals(DEFAULT_UDP_PORT, req.port)
        assertEquals(PAIR_REQ_SIZE, req.bytes.size)
        assertContentEquals(CODE, req.bytes.copyOfRange(1, 7))
        return req.bytes.copyOfRange(7, 11)
    }

    @Test
    fun successUnwrapsPskWithRealCrypto() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val udp = FakePairingUdp()
        val vault = FakePairSecretStore()
        val deferred = startPairing(udp, useCase(deviceRepo, udp, vault))

        val clientNonce = clientNonceOf(udp)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        val outcome = assertIs<Outcome.Success<PairedDevice>>(result)
        assertEquals("desky-192.168.4.1", outcome.data.id)
        assertEquals(ROBOT_HOST, outcome.data.ip)
        assertEquals(DEFAULT_UDP_PORT, outcome.data.port)
        assertEquals("psk-v1", outcome.data.authToken)
        // Room never holds key material (no pskHex column since Task 9);
        // the counter resets so the first drive packet is 1; the vault
        // holds the key. Default pairOkFor carries interval byte 128.
        assertEquals(0u, outcome.data.lastCounter)
        assertEquals(128, outcome.data.persistEvery)
        assertEquals(PSK_HEX, vault.loadPsk(outcome.data.id))
        assertEquals(ROBOT_HOST, outcome.data.alias)
        verifySuspend { deviceRepo.save(outcome.data) }
        // No re-send was needed: exactly one PAIR_REQ on the wire.
        assertEquals(1, udp.sent.size)
    }

    @Test
    fun successCarriesNegotiatedPersistEvery() = runTest {
        for ((wire, expected) in listOf(16 to 16, 128 to 128, 0 to 128, 7 to 128)) {
            val deviceRepo = repo()
            everySuspend { deviceRepo.save(any()) } returns Unit
            val udp = FakePairingUdp()
            val vault = FakePairSecretStore()
            val deferred = startPairing(udp, useCase(deviceRepo, udp, vault))

            val clientNonce = clientNonceOf(udp)
            udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce, persistEvery = wire), ROBOT_HOST, DEFAULT_UDP_PORT))
            val result = deferred.await()

            val outcome = assertIs<Outcome.Success<PairedDevice>>(result, "wire=$wire")
            assertEquals(expected, outcome.data.persistEvery, "wire=$wire")
            // The vault holds the unwrapped 16-byte PSK regardless of frame size.
            assertEquals(PSK_HEX, vault.loadPsk(outcome.data.id), "wire=$wire")
        }
    }

    @Test
    fun legacyFrameWithoutIntervalDefaultsTo128() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val udp = FakePairingUdp()
        val vault = FakePairSecretStore()
        val deferred = startPairing(udp, useCase(deviceRepo, udp, vault))

        // v1 robot: 38B frame, no interval byte.
        val reply = pairOkFor(clientNonceOf(udp), persistEvery = null)
        assertEquals(PAIR_OK_SIZE, reply.size)
        udp.inbound.emit(UdpDatagram(reply, ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        val outcome = assertIs<Outcome.Success<PairedDevice>>(result)
        assertEquals(128, outcome.data.persistEvery)
        assertEquals(0u, outcome.data.lastCounter)
        assertEquals(PSK_HEX, vault.loadPsk(outcome.data.id))
    }

    @Test
    fun garbageDatagramsAreIgnoredBeforePairOk() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val udp = FakePairingUdp()
        val vault = FakePairSecretStore()
        val deferred = startPairing(udp, useCase(deviceRepo, udp, vault))

        val clientNonce = clientNonceOf(udp)
        // Telemetry-shaped traffic and junk share the hot flow; none match.
        udp.inbound.emit(UdpDatagram(ByteArray(9) { 0xBB.toByte() }, ROBOT_HOST, DEFAULT_UDP_PORT))
        udp.inbound.emit(UdpDatagram(byteArrayOf(0x51, 0x01), ROBOT_HOST, DEFAULT_UDP_PORT))
        runCurrent()
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))

        val outcome = assertIs<Outcome.Success<PairedDevice>>(deferred.await())
        assertEquals(0u, outcome.data.lastCounter)
        assertEquals(PSK_HEX, vault.loadPsk(outcome.data.id))
        assertEquals(1, udp.sent.size)
    }

    @Test
    fun pairErrReasonsMapToMessages() = runTest {
        val cases = listOf(
            Triple(0, 0, "incorrect code"),
            Triple(1, 30, "locked"),
            Triple(2, 0, "expired"),
            Triple(3, 0, "already paired"),
            Triple(4, 0, "robot error, try a fresh code"),
        )
        for ((reason, retryS, fragment) in cases) {
            val deviceRepo = repo()
            val udp = FakePairingUdp()

            val result = pairWithReply(udp, useCase(deviceRepo, udp), pairErr(reason, retryS))

            val failure = assertIs<Outcome.Failure>(result, "reason=$reason")
            assertTrue(failure.reason.contains(fragment), "reason=$reason -> ${failure.reason}")
            assertEquals(1, udp.sent.size, "no re-send on PAIR_ERR (reason=$reason)")
        }
    }

    @Test
    fun lockedMessageIncludesRetrySeconds() = runTest {
        val udp = FakePairingUdp()

        val result = pairWithReply(udp, useCase(repo(), udp), pairErr(1, 30))

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("30"), failure.reason)
    }

    @Test
    fun unknownReasonSurfacesCode() = runTest {
        val udp = FakePairingUdp()

        val result = pairWithReply(udp, useCase(repo(), udp), pairErr(9, 0))

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("reason=9"), failure.reason)
    }

    @Test
    fun reason4CopyDistinctFromExpired() = runTest {
        // Direct pair: reason 4 (burn-failure) vs reason 2 (genuine expiry).
        val udp4 = FakePairingUdp()
        val r4 = pairWithReply(udp4, useCase(repo(), udp4), pairErr(4, 0))
        val f4 = assertIs<Outcome.Failure>(r4)
        assertTrue(f4.reason.contains("robot error, try a fresh code"), f4.reason)
        assertTrue(!f4.reason.contains("expired"), "reason 4 must not read as expired: ${f4.reason}")
        val udp2 = FakePairingUdp()
        val r2 = pairWithReply(udp2, useCase(repo(), udp2), pairErr(2, 0))
        val f2 = assertIs<Outcome.Failure>(r2)
        assertTrue(f2.reason.contains("expired"), f2.reason)
        assertEquals(1, udp4.sent.size, "no re-send on PAIR_ERR reason=4")
    }

    @Test
    fun oldAppShowsGenericForReason4() = runTest {
        // Interop old-app x new-robot: an old peer decodes any reason byte
        // (same 4B framing) but maps only 0-3, so 4 must fall to generic.
        // Replicate the pre-M11 mapping here (decode-level, real framing).
        fun oldMap(reason: Int, retryS: Int): String = when (reason) {
            0 -> "Pairing failed: incorrect code"
            1 -> "Pairing failed: robot locked, retry in ${retryS}s"
            2 -> "Pairing failed: code expired"
            3 -> "Pairing failed: robot already paired"
            else -> "Pairing failed: robot rejected pairing (reason=$reason)"
        }
        val frame = pairErr(4, 0)
        val decoded = PairingPacketCodec.decodePairErr(frame)
        assertTrue(decoded != null, "old app decodes the 4B frame")
        val (reason, retryS) = requireNotNull(decoded)
        assertEquals(4, reason)
        val copy = oldMap(reason, retryS)
        assertTrue(copy.contains("reason=4"), copy)
        assertTrue(!copy.contains("robot error"), "old app has no reason-4 copy: $copy")
    }

    @Test
    fun timeoutSendsFiveTimesByDefaultThenFails() = runTest {
        val udp = FakePairingUdp()

        val result = useCase(repo(), udp)("123456")

        val failure = assertIs<Outcome.Failure>(result)
        assertEquals("No response from robot", failure.reason)
        // Default schedule is 5 sends (1s+2s+4s+4s+4s = 15s total, well
        // inside the firmware 60s redelivery window): never more, or the
        // firmware 5-try lockout could trip on our own retries.
        assertEquals(5, udp.sent.size)
        for (i in 1 until udp.sent.size) {
            assertContentEquals(udp.sent[0].bytes, udp.sent[i].bytes)
        }
    }

    @Test
    fun invalidSchedulesRejected() = runTest {
        val udp = FakePairingUdp()
        val vault = FakePairSecretStore()
        assertFailsWith<IllegalArgumentException> {
            PairDeviceWithCodeUseCase(repo(), udp, vault, FromRobotCounter(repo()), emptyList())
        }
        assertFailsWith<IllegalArgumentException> {
            PairDeviceWithCodeUseCase(repo(), udp, vault, FromRobotCounter(repo()), listOf(10, 20, 30, 40, 50, 60))
        }
        assertFailsWith<IllegalArgumentException> {
            PairDeviceWithCodeUseCase(repo(), udp, vault, FromRobotCounter(repo()), listOf(10, 0))
        }
        assertFailsWith<IllegalArgumentException> {
            PairDeviceWithCodeUseCase(repo(), udp, vault, FromRobotCounter(repo()), listOf(-5))
        }
    }

    @Test
    fun errRepliesNeverRetryAndReportProgressOnce() = runTest {
        val malformedErr = byteArrayOf(PAIR_ERR_HEADER.toByte(), 0, 0, 0xFF.toByte())
        val replies = listOf(
            pairErr(0, 0),
            pairErr(1, 30),
            pairErr(2, 0),
            pairErr(3, 0),
            pairErr(4, 0),
            malformedErr,
        )
        for (reply in replies) {
            val udp = FakePairingUdp()
            val progress = mutableListOf<Pair<Int, Int>>()
            val deferred = async {
                useCase(repo(), udp)("123456") { a, m -> progress += a to m }
            }
            runCurrent()
            assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")
            udp.inbound.emit(UdpDatagram(reply, ROBOT_HOST, DEFAULT_UDP_PORT))
            val result = deferred.await()

            assertIs<Outcome.Failure>(result, "reply=${reply.contentToString()}")
            assertEquals(1, udp.sent.size, "no re-send on ANY reply")
            assertEquals(listOf(1 to 5), progress, "onProgress fires exactly once (attempt 1)")
        }
    }

    @Test
    fun silenceRetriesShortScheduleWithByteIdenticalSends() = runTest {
        val schedule = listOf(50L, 100L, 150L)
        val udp = FakePairingUdp()
        val progress = mutableListOf<Int>()
        val uc = useCase(repo(), udp, attemptTimeoutsMs = schedule)

        val result = uc("123456", onProgress = { a, _ -> progress += a })

        val failure = assertIs<Outcome.Failure>(result)
        assertEquals("No response from robot", failure.reason)
        assertEquals(3, udp.sent.size)
        assertContentEquals(udp.sent[0].bytes, udp.sent[1].bytes)
        assertContentEquals(udp.sent[1].bytes, udp.sent[2].bytes)
        assertEquals(listOf(1, 2, 3), progress)
    }

    @Test
    fun singleEntryScheduleSendsOnce() = runTest {
        val udp = FakePairingUdp()
        val progress = mutableListOf<Int>()
        val uc = useCase(repo(), udp, attemptTimeoutsMs = listOf(10L))

        val result = uc("123456", onProgress = { a, _ -> progress += a })

        assertIs<Outcome.Failure>(result)
        assertEquals(1, udp.sent.size, "schedule of 1 caps sends at 1")
        assertEquals(listOf(1), progress)
    }

    @Test
    fun successOnSecondSendAfterOneTimeout() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val udp = FakePairingUdp()
        val vault = FakePairSecretStore()
        val progress = mutableListOf<Int>()
        val deferred = startPairing(udp, useCase(deviceRepo, udp, vault)) { a, _ -> progress += a }
        assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")

        // First 1s window expires with no reply -> exactly one re-send.
        advanceTimeBy(1000)
        runCurrent()
        assertEquals(2, udp.sent.size, "one re-send after the first timeout")
        // The re-send is byte-identical (same code+nonce): idempotent, so a
        // duplicated request the robot already answered just re-triggers the
        // same reply and can never burn an extra pairing try.
        assertContentEquals(udp.sent[0].bytes, udp.sent[1].bytes)

        val clientNonce = udp.sent[0].bytes.copyOfRange(7, 11)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        val result = deferred.await()

        val outcome = assertIs<Outcome.Success<PairedDevice>>(result)
        assertEquals(PSK_HEX, vault.loadPsk(outcome.data.id))
        verifySuspend { deviceRepo.save(outcome.data) }
        assertEquals(2, udp.sent.size, "no further sends after the reply")
        assertEquals(listOf(1, 2), progress, "success on 2nd send stops the loop")
    }

    @Test
    fun tamperedCiphertextFailsUnwrap() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val udp = FakePairingUdp()
        val deferred = startPairing(udp, useCase(deviceRepo, udp))

        val clientNonce = clientNonceOf(udp)
        val tampered = pairOkFor(clientNonce).copyOf().also {
            // Flip a ct byte, then repair the XOR so the frame decodes and
            // the failure lands in GCM authentication, not the checksum.
            it[5] = (it[5].toInt() xor 0xFF).toByte()
            var xor = 0
            for (i in 0 until it.size - 1) xor = xor xor (it[i].toInt() and 0xFF)
            it[it.size - 1] = xor.toByte()
        }
        udp.inbound.emit(UdpDatagram(tampered, ROBOT_HOST, DEFAULT_UDP_PORT))

        val result = deferred.await()
        val failure = assertIs<Outcome.Failure>(result)
        assertEquals("Pairing failed: could not unwrap key", failure.reason)
    }

    @Test
    fun invalidCodesFailWithoutSending() = runTest {
        val cases = listOf(
            "",
            "12345", // too short
            "1234567", // too long
            "12345a", // non-digit
            "abcdef",
            "12 456", // inner space is not a digit
            "12.456",
            "１２３４５６", // full-width digits are not ASCII
            " 1234 ", // trims to a short code
        )
        for (code in cases) {
            val udp = FakePairingUdp()
            val result = useCase(repo(), udp)(code)
            val failure = assertIs<Outcome.Failure>(result, "code=$code")
            assertTrue(failure.reason.isNotBlank(), "code=$code")
            assertTrue(udp.sent.isEmpty(), "no PAIR_REQ for invalid code=$code")
        }
    }

    @Test
    fun sendThrowingMapsToFailure() = runTest {
        val udp = FakePairingUdp().apply {
            sendError = IllegalStateException("socket down")
        }

        val result = useCase(repo(), udp)("123456")

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }

    @Test
    fun saveThrowingMapsToFailure() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } calls { throw IllegalStateException("disk gone") }
        val udp = FakePairingUdp()
        val deferred = startPairing(udp, useCase(deviceRepo, udp))

        val clientNonce = clientNonceOf(udp)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))

        val result = deferred.await()
        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }
}
