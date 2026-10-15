package com.desky.shared.domain

import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertFailsWith

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

// Fixed inputs (embedded/scripts/gen_pair_vectors.py; README §v1 vectors).
private val CODE = "123456".encodeToByteArray()
private val CLIENT_NONCE = hex("01020304")
private val SERVER_NONCE = hex("05060708")
private val PSK = hex("00112233445566778899aabbccddeeff")
private val NONCE12 = hex("050607080102030400000000")
private val AAD = hex("510102030405060708")

// Expected outputs (script output; README must match byte-for-byte).
private const val KEK_HEX = "0544036bb09f5c5a7a41759d414e2f46"
private const val CT_HEX = "f47f282e8b8020ec2acf8bf1eaf2ee25"
private const val GCM_TAG_HEX = "dd497abf431870b75f5722754aa2a105"
private const val DRIVE_TAG_HEX = "3ec8b625c9c1"

class PairingCryptoTest {

    @Test
    fun hkdfDerivesKek() = runTest {
        val kek = hkdfSha256(
            salt = CLIENT_NONCE + SERVER_NONCE,
            ikm = CODE,
            info = "desky-pair-v1",
            length = 16,
        )
        assertContentEquals(hex(KEK_HEX), kek)
    }

    @Test
    fun aesGcmWrapsPsk() = runTest {
        val (ct, tag) = aesGcmEncrypt(
            key = hex(KEK_HEX),
            nonce12 = NONCE12,
            aad = AAD,
            plaintext = PSK,
        )
        assertContentEquals(hex(CT_HEX), ct)
        assertContentEquals(hex(GCM_TAG_HEX), tag)
    }

    @Test
    fun driveTagMatches() = runTest {
        val tag = driveTag(
            psk = PSK,
            counter = 1u,
            mode = 0x00,
            thr = 0x80.toByte(),
            str = 0x80.toByte(),
            flags = 0x00,
        )
        assertContentEquals(hex(DRIVE_TAG_HEX), tag)
    }

    @Test
    fun aesGcmDecryptsGoldenPsk() = runTest {
        val psk = aesGcmDecrypt(
            key = hex(KEK_HEX),
            nonce12 = NONCE12,
            aad = AAD,
            ct = hex(CT_HEX),
            tag = hex(GCM_TAG_HEX),
        )
        assertContentEquals(PSK, psk)
    }

    @Test
    fun aesGcmDecryptRejectsTamperedTag() = runTest {
        val badTag = hex(GCM_TAG_HEX).copyOf().also { it[0] = (it[0].toInt() xor 0xFF).toByte() }
        var threw = false
        try {
            aesGcmDecrypt(
                key = hex(KEK_HEX),
                nonce12 = NONCE12,
                aad = AAD,
                ct = hex(CT_HEX),
                tag = badTag,
            )
        } catch (e: Exception) {
            threw = true
        }
        kotlin.test.assertTrue(threw, "GCM tag mismatch must throw")
    }

    @Test
    fun pairingNonceIsFourBytes() = runTest {
        kotlin.test.assertEquals(4, pairingNonce().size)
        kotlin.test.assertEquals(4, pairingNonce().size)
    }

    @Test
    fun rejectsBadSizes() = runTest {
        assertFailsWith<IllegalArgumentException> {
            hkdfSha256(salt = ByteArray(4), ikm = CODE, info = "desky-pair-v1", length = 16)
        }
        assertFailsWith<IllegalArgumentException> {
            aesGcmEncrypt(key = ByteArray(8), nonce12 = NONCE12, aad = AAD, plaintext = PSK)
        }
        assertFailsWith<IllegalArgumentException> {
            aesGcmDecrypt(
                key = ByteArray(8),
                nonce12 = NONCE12,
                aad = AAD,
                ct = hex(CT_HEX),
                tag = hex(GCM_TAG_HEX),
            )
        }
        assertFailsWith<IllegalArgumentException> {
            aesGcmDecrypt(
                key = hex(KEK_HEX),
                nonce12 = NONCE12,
                aad = AAD,
                ct = hex(CT_HEX),
                tag = ByteArray(8),
            )
        }
        assertFailsWith<IllegalArgumentException> {
            driveTag(psk = ByteArray(8), counter = 1u, mode = 0, thr = 0, str = 0, flags = 0)
        }
        assertFailsWith<IllegalArgumentException> {
            unbondTag(psk = ByteArray(8), counter = 1u)
        }
    }
}
