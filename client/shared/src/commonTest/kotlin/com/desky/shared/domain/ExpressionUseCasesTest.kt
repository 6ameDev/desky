package com.desky.shared.domain

import com.desky.shared.presentation.FakeUdpSocketManager
import com.desky.shared.presentation.testDevice
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

private fun Byte.unsigned(): Int = this.toInt() and 0xFF

private fun xorOf(bytes: ByteArray, len: Int): Int {
    var sum = 0
    for (i in 0 until len) sum = sum xor bytes[i].unsigned()
    return sum
}

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

// Fixed inputs (independent python hmac reference: HMAC-SHA256(psk,
// "EXPRLIST"/"EXPRTRIG" || eraBE2 || counterBE4), first 6 bytes):
// psk=00112233445566778899aabbccddeeff, era=1, counter=100, id=3.
// NOTE: shared/protocol/README.md carries no expression vectors yet —
// these literals were computed with an independent python hmac
// implementation, not the firmware half.
private val PSK = hex("00112233445566778899aabbccddeeff")
private const val LIST_TAG_HEX = "926efe36104a"
private const val LIST_REQ_FRAME_HEX = "59000100000064926efe36104a52"
private const val TRIG_TAG_HEX = "611d596f4c56"
private const val TRIG_FRAME_HEX = "5b03000100000064611d596f4c566d"
// Hand-laid single-entry RESP: ver=1 count=1 (id=3, flags=1, "Wave"+6 blanks) + XOR.
private const val RESP_FRAME_HEX = "5a01010301576176652020202020207d"

class ExpressionCodecTest {

    @Test
    fun constants() {
        assertEquals(0x59, EXPR_LIST_REQ_HEADER)
        assertEquals(14, EXPR_LIST_REQ_SIZE)
        assertEquals(0x5A, EXPR_LIST_RESP_HEADER)
        assertEquals(0x5B, EXPR_TRIGGER_HEADER)
        assertEquals(15, EXPR_TRIGGER_SIZE)
    }

    @Test
    fun listTagMatchesIndependentVector() = runTest {
        // msg = "EXPRLIST" || eraBE2(0001) || counterBE4(00000064), 14 bytes.
        val tag = exprListTag(psk = PSK, era = 1u, counter = 100u)
        assertContentEquals(hex(LIST_TAG_HEX), tag)
    }

    @Test
    fun triggerTagMatchesIndependentVector() = runTest {
        // msg = "EXPRTRIG" || id(03) || eraBE2(0001) || counterBE4(00000064), 15 bytes.
        val tag = exprTriggerTag(psk = PSK, id = 3, era = 1u, counter = 100u)
        assertContentEquals(hex(TRIG_TAG_HEX), tag)
    }

    /**
     * Golden `EXPR_LIST_REQ` frame (psk=`0011...eeff`, era=1,
     * counter=100). The expected tag is built from a MANUALLY assembled
     * 14-byte HMAC input (`"EXPRLIST"` ASCII || eraBE2 || counterBE4)
     * through the [hmacSha256] primitive — never through
     * [encodeExprListReq] itself — and the frame bytes are laid out
     * literally around it.
     */
    @Test
    fun listReqFrameGolden() = runTest {
        val hmacInput = "EXPRLIST".encodeToByteArray() + hex("000100000064")
        assertEquals(14, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, 6)
        assertContentEquals(hex(LIST_TAG_HEX), tag)

        val body = byteArrayOf(0x59.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(13, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeExprListReq(PSK, 1u, 100u)

        assertEquals(EXPR_LIST_REQ_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (independent python reference).
        assertContentEquals(hex(LIST_REQ_FRAME_HEX), actual)
        assertEquals(xorOf(actual, EXPR_LIST_REQ_SIZE - 1), actual[EXPR_LIST_REQ_SIZE - 1].unsigned())
    }

    /**
     * Golden `EXPR_TRIGGER` frame (same fixed inputs, id=3). The expected
     * tag is built from a MANUALLY assembled 15-byte HMAC input
     * (`"EXPRTRIG"` ASCII || id || eraBE2 || counterBE4) through
     * [hmacSha256] — never through [encodeExprTrigger] itself.
     */
    @Test
    fun triggerFrameGolden() = runTest {
        val hmacInput = "EXPRTRIG".encodeToByteArray() + hex("03000100000064")
        assertEquals(15, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, 6)
        assertContentEquals(hex(TRIG_TAG_HEX), tag)

        val body = byteArrayOf(0x5B.toByte(), 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(14, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeExprTrigger(PSK, 3, 1u, 100u)

        assertEquals(EXPR_TRIGGER_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (independent python reference).
        assertContentEquals(hex(TRIG_FRAME_HEX), actual)
        assertEquals(xorOf(actual, EXPR_TRIGGER_SIZE - 1), actual[EXPR_TRIGGER_SIZE - 1].unsigned())
    }

    @Test
    fun eraCounterAndIdLayOutBigEndian() = runTest {
        val psk = ByteArray(16) { it.toByte() }
        val req = encodeExprListReq(psk, 0x0102u, 0x01020304u)
        assertEquals(0x59, req[0].unsigned())
        assertEquals(0x01, req[1].unsigned())
        assertEquals(0x02, req[2].unsigned())
        assertEquals(0x01, req[3].unsigned())
        assertEquals(0x02, req[4].unsigned())
        assertEquals(0x03, req[5].unsigned())
        assertEquals(0x04, req[6].unsigned())
        assertEquals(xorOf(req, EXPR_LIST_REQ_SIZE - 1), req[EXPR_LIST_REQ_SIZE - 1].unsigned())

        val trig = encodeExprTrigger(psk, 0xAB, 0x0102u, 0x01020304u)
        assertEquals(0x5B, trig[0].unsigned())
        assertEquals(0xAB, trig[1].unsigned())
        assertEquals(0x01, trig[2].unsigned())
        assertEquals(0x02, trig[3].unsigned())
        assertEquals(0x01, trig[4].unsigned())
        assertEquals(0x02, trig[5].unsigned())
        assertEquals(0x03, trig[6].unsigned())
        assertEquals(0x04, trig[7].unsigned())
        assertEquals(xorOf(trig, EXPR_TRIGGER_SIZE - 1), trig[EXPR_TRIGGER_SIZE - 1].unsigned())
    }

    @Test
    fun rejectsShortPskAndBadId() = runTest {
        assertFailsWith<IllegalArgumentException> {
            encodeExprListReq(ByteArray(8), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeExprTrigger(ByteArray(8), 3, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeExprTrigger(PSK, 256, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeExprTrigger(PSK, -1, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            exprListTag(ByteArray(8), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            exprTriggerTag(ByteArray(8), 3, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            exprTriggerTag(PSK, 256, 1u, 1u)
        }
    }

    @Test
    fun tagsDifferByIdEraCounterAndDomain() = runTest {
        val base = exprListTag(psk = PSK, era = 1u, counter = 100u)
        assertTrue(
            exprListTag(psk = PSK, era = 2u, counter = 100u).contentEquals(base).not(),
            "era is domain-separated",
        )
        assertTrue(
            exprListTag(psk = PSK, era = 1u, counter = 101u).contentEquals(base).not(),
            "counter is domain-separated",
        )
        assertTrue(
            exprTriggerTag(psk = PSK, id = 3, era = 1u, counter = 100u).contentEquals(base).not(),
            "LIST and TRIGGER domains differ",
        )
        assertTrue(
            exprTriggerTag(psk = PSK, id = 4, era = 1u, counter = 100u).contentEquals(
                exprTriggerTag(psk = PSK, id = 3, era = 1u, counter = 100u),
            ).not(),
            "id is domain-separated",
        )
        assertTrue(
            manualTakeTag(psk = PSK, era = 1u, counter = 100u).contentEquals(base).not(),
            "MANUAL_TAKE and EXPR_LIST domains differ",
        )
    }
}

class ExprListRespCodecTest {

    @Test
    fun emptyListRoundTrip() {
        val frame = ExprListRespCodec.encode(1, emptyList())
        assertEquals(4, frame.size)
        assertEquals(0x5A, frame[0].unsigned())
        assertEquals(1, frame[1].unsigned())
        assertEquals(0, frame[2].unsigned())
        assertEquals(xorOf(frame, 3), frame[3].unsigned())
        assertEquals(emptyList(), ExprListRespCodec.decode(frame))
    }

    @Test
    fun singleEntryGoldenBytesAndName() {
        // Flag bit0 set = play-once; name blank-padded to exactly 10B.
        val entries = listOf(ExpressionEntry(id = 3, flags = 1, name = "Wave"))
        val frame = ExprListRespCodec.encode(1, entries)
        assertContentEquals(hex(RESP_FRAME_HEX), frame)
        assertEquals(16, frame.size)

        val back = assertNotNull(ExprListRespCodec.decode(frame))
        assertEquals(1, back.size)
        assertEquals(3, back[0].id)
        assertEquals(1, back[0].flags)
        assertEquals("Wave", back[0].name)
        assertTrue(back[0].playOnce)
    }

    @Test
    fun namesPadToExactlyTenBytesAndTrimOnDecode() {
        val entries = listOf(
            ExpressionEntry(id = 0, flags = 0, name = "A"),
            ExpressionEntry(id = 255, flags = 0xFE, name = "0123456789"),
        )
        val frame = ExprListRespCodec.encode(1, entries)
        assertEquals(4 + 2 * 12, frame.size)
        // Short name padded with blanks (0x20), full name stored verbatim.
        assertContentEquals(
            "A         ".encodeToByteArray(),
            frame.copyOfRange(5, 15),
        )
        assertContentEquals(
            "0123456789".encodeToByteArray(),
            frame.copyOfRange(17, 27),
        )
        val back = assertNotNull(ExprListRespCodec.decode(frame))
        assertEquals("A", back[0].name)
        assertEquals("0123456789", back[1].name)
        assertEquals(false, back[0].playOnce)
        assertEquals(false, back[1].playOnce, "bit0 clear reads play-once false")
    }

    @Test
    fun maxCountRoundTrip() {
        val entries = List(12) { i -> ExpressionEntry(id = i, flags = i, name = "E$i") }
        val frame = ExprListRespCodec.encode(1, entries)
        assertEquals(148, frame.size)
        val back = assertNotNull(ExprListRespCodec.decode(frame))
        assertEquals(entries, back)
    }

    @Test
    fun rejectsBadHeader() {
        val frame = ExprListRespCodec.encode(1, listOf(ExpressionEntry(3, 1, "Wave")))
        frame[0] = 0x00
        assertNull(ExprListRespCodec.decode(frame))
        frame[0] = 0x59.toByte()
        assertNull(ExprListRespCodec.decode(frame))
    }

    @Test
    fun rejectsWrongLength() {
        assertNull(ExprListRespCodec.decode(byteArrayOf()))
        assertNull(ExprListRespCodec.decode(byteArrayOf(0x5A.toByte(), 0x01, 0x00)))
        // Declared count 1 but zero entry bytes (truncated).
        val truncated = hex(RESP_FRAME_HEX).copyOf(4)
        assertNull(ExprListRespCodec.decode(truncated))
        // Declared count 1 with two entries worth of bytes (trailing garbage).
        val padded = hex(RESP_FRAME_HEX) + ByteArray(12)
        assertNull(ExprListRespCodec.decode(padded))
        // Declared count 2 with one entry's bytes.
        val short = hex(RESP_FRAME_HEX).copyOf()
        short[2] = 0x02
        assertNull(ExprListRespCodec.decode(short))
    }

    @Test
    fun rejectsCorruptedChecksum() {
        val frame = ExprListRespCodec.encode(1, listOf(ExpressionEntry(3, 1, "Wave")))
        val badChecksum = frame.copyOf().also { it[it.size - 1] = (it[it.size - 1] + 1).toByte() }
        assertNull(ExprListRespCodec.decode(badChecksum))
        val badPayload = frame.copyOf().also { it[3] = (it[3] + 1).toByte() }
        assertNull(ExprListRespCodec.decode(badPayload))
    }

    @Test
    fun rejectsWrongVersionAndOutOfBoundsCount() {
        val frame = ExprListRespCodec.encode(1, listOf(ExpressionEntry(3, 1, "Wave")))
        frame[1] = 0x02
        // Recompute the XOR so shape passes and only the version must fail.
        var xor = 0
        for (i in 0 until frame.size - 1) xor = xor xor frame[i].unsigned()
        frame[frame.size - 1] = xor.toByte()
        assertNull(ExprListRespCodec.decode(frame), "ver != 1 drops")

        // Count 13 over-sizes the frame: build the bytes literally.
        val body = byteArrayOf(0x5A.toByte(), 0x01, 13.toByte()) + ByteArray(13 * 12)
        var xor13 = 0
        for (b in body) xor13 = xor13 xor b.unsigned()
        assertNull(ExprListRespCodec.decode(body + byteArrayOf(xor13.toByte())), "count > 12 drops")
    }

    @Test
    fun encodeRejectsOverCountAndLongName() {
        assertFailsWith<IllegalArgumentException> {
            ExprListRespCodec.encode(1, List(13) { i -> ExpressionEntry(i, 0, "E") })
        }
        assertFailsWith<IllegalArgumentException> {
            ExprListRespCodec.encode(1, listOf(ExpressionEntry(0, 0, "01234567890")))
        }
    }
}

class FetchExpressionsUseCaseTest {

    @Test
    fun sendsOneReqAndReturnsDecodedList() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val fetch = FetchExpressionsUseCase(udp)
        val entries = listOf(ExpressionEntry(3, 1, "Wave"), ExpressionEntry(7, 0, "Nod"))

        val deferred = async { fetch(device, PSK, 1u, 100u) }
        runCurrent()
        assertEquals(1, udp.sent.size, "single send per screen entry, no retry")
        val sent = udp.sent[0]
        assertContentEquals(hex(LIST_REQ_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)

        udp.inbound.emit(UdpDatagram(ExprListRespCodec.encode(1, entries), device.ip, device.port))
        val result = deferred.await()
        val success = assertIs<Outcome.Success<List<ExpressionEntry>>>(result)
        assertEquals(entries, success.data)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun ignoresNonMatchingFramesThenAccepts() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val fetch = FetchExpressionsUseCase(udp)
        val entries = listOf(ExpressionEntry(3, 1, "Wave"))

        val deferred = async { fetch(device, PSK, 1u, 100u) }
        runCurrent()
        // Telemetry + a corrupt-XOR 0x5A frame must not terminate the wait.
        val telemetry = TelemetryPacketCodec.encode(
            Telemetry(0f, 0f, 100, cliff = false, driving = false, mode = RobotMode.MANUAL),
        )
        udp.inbound.emit(UdpDatagram(telemetry, device.ip, device.port))
        val corrupt = ExprListRespCodec.encode(1, entries).also { it[5] = (it[5] + 1).toByte() }
        udp.inbound.emit(UdpDatagram(corrupt, device.ip, device.port))
        runCurrent()
        assertTrue(deferred.isActive, "mismatched frames keep the wait open")
        udp.inbound.emit(UdpDatagram(ExprListRespCodec.encode(1, entries), device.ip, device.port))
        val result = deferred.await()
        assertEquals(entries, assertIs<Outcome.Success<List<ExpressionEntry>>>(result).data)
        assertEquals(1, udp.sent.size, "still exactly one request")
    }

    @Test
    fun timeoutMapsToFailureWithOneSend() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val fetch = FetchExpressionsUseCase(udp, timeoutMs = 200)

        val result = fetch(device, PSK, 1u, 100u)

        assertIs<Outcome.Failure>(result)
        assertEquals(1, udp.sent.size, "the request still went out once")
    }

    @Test
    fun sendErrorMapsToFailureWithoutThrowing() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val fetch = FetchExpressionsUseCase(udp)

        val result = fetch(device, PSK, 1u, 100u)

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun cancellationPropagates() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = CancellationException("cancelled") }

        assertFailsWith<CancellationException> {
            FetchExpressionsUseCase(udp)(device, PSK, 1u, 100u)
        }
    }

    @Test
    fun rejectsNonPositiveTimeout() {
        assertFailsWith<IllegalArgumentException> {
            FetchExpressionsUseCase(FakeUdpSocketManager(), timeoutMs = 0)
        }
    }
}

class TriggerExpressionUseCaseTest {

    @Test
    fun sendsOneTriggerToPairedEndpoint() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, 3, setOf(3, 7))

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, udp.sent.size, "single send per tap")
        val sent = udp.sent[0]
        assertContentEquals(hex(TRIG_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)
    }

    @Test
    fun unknownIdRejectedWithNoSend() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, 9, setOf(3, 7))

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty(), "unknown ids never reach the wire")
    }

    @Test
    fun sendErrorMapsToFailureWithoutThrowing() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, 3, setOf(3))

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun cancellationPropagates() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = CancellationException("cancelled") }

        assertFailsWith<CancellationException> {
            TriggerExpressionUseCase(udp)(device, PSK, 1u, 100u, 3, setOf(3))
        }
    }
}
