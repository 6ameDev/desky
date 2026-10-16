package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertNull
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

class PairingPacketsTest {

    @Test
    fun constants() {
        assertEquals(0x50, PAIR_REQ_HEADER)
        assertEquals(0x51, PAIR_OK_HEADER)
        assertEquals(0x52, PAIR_ERR_HEADER)
        assertEquals(12, PAIR_REQ_SIZE)
        assertEquals(38, PAIR_OK_SIZE)
        assertEquals(39, PAIR_OK_SIZE_V11)
        assertEquals(4, PAIR_ERR_SIZE)
        assertEquals(0x53, REKEY_REQ_HEADER)
        assertEquals(14, REKEY_REQ_SIZE)
        assertEquals(0x54, REKEY_ACK_HEADER)
        assertEquals(10, REKEY_ACK_SIZE)
        assertEquals("192.168.4.1", ROBOT_HOST)
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
        for (b in body) xor = xor xor (b.toInt() and 0xFF)
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
        for (b in body) xor = xor xor (b.toInt() and 0xFF)
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
            val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + ByteArray(36) { it.toByte() }
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and 0xFF)
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
        assertEquals(1 to 30, PairingPacketCodec.decodePairErr(pkt))
        // Every documented reason round-trips (0-3 frozen, 4 M11 additive).
        for (reason in 0..4) {
            val body = byteArrayOf(PAIR_ERR_HEADER.toByte(), reason.toByte(), 0x00.toByte())
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and 0xFF)
            val framed = body + byteArrayOf(xor.toByte())
            assertEquals(reason to 0, PairingPacketCodec.decodePairErr(framed), "reason=$reason")
        }
    }

    @Test
    fun decodePairErrReason4Golden() {
        // M11 additive reason 4 (internal_retry_fresh): bytes 52 04 00, XOR 0x56.
        // Same bytes the firmware burn-failure branch sends (saveBond/deriveKek/
        // wrapPsk fail after tryCode kOk); old 0-3 vectors still decode above.
        val pkt = byteArrayOf(0x52.toByte(), 0x04.toByte(), 0x00.toByte(), 0x56.toByte())
        assertEquals(xorOf(pkt, PAIR_ERR_SIZE - 1), pkt[PAIR_ERR_SIZE - 1].unsigned())
        assertEquals(4 to 0, PairingPacketCodec.decodePairErr(pkt))
    }
    @Test
    fun decodePairErrRejects() {
        val good = byteArrayOf(0x52.toByte(), 0x00.toByte(), 0x00.toByte(), 0x52.toByte())
        assertNull(PairingPacketCodec.decodePairErr(byteArrayOf()))
        assertNull(PairingPacketCodec.decodePairErr(good.copyOf(3)))
        assertNull(PairingPacketCodec.decodePairErr(good + byteArrayOf(0x00)))
        val badHeader = good.copyOf().also { it[0] = PAIR_OK_HEADER.toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badHeader))
        val badChecksum = good.copyOf().also { it[3] = (it[3] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badChecksum))
        val badPayload = good.copyOf().also { it[1] = (it[1] + 1).toByte() }
        assertNull(PairingPacketCodec.decodePairErr(badPayload))
    }
}

class UnbondPacketTest {

    @Test
    fun constants() {
        assertEquals(0xAD, UNBOND_HEADER)
        assertEquals(12, UNBOND_SIZE)
    }

    /**
     * Golden `UNBOND` frame (psk=`0011...eeff`, counter=1). The expected tag
     * is built from a MANUALLY assembled 10-byte HMAC input
     * (`"UNBOND" ASCII || counterBE4`) through the [hmacSha256] primitive —
     * never through [encodeUnbond][com.desky.shared.domain.encodeUnbond] itself — and the frame
     * bytes are laid out literally around it.
     */
    @Test
    fun goldenFrameBytesAndTag() = kotlinx.coroutines.test.runTest {
        val psk = hex("00112233445566778899aabbccddeeff")
        val counter = 1u

        val hmacInput = "UNBOND".encodeToByteArray() + byteArrayOf(0x00, 0x00, 0x00, 0x01)
        assertEquals(10, hmacInput.size)
        val tag = hmacSha256(psk, hmacInput).copyOfRange(0, 6)

        val body = byteArrayOf(0xAD.toByte(), 0x00, 0x00, 0x00, 0x01) + tag
        assertEquals(11, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeUnbond(psk, counter)

        assertEquals(UNBOND_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Structural spot checks independent of the tag value.
        assertEquals(0xAD, actual[0].unsigned())
        assertEquals(0, actual[1].unsigned())
        assertEquals(0, actual[2].unsigned())
        assertEquals(0, actual[3].unsigned())
        assertEquals(1, actual[4].unsigned())
        assertContentEquals(tag, actual.copyOfRange(5, 11))
        assertEquals(xorOf(actual, UNBOND_SIZE - 1), actual[UNBOND_SIZE - 1].unsigned())
    }

    @Test
    fun counterLaysOutBigEndian() = kotlinx.coroutines.test.runTest {
        val psk = ByteArray(16) { it.toByte() }
        val pkt = encodeUnbond(psk, 0x01020304u)
        assertEquals(0xAD, pkt[0].unsigned())
        assertEquals(0x01, pkt[1].unsigned())
        assertEquals(0x02, pkt[2].unsigned())
        assertEquals(0x03, pkt[3].unsigned())
        assertEquals(0x04, pkt[4].unsigned())
        assertEquals(xorOf(pkt, UNBOND_SIZE - 1), pkt[UNBOND_SIZE - 1].unsigned())
    }

    @Test
    fun rejectsShortPsk() = kotlinx.coroutines.test.runTest {
        var threw = false
        try {
            encodeUnbond(ByteArray(8), 1u)
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue(threw, "encodeUnbond must require a 16-byte PSK")
    }
}

class RekeyPacketTest {

    /**
     * Golden `REKEY_REQ` frame (oldPsk=`0011...eeff`, era=1, counter=100).
     * The expected tag is built from a MANUALLY assembled 11-byte HMAC input
     * (`"REKEY" ASCII || eraBE2 || counterBE4`) through the [hmacSha256]
     * primitive — never through [encodeRekeyReq] itself — and the frame
     * bytes are laid out literally around it.
     */
    @Test
    fun goldenReqBytesAndTag() = kotlinx.coroutines.test.runTest {
        val oldPsk = hex("00112233445566778899aabbccddeeff")

        val hmacInput = "REKEY".encodeToByteArray() + byteArrayOf(0x00, 0x01, 0x00, 0x00, 0x00, 0x64)
        assertEquals(11, hmacInput.size)
        val tag = hmacSha256(oldPsk, hmacInput).copyOfRange(0, 6)
        assertContentEquals(hex("fdfe277d1623"), tag)

        val body = byteArrayOf(0x53.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(13, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeRekeyReq(oldPsk, 1u, 100u)

        assertEquals(REKEY_REQ_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (README §rekey vectors): header + era + counter + tag + XOR.
        assertContentEquals(hex("53000100000064fdfe277d16235a"), actual)
        assertEquals(xorOf(actual, REKEY_REQ_SIZE - 1), actual[REKEY_REQ_SIZE - 1].unsigned())
    }

    @Test
    fun reqEraAndCounterLayOutBigEndian() = kotlinx.coroutines.test.runTest {
        val psk = ByteArray(16) { it.toByte() }
        val pkt = encodeRekeyReq(psk, 0x0102u, 0x01020304u)
        assertEquals(0x53, pkt[0].unsigned())
        assertEquals(0x01, pkt[1].unsigned())
        assertEquals(0x02, pkt[2].unsigned())
        assertEquals(0x01, pkt[3].unsigned())
        assertEquals(0x02, pkt[4].unsigned())
        assertEquals(0x03, pkt[5].unsigned())
        assertEquals(0x04, pkt[6].unsigned())
        assertEquals(xorOf(pkt, REKEY_REQ_SIZE - 1), pkt[REKEY_REQ_SIZE - 1].unsigned())
    }

    @Test
    fun reqRejectsShortPsk() = kotlinx.coroutines.test.runTest {
        var threw = false
        try {
            encodeRekeyReq(ByteArray(8), 1u, 1u)
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue(threw, "encodeRekeyReq must require a 16-byte PSK")
    }

    @Test
    fun decodeAckGolden() = kotlinx.coroutines.test.runTest {
        // Literal golden (README §rekey vectors): era=1 under the derived
        // era-1 key, tag ed5f52335855, XOR 0x8b.
        val pkt = hex("540001ed5f523358558b")
        assertEquals(REKEY_ACK_SIZE, pkt.size)
        val decoded = assertNotNull(decodeRekeyAck(pkt))
        assertEquals(1u, decoded.first)
        assertContentEquals(hex("ed5f52335855"), decoded.second)
    }

    @Test
    fun decodeAckRoundTripsDerivedTag() = kotlinx.coroutines.test.runTest {
        val newPsk = rekeyPsk(hex("00112233445566778899aabbccddeeff"), 1u)
        val tag = rekeyAckTag(newPsk, 1u)
        val body = byteArrayOf(0x54.toByte(), 0x00, 0x01) + tag
        val pkt = body + byteArrayOf(xorOf(body, body.size).toByte())
        val decoded = assertNotNull(decodeRekeyAck(pkt))
        assertEquals(1u, decoded.first)
        assertContentEquals(tag, decoded.second)
    }

    @Test
    fun decodeAckRejects() {
        val good = hex("540001ed5f523358558b")
        assertNull(decodeRekeyAck(byteArrayOf()))
        assertNull(decodeRekeyAck(good.copyOf(REKEY_ACK_SIZE - 1)))
        assertNull(decodeRekeyAck(good + byteArrayOf(0x00)))
        // Wrong header (a REKEY_REQ header on a 10-byte frame).
        val badHeader = good.copyOf().also { it[0] = REKEY_REQ_HEADER.toByte() }
        assertNull(decodeRekeyAck(badHeader))
        // Corrupt checksum byte.
        val badChecksum = good.copyOf().also { it[REKEY_ACK_SIZE - 1] = (it[REKEY_ACK_SIZE - 1] + 1).toByte() }
        assertNull(decodeRekeyAck(badChecksum))
        // Corrupt payload without fixing the checksum.
        val badPayload = good.copyOf().also { it[4] = (it[4] + 1).toByte() }
        assertNull(decodeRekeyAck(badPayload))
    }
}
