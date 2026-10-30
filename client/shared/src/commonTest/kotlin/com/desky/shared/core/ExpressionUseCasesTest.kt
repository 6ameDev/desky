package com.desky.shared.core

import com.desky.shared.presentation.FakeUdpSocketManager
import com.desky.shared.presentation.testDevice
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

private const val BYTE_MASK = 0xFF
private const val HEX_RADIX = 16
private const val BLINK_EXPRESSION_ID = 3
private const val SURPRISED_EXPRESSION_ID = 7
private const val UNKNOWN_EXPRESSION_ID = 5
private const val DISALLOWED_EXPRESSION_ID = 9
private const val INVALID_EXPRESSION_ID = 256
private const val SHORT_PSK_SIZE = 8
private const val PSK_BYTE_LENGTH = 16
private const val AUTH_TAG_SIZE = 6
private const val TRIGGER_HMAC_MSG_SIZE = 15
private const val EXPECTED_LIST_REQ_HEADER = 0x59
private const val EXPECTED_LIST_RESP_HEADER = 0x5A
private const val EXPECTED_TRIGGER_HEADER = 0x5B
private const val EXPECTED_TRIGGER_SIZE = 15

private fun Byte.unsigned(): Int = this.toInt() and BYTE_MASK

private fun xorOf(bytes: ByteArray, len: Int): Int {
    var sum = 0
    for (i in 0 until len) sum = sum xor bytes[i].unsigned()
    return sum
}

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(HEX_RADIX).toByte()
    }
}

// Fixed inputs (independent python hmac reference: HMAC-SHA256(psk,
// "EXPRTRIG" || id || eraBE2 || counterBE4), first 6 bytes):
// psk=00112233445566778899aabbccddeeff, era=1, counter=100, id=3.
// NOTE: shared/protocol/README.md carries no expression vectors yet —
// these literals were computed with an independent python hmac
// implementation, not the firmware half.
private val PSK = hex("00112233445566778899aabbccddeeff")
private const val TRIG_TAG_HEX = "611d596f4c56"
private const val TRIG_FRAME_HEX = "5b03000100000064611d596f4c566d"

class ExpressionCodecTest {

    @Test
    fun constants() {
        // 0x59/0x5A stay claimed retired-reserved (never reuse); 0x5B is live.
        assertEquals(EXPECTED_LIST_REQ_HEADER, EXPR_LIST_REQ_HEADER)
        assertEquals(EXPECTED_LIST_RESP_HEADER, EXPR_LIST_RESP_HEADER)
        assertEquals(EXPECTED_TRIGGER_HEADER, EXPR_TRIGGER_HEADER)
        assertEquals(EXPECTED_TRIGGER_SIZE, EXPR_TRIGGER_SIZE)
    }

    @Test
    fun triggerTagMatchesIndependentVector() = runTest {
        // msg = "EXPRTRIG" || id(03) || eraBE2(0001) || counterBE4(00000064), 15 bytes.
        val tag = exprTriggerTag(psk = PSK, id = BLINK_EXPRESSION_ID, era = 1u, counter = 100u)
        assertContentEquals(hex(TRIG_TAG_HEX), tag)
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
        assertEquals(TRIGGER_HMAC_MSG_SIZE, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, AUTH_TAG_SIZE)
        assertContentEquals(hex(TRIG_TAG_HEX), tag)

        val body = byteArrayOf(0x5B.toByte(), 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(EXPR_TRIGGER_SIZE - 1, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeExprTrigger(PSK, BLINK_EXPRESSION_ID, 1u, 100u)

        assertEquals(EXPR_TRIGGER_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (independent python reference).
        assertContentEquals(hex(TRIG_FRAME_HEX), actual)
        assertEquals(xorOf(actual, EXPR_TRIGGER_SIZE - 1), actual[EXPR_TRIGGER_SIZE - 1].unsigned())
    }

    @Test
    fun eraCounterAndIdLayOutBigEndian() = runTest {
        val psk = ByteArray(PSK_BYTE_LENGTH) { it.toByte() }
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
            encodeExprTrigger(ByteArray(SHORT_PSK_SIZE), BLINK_EXPRESSION_ID, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeExprTrigger(PSK, INVALID_EXPRESSION_ID, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeExprTrigger(PSK, -1, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            exprTriggerTag(ByteArray(SHORT_PSK_SIZE), BLINK_EXPRESSION_ID, 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            exprTriggerTag(PSK, INVALID_EXPRESSION_ID, 1u, 1u)
        }
    }

    @Test
    fun tagsDifferByIdEraCounterAndDomain() = runTest {
        val base = exprTriggerTag(psk = PSK, id = BLINK_EXPRESSION_ID, era = 1u, counter = 100u)
        assertTrue(
            exprTriggerTag(psk = PSK, id = BLINK_EXPRESSION_ID, era = 2u, counter = 100u).contentEquals(base).not(),
            "era is domain-separated",
        )
        assertTrue(
            exprTriggerTag(psk = PSK, id = BLINK_EXPRESSION_ID, era = 1u, counter = 101u).contentEquals(base).not(),
            "counter is domain-separated",
        )
        assertTrue(
            exprTriggerTag(psk = PSK, id = 4, era = 1u, counter = 100u).contentEquals(base).not(),
            "id is domain-separated",
        )
        assertTrue(
            manualTakeTag(psk = PSK, era = 1u, counter = 100u).contentEquals(base).not(),
            "MANUAL_TAKE and EXPR_TRIGGER domains differ",
        )
    }
}

/**
 * Static-catalog pin: [STATIC_EXPRESSIONS] must mirror the shared firmware
 * catalog (`embedded/shared/link/expression_catalog.h`) id + flags + name
 * exactly. Update this test if the catalog ever grows (additive only —
 * never renumber, never reorder, never change a shipped row).
 */
class StaticExpressionCatalogTest {

    @Test
    fun mirrorsSharedCatalogExactly() {
        val expected = listOf(
            ExpressionEntry(id = 0, flags = 0, name = "boot"),
            ExpressionEntry(id = 1, flags = 0, name = "happy"),
            ExpressionEntry(id = 2, flags = 0, name = "sad"),
            ExpressionEntry(id = 3, flags = 0, name = "blink"),
            ExpressionEntry(id = 4, flags = 0, name = "alert"),
            ExpressionEntry(id = 6, flags = 0, name = "curious"),
            ExpressionEntry(id = 7, flags = 1, name = "surprised"),
            ExpressionEntry(id = 8, flags = 0, name = "angry"),
            ExpressionEntry(id = 9, flags = 0, name = "scared"),
            ExpressionEntry(id = 10, flags = 0, name = "excited"),
            ExpressionEntry(id = 11, flags = 0, name = "confused"),
        )
        assertEquals(expected, STATIC_EXPRESSIONS)
        assertEquals(expected.map { it.id }.toSet(), STATIC_EXPRESSION_IDS)
    }

    @Test
    fun idsUniqueAndPlayOnceOnlyOnSurprised() {
        val ids = STATIC_EXPRESSIONS.map { it.id }
        assertEquals(ids.size, ids.toSet().size, "catalog ids unique")
        assertEquals(listOf(SURPRISED_EXPRESSION_ID), STATIC_EXPRESSIONS.filter { it.playOnce }.map { it.id })
    }
}

class TriggerExpressionUseCaseTest {

    @Test
    fun sendsOneTriggerToPairedEndpoint() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, BLINK_EXPRESSION_ID, setOf(BLINK_EXPRESSION_ID, SURPRISED_EXPRESSION_ID))

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, udp.sent.size, "single send per tap")
        val sent = udp.sent[0]
        assertContentEquals(hex(TRIG_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)
    }

    @Test
    fun defaultsToStaticAllowList() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val trigger = TriggerExpressionUseCase(udp)

        // Id 0 is in the static catalog: no explicit allow-list needed.
        val ok = trigger(device, PSK, 1u, 100u, 0)
        assertIs<Outcome.Success<Unit>>(ok)
        assertEquals(1, udp.sent.size)
        assertEquals(0, udp.sent[0].bytes[1].unsigned())

        // Id 5 is the firmware QR gap (no catalog row): rejected by default.
        val bad = trigger(device, PSK, 1u, 101u, UNKNOWN_EXPRESSION_ID)
        assertIs<Outcome.Failure>(bad)
        assertEquals(1, udp.sent.size, "unknown ids never reach the wire")
    }

    @Test
    fun unknownIdRejectedWithNoSend() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, DISALLOWED_EXPRESSION_ID, setOf(BLINK_EXPRESSION_ID, SURPRISED_EXPRESSION_ID))

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty(), "unknown ids never reach the wire")
    }

    @Test
    fun sendErrorMapsToFailureWithoutThrowing() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val trigger = TriggerExpressionUseCase(udp)

        val result = trigger(device, PSK, 1u, 100u, BLINK_EXPRESSION_ID, setOf(BLINK_EXPRESSION_ID))

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun cancellationPropagates() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = CancellationException("cancelled") }

        assertFailsWith<CancellationException> {
            TriggerExpressionUseCase(udp)(device, PSK, 1u, 100u, BLINK_EXPRESSION_ID, setOf(BLINK_EXPRESSION_ID))
        }
    }
}
