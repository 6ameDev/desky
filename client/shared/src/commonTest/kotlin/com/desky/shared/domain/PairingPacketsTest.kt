package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val BYTE_MASK = 0xFF
private const val HEX_RADIX = 16
private const val PSK_BYTE_LENGTH = 16
private const val AUTH_TAG_SIZE = 6
private const val SHORT_PSK_SIZE = 8
private const val ROBOT_ERROR_REASON = 4
private const val LOCKED_RETRY_SECONDS = 30
private const val EXPECTED_PAIR_REQ_HEADER = 0x50
private const val EXPECTED_PAIR_OK_HEADER = 0x51
private const val EXPECTED_PAIR_ERR_HEADER = 0x52
private const val EXPECTED_PAIR_REQ_SIZE = 12
private const val EXPECTED_PAIR_OK_SIZE = 38
private const val EXPECTED_PAIR_OK_SIZE_V11 = 39
private const val EXPECTED_PAIR_ERR_SIZE = 4
private const val EXPECTED_PAIR_REFRESH_REQ_HEADER = 0x53
private const val EXPECTED_PAIR_REFRESH_REQ_SIZE = 14
private const val EXPECTED_PAIR_REFRESH_ACK_HEADER = 0x54
private const val EXPECTED_PAIR_REFRESH_ACK_SIZE = 10
private const val EXPECTED_UNPAIR_HEADER = 0xAD
private const val EXPECTED_UNPAIR_SIZE = 12
private const val TOO_SHORT_FRAME_SIZE = 3
private const val UNPAIR_HMAC_MSG_SIZE = 10
private const val REFRESH_HMAC_MSG_SIZE = 17

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

class PairingPacketsTest {

    @Test
    fun constants() {
        assertEquals(EXPECTED_PAIR_REQ_HEADER, PAIR_REQ_HEADER)
        assertEquals(EXPECTED_PAIR_OK_HEADER, PAIR_OK_HEADER)
        assertEquals(EXPECTED_PAIR_ERR_HEADER, PAIR_ERR_HEADER)
        assertEquals(EXPECTED_PAIR_REQ_SIZE, PAIR_REQ_SIZE)
        assertEquals(EXPECTED_PAIR_OK_SIZE, PAIR_OK_SIZE)
        assertEquals(EXPECTED_PAIR_OK_SIZE_V11, PAIR_OK_SIZE_V11)
        assertEquals(EXPECTED_PAIR_ERR_SIZE, PAIR_ERR_SIZE)
        assertEquals(EXPECTED_PAIR_REFRESH_REQ_HEADER, PAIR_REFRESH_REQ_HEADER)
        assertEquals(EXPECTED_PAIR_REFRESH_REQ_SIZE, PAIR_REFRESH_REQ_SIZE)
        assertEquals(EXPECTED_PAIR_REFRESH_ACK_HEADER, PAIR_REFRESH_ACK_HEADER)
        assertEquals(EXPECTED_PAIR_REFRESH_ACK_SIZE, PAIR_REFRESH_ACK_SIZE)
        assertEquals("desky.local", ROBOT_HOST)
    }

    @Test
    fun encodePairReqGolden() {
        // code="123456", client_nonce=01020304: XOR over the first 11 bytes is 0x53.
        val pkt = PairingPacketCodec.encodePairReq(
            "123456".encodeToByteArray(),
            hex("01020304"),
        )
        val expected = hex("5031323334353601020304") + byteArrayOf(0x53.toByte())
        assertContentEquals(expected, pkt)
        assertEquals(xorOf(pkt, PAIR_REQ_SIZE - 1), pkt[PAIR_REQ_SIZE - 1].unsigned())
    }

    @Test
    fun encodePairReqRejectsBadSizes() {
        assertFailsWith<IllegalArgumentException> {
            PairingPacketCodec.encodePairReq("12345".encodeToByteArray(), hex("01020304"))
        }
        assertFailsWith<IllegalArgumentException> {
            PairingPacketCodec.encodePairReq("1234567".encodeToByteArray(), hex("01020304"))
        }
        assertFailsWith<IllegalArgumentException> {
            PairingPacketCodec.encodePairReq("123456".encodeToByteArray(), hex("010203"))
        }
    }

    @Test
    fun decodePairOkGolden() {
        // Frozen v1 vectors (README §v1): server=05060708, ct/tag from aesGcmEncrypt.
        val serverNonce = hex("05060708")
        val ct = hex("f47f282e8b8020ec2acf8bf1eaf2ee25")
        val tag = hex("dd497abf431870b75f5722754aa2a105")
        val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
        var xor = 0
        for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
        val pkt = body + byteArrayOf(xor.toByte())

        assertEquals(PAIR_OK_SIZE, pkt.size)
        val decoded = assertNotNull(PairingPacketCodec.decodePairOk(pkt))
        assertContentEquals(serverNonce, decoded.first)
        assertContentEquals(ct, decoded.second)
        assertContentEquals(tag, decoded.third)
    }

    @Test
    fun decodePairOkV11Golden() {
        // v1.1 vectors (README §v1.1 addendum): same nonces, 17B ct covering
        // psk16||persist_u8(0x80).
        val serverNonce = hex("05060708")
        val ct = hex("f47f282e8b8020ec2acf8bf1eaf2ee2590")
        val tag = hex("e211690852a899b6d4062fbf1058876c")
        val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
        var xor = 0
        for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
        val pkt = body + byteArrayOf(xor.toByte())

        assertEquals(PAIR_OK_SIZE_V11, pkt.size)
        val decoded = assertNotNull(PairingPacketCodec.decodePairOk(pkt))
        assertContentEquals(serverNonce, decoded.first)
        assertContentEquals(ct, decoded.second)
        assertContentEquals(tag, decoded.third)
    }

    @Test
    fun decodePairOkRejects() {
        val good = run {
            val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + ByteArray(PAIR_OK_SIZE - 2) { it.toByte() }
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
            body + byteArrayOf(xor.toByte())
        }
        assertNull(PairingPacketCodec.decodePairOk(byteArrayOf()))
        assertNull(PairingPacketCodec.decodePairOk(good.copyOf(PAIR_OK_SIZE - 1)))
        // 39B is the valid v1.1 size now (framing accepts it when the
        // checksum lines up); 40B stays a wrong-length reject.
        assertNull(PairingPacketCodec.decodePairOk(good + byteArrayOf(0x00, 0x00)))
        // Wrong header (e.g. a PAIR_REQ header on a 38-byte frame).
        val badHeader = good.copyOf().also { it[0] = PAIR_REQ_HEADER.toByte() }
        assertNull(PairingPacketCodec.decodePairOk(badHeader))
        // Corrupt checksum byte.
        val badChecksum = good.copyOf().also { it[PAIR_OK_SIZE - 1] = (it[PAIR_OK_SIZE - 1] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairOk(badChecksum))
        // Corrupt payload without fixing the checksum.
        val badPayload = good.copyOf().also { it[10] = (it[10] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairOk(badPayload))
    }

    @Test
    fun decodePairErrGolden() {
        // reason=locked(1), retry_s=30: bytes 52 01 1E, XOR 0x4D.
        val pkt = byteArrayOf(0x52.toByte(), 0x01.toByte(), 0x1E.toByte(), 0x4D.toByte())
        assertEquals(xorOf(pkt, PAIR_ERR_SIZE - 1), pkt[PAIR_ERR_SIZE - 1].unsigned())
        assertEquals(1 to LOCKED_RETRY_SECONDS, PairingPacketCodec.decodePairErr(pkt))
        // Every documented reason round-trips (0-3 frozen, 4 M11 additive).
        for (reason in 0..ROBOT_ERROR_REASON) {
            val body = byteArrayOf(PAIR_ERR_HEADER.toByte(), reason.toByte(), 0x00.toByte())
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
            val framed = body + byteArrayOf(xor.toByte())
            assertEquals(reason to 0, PairingPacketCodec.decodePairErr(framed), "reason=$reason")
        }
    }

    @Test
    fun decodePairErrReason4Golden() {
        // M11 additive reason 4 (internal_retry_fresh): bytes 52 04 00, XOR 0x56.
        // Same bytes the firmware burn-failure branch sends (savePair/deriveKek/
        // wrapPsk fail after tryCode kOk); old 0-3 vectors still decode above.
        val pkt = byteArrayOf(0x52.toByte(), 0x04.toByte(), 0x00.toByte(), 0x56.toByte())
        assertEquals(xorOf(pkt, PAIR_ERR_SIZE - 1), pkt[PAIR_ERR_SIZE - 1].unsigned())
        assertEquals(ROBOT_ERROR_REASON to 0, PairingPacketCodec.decodePairErr(pkt))
    }
    @Test
    fun decodePairErrRejects() {
        val good = byteArrayOf(0x52.toByte(), 0x00.toByte(), 0x00.toByte(), 0x52.toByte())
        assertNull(PairingPacketCodec.decodePairErr(byteArrayOf()))
        assertNull(PairingPacketCodec.decodePairErr(good.copyOf(TOO_SHORT_FRAME_SIZE)))
        assertNull(PairingPacketCodec.decodePairErr(good + byteArrayOf(0x00)))
        val badHeader = good.copyOf().also { it[0] = PAIR_OK_HEADER.toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badHeader))
        val badChecksum = good.copyOf().also { it[PAIR_ERR_SIZE - 1] = (it[PAIR_ERR_SIZE - 1] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badChecksum))
        val badPayload = good.copyOf().also { it[1] = (it[1] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badPayload))
    }
}

class UnpairPacketTest {

    @Test
    fun constants() {
        assertEquals(EXPECTED_UNPAIR_HEADER, UNPAIR_HEADER)
        assertEquals(EXPECTED_UNPAIR_SIZE, UNPAIR_SIZE)
    }

    /**
     * Golden `UNPAIR` frame (psk=`0011...eeff`, counter=1). The expected tag
     * is built from a MANUALLY assembled 10-byte HMAC input
     * (`"UNPAIR" ASCII || counterBE4`) through the [hmacSha256] primitive —
     * never through [encodeUnpair][com.desky.shared.domain.encodeUnpair] itself — and the frame
     * bytes are laid out literally around it.
     */
    @Test
    fun goldenFrameBytesAndTag() = kotlinx.coroutines.test.runTest {
        val psk = hex("00112233445566778899aabbccddeeff")
        val counter = 1u

        val hmacInput = "UNPAIR".encodeToByteArray() + byteArrayOf(0x00, 0x00, 0x00, 0x01)
        assertEquals(UNPAIR_HMAC_MSG_SIZE, hmacInput.size)
        val tag = hmacSha256(psk, hmacInput).copyOfRange(0, AUTH_TAG_SIZE)

        val body = byteArrayOf(0xAD.toByte(), 0x00, 0x00, 0x00, 0x01) + tag
        assertEquals(UNPAIR_SIZE - 1, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeUnpair(psk, counter)

        assertEquals(UNPAIR_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Structural spot checks independent of the tag value.
        assertEquals(0xAD, actual[0].unsigned())
        assertEquals(0, actual[1].unsigned())
        assertEquals(0, actual[2].unsigned())
        assertEquals(0, actual[3].unsigned())
        assertEquals(1, actual[4].unsigned())
        assertContentEquals(tag, actual.copyOfRange(5, 11))
        assertEquals(xorOf(actual, UNPAIR_SIZE - 1), actual[UNPAIR_SIZE - 1].unsigned())
    }

    @Test
    fun counterLaysOutBigEndian() = kotlinx.coroutines.test.runTest {
        val psk = ByteArray(PSK_BYTE_LENGTH) { it.toByte() }
        val pkt = encodeUnpair(psk, 0x01020304u)
        assertEquals(0xAD, pkt[0].unsigned())
        assertEquals(0x01, pkt[1].unsigned())
        assertEquals(0x02, pkt[2].unsigned())
        assertEquals(0x03, pkt[3].unsigned())
        assertEquals(0x04, pkt[4].unsigned())
        assertEquals(xorOf(pkt, UNPAIR_SIZE - 1), pkt[UNPAIR_SIZE - 1].unsigned())
    }

    @Test
    fun rejectsShortPsk() = kotlinx.coroutines.test.runTest {
        var threw = false
        try {
            encodeUnpair(ByteArray(SHORT_PSK_SIZE), 1u)
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue(threw, "encodeUnpair must require a 16-byte PSK")
    }
}

class PairRefreshPacketTest {

    /**
     * Golden `PAIR_REFRESH_REQ` frame (oldPsk=`0011...eeff`, era=1, counter=100).
     * The expected tag is built from a MANUALLY assembled 17-byte HMAC input
     * (`"PAIRREFRESH" ASCII || eraBE2 || counterBE4`) through the [hmacSha256]
     * primitive — never through [encodePairRefreshReq] itself — and the frame
     * bytes are laid out literally around it.
     */
    @Test
    fun goldenReqBytesAndTag() = kotlinx.coroutines.test.runTest {
        val oldPsk = hex("00112233445566778899aabbccddeeff")

        val hmacInput = "PAIRREFRESH".encodeToByteArray() + byteArrayOf(0x00, 0x01, 0x00, 0x00, 0x00, 0x64)
        assertEquals(REFRESH_HMAC_MSG_SIZE, hmacInput.size)
        val tag = hmacSha256(oldPsk, hmacInput).copyOfRange(0, AUTH_TAG_SIZE)
        assertContentEquals(hex("ecac8eb7c93c"), tag)

        val body = byteArrayOf(0x53.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(PAIR_REFRESH_REQ_SIZE - 1, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodePairRefreshReq(oldPsk, 1u, 100u)

        assertEquals(PAIR_REFRESH_REQ_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (README §pair-refresh vectors): header + era + counter + tag + XOR.
        assertContentEquals(hex("53000100000064ecac8eb7c93cba"), actual)
        assertEquals(xorOf(actual, PAIR_REFRESH_REQ_SIZE - 1), actual[PAIR_REFRESH_REQ_SIZE - 1].unsigned())
    }

    @Test
    fun reqEraAndCounterLayOutBigEndian() = kotlinx.coroutines.test.runTest {
        val psk = ByteArray(PSK_BYTE_LENGTH) { it.toByte() }
        val pkt = encodePairRefreshReq(psk, 0x0102u, 0x01020304u)
        assertEquals(0x53, pkt[0].unsigned())
        assertEquals(0x01, pkt[1].unsigned())
        assertEquals(0x02, pkt[2].unsigned())
        assertEquals(0x01, pkt[3].unsigned())
        assertEquals(0x02, pkt[4].unsigned())
        assertEquals(0x03, pkt[5].unsigned())
        assertEquals(0x04, pkt[6].unsigned())
        assertEquals(xorOf(pkt, PAIR_REFRESH_REQ_SIZE - 1), pkt[PAIR_REFRESH_REQ_SIZE - 1].unsigned())
    }

    @Test
    fun reqRejectsShortPsk() = kotlinx.coroutines.test.runTest {
        var threw = false
        try {
            encodePairRefreshReq(ByteArray(SHORT_PSK_SIZE), 1u, 1u)
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue(threw, "encodePairRefreshReq must require a 16-byte PSK")
    }

    @Test
    fun decodeAckGolden() = kotlinx.coroutines.test.runTest {
        // Literal golden (README §pair-refresh vectors): era=1 under the derived
        // era-1 key, tag 62c29ea4d181, XOR 0x9F.
        val pkt = hex("54000162c29ea4d1819f")
        assertEquals(PAIR_REFRESH_ACK_SIZE, pkt.size)
        val decoded = assertNotNull(decodePairRefreshAck(pkt))
        assertEquals(1u, decoded.first)
        assertContentEquals(hex("62c29ea4d181"), decoded.second)
    }

    @Test
    fun decodeAckRoundTripsDerivedTag() = kotlinx.coroutines.test.runTest {
        val newPsk = pairRefreshPsk(hex("00112233445566778899aabbccddeeff"), 1u)
        val tag = pairRefreshAckTag(newPsk, 1u)
        val body = byteArrayOf(0x54.toByte(), 0x00, 0x01) + tag
        val pkt = body + byteArrayOf(xorOf(body, body.size).toByte())
        val decoded = assertNotNull(decodePairRefreshAck(pkt))
        assertEquals(1u, decoded.first)
        assertContentEquals(tag, decoded.second)
    }

    @Test
    fun decodeAckRejects() {
        val good = hex("54000162c29ea4d1819f")
        assertNull(decodePairRefreshAck(byteArrayOf()))
        assertNull(decodePairRefreshAck(good.copyOf(PAIR_REFRESH_ACK_SIZE - 1)))
        assertNull(decodePairRefreshAck(good + byteArrayOf(0x00)))
        // Wrong header (a PAIR_REFRESH_REQ header on a 10-byte frame).
        val badHeader = good.copyOf().also { it[0] = PAIR_REFRESH_REQ_HEADER.toByte() }
        assertNull(decodePairRefreshAck(badHeader))
        // Corrupt checksum byte.
        val badChecksum = good.copyOf().also { it[PAIR_REFRESH_ACK_SIZE - 1] = (it[PAIR_REFRESH_ACK_SIZE - 1] + 1).toByte() }
        assertNull(decodePairRefreshAck(badChecksum))
        // Corrupt payload without fixing the checksum.
        val badPayload = good.copyOf().also { it[4] = (it[4] + 1).toByte() }
        assertNull(decodePairRefreshAck(badPayload))
    }
}
