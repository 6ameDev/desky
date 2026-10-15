package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertNull

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
        assertEquals(4, PAIR_ERR_SIZE)
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
        // Frozen vectors (README §v1): server=05060708, ct/tag from aesGcmEncrypt.
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
    fun decodePairOkRejects() {
        val good = run {
            val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + ByteArray(36) { it.toByte() }
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and 0xFF)
            body + byteArrayOf(xor.toByte())
        }
        assertNull(PairingPacketCodec.decodePairOk(byteArrayOf()))
        assertNull(PairingPacketCodec.decodePairOk(good.copyOf(PAIR_OK_SIZE - 1)))
        assertNull(PairingPacketCodec.decodePairOk(good + byteArrayOf(0x00)))
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
        // Every documented reason round-trips.
        for (reason in 0..3) {
            val body = byteArrayOf(PAIR_ERR_HEADER.toByte(), reason.toByte(), 0x00.toByte())
            var xor = 0
            for (b in body) xor = xor xor (b.toInt() and 0xFF)
            val framed = body + byteArrayOf(xor.toByte())
            assertEquals(reason to 0, PairingPacketCodec.decodePairErr(framed), "reason=$reason")
        }
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
