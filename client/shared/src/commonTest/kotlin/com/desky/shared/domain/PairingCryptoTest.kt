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

// v1.1 outputs (same fixed inputs + persist_every=128/0x80 appended to the
// GCM plaintext: 17B plain -> 17B ct; KEK/drive_tag unchanged).
private const val CT17_HEX = "f47f282e8b8020ec2acf8bf1eaf2ee2590"
private const val GCM_TAG_V11_HEX = "e211690852a899b6d4062fbf1058876c"

// Tier-1 rekey goldens (python hashlib reference: HKDF-SHA256 extract+expand
// per RFC 5869, HMAC-SHA256 tags; must match firmware deriveRekeyPsk +
// rekeyTagMsg/rekeyAckTagMsg layouts byte-for-byte).
// Fixed inputs: oldPsk=PSK, nextEra=1, counter=100.
private const val REKEY_NEXT_PSK_HEX = "df7441d0d71778b2dd10493962d45a5b"
private const val REKEY_TAG_HEX = "fdfe277d1623"
private const val REKEY_ACK_TAG_HEX = "ed5f52335855"

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

    /**
     * v1.1 golden: decrypting the fixed 17B ct/tag yields `psk16||0x80`.
     * Decrypt-only on the fixed nonce — JCA forbids encrypting twice under
     * one GCM IV, so the fixed-vector [aesGcmWrapsPsk] stays the single
     * fixed-nonce encrypt test (the 17B encrypt path round-trips below with
     * a fresh nonce instead).
     */
    @Test
    fun aesGcmDecryptsIntervalPlaintext() = runTest {
        val plain = aesGcmDecrypt(
            key = hex(KEK_HEX),
            nonce12 = NONCE12,
            aad = AAD,
            ct = hex(CT17_HEX),
            tag = hex(GCM_TAG_V11_HEX),
        )
        assertContentEquals(PSK + byteArrayOf(0x80.toByte()), plain)
        val (psk, persistEvery) = unwrapPairPlaintext(plain)
        assertContentEquals(PSK, psk)
        kotlin.test.assertEquals(128, persistEvery)
    }

    @Test
    fun aesGcmRoundTrips17BytePlaintext() = runTest {
        // 17B encrypt path with a fresh nonce (never the fixed vector one).
        val nonce12 = pairingNonce() + pairingNonce() + pairingNonce()
        val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + pairingNonce() + pairingNonce()
        val plain = PSK + byteArrayOf(0x10)
        val (ct, tag) = aesGcmEncrypt(key = hex(KEK_HEX), nonce12 = nonce12, aad = aad, plaintext = plain)
        kotlin.test.assertEquals(17, ct.size)
        val back = aesGcmDecrypt(key = hex(KEK_HEX), nonce12 = nonce12, aad = aad, ct = ct, tag = tag)
        assertContentEquals(plain, back)
        val (psk, persistEvery) = unwrapPairPlaintext(back)
        assertContentEquals(PSK, psk)
        kotlin.test.assertEquals(16, persistEvery)
    }

    @Test
    fun unwrapLegacyPlaintextDefaultsTo128() {
        val (psk, persistEvery) = unwrapPairPlaintext(PSK)
        assertContentEquals(PSK, psk)
        kotlin.test.assertEquals(DriveCounter.COUNTER_PERSIST_EVERY, persistEvery)
    }

    @Test
    fun unwrapSanitizesIntervalByte() {
        // Custom cadence passes through.
        val (_, sixteen) = unwrapPairPlaintext(PSK + byteArrayOf(0x10))
        kotlin.test.assertEquals(16, sixteen)
        // 0 (div-zero) and sub-minimum values fall back to 128.
        val (_, zero) = unwrapPairPlaintext(PSK + byteArrayOf(0x00))
        kotlin.test.assertEquals(128, zero)
        val (_, seven) = unwrapPairPlaintext(PSK + byteArrayOf(0x07))
        kotlin.test.assertEquals(128, seven)
        // Wire max (255, inside the 8..1024 app guard) passes.
        val (_, max) = unwrapPairPlaintext(PSK + byteArrayOf(0xFF.toByte()))
        kotlin.test.assertEquals(255, max)
    }

    @Test
    fun sanitizePersistEveryBounds() {
        kotlin.test.assertEquals(128, sanitizePersistEvery(0))
        kotlin.test.assertEquals(128, sanitizePersistEvery(7))
        kotlin.test.assertEquals(8, sanitizePersistEvery(8))
        kotlin.test.assertEquals(16, sanitizePersistEvery(16))
        kotlin.test.assertEquals(128, sanitizePersistEvery(128))
        kotlin.test.assertEquals(1024, sanitizePersistEvery(1024))
        kotlin.test.assertEquals(128, sanitizePersistEvery(1025))
        kotlin.test.assertEquals(128, sanitizePersistEvery(-1))
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

    /**
     * Tier-1 rekey key schedule: `PSK_next = HKDF-SHA256(
     * salt="desky-rekey-v1", IKM=oldPsk, info=eraBE2(1), L=16)`.
     * Golden pinned against the python RFC-5869 reference AND the firmware
     * `deriveRekeyPsk` contract (same salt/info/length).
     */
    @Test
    fun rekeyDerivesNextPsk() = runTest {
        val next = rekeyPsk(oldPsk = PSK, nextEra = 1u)
        assertContentEquals(hex(REKEY_NEXT_PSK_HEX), next)
    }

    @Test
    fun rekeyChainAdvancesEra() = runTest {
        // Era 2 chains under the era-1 key (never reuses the old PSK).
        val era1 = rekeyPsk(oldPsk = PSK, nextEra = 1u)
        val era2 = rekeyPsk(oldPsk = era1, nextEra = 2u)
        kotlin.test.assertEquals(16, era2.size)
        kotlin.test.assertFalse(era2.contentEquals(PSK), "rotation must change the key")
        kotlin.test.assertFalse(era2.contentEquals(era1), "each era must differ")
    }

    @Test
    fun rekeyTagMatches() = runTest {
        // msg = "REKEY" || eraBE2(0001) || counterBE4(00000064).
        val tag = rekeyTag(oldPsk = PSK, nextEra = 1u, counter = 100u)
        assertContentEquals(hex(REKEY_TAG_HEX), tag)
    }

    @Test
    fun rekeyAckTagMatches() = runTest {
        // msg = "REKEYACK" || eraBE2(0001), under the NEW key (proves switch).
        val tag = rekeyAckTag(newPsk = hex(REKEY_NEXT_PSK_HEX), nextEra = 1u)
        assertContentEquals(hex(REKEY_ACK_TAG_HEX), tag)
    }

    @Test
    fun rekeyTagsDifferByEraAndCounter() = runTest {
        val base = rekeyTag(oldPsk = PSK, nextEra = 1u, counter = 100u)
        kotlin.test.assertFalse(
            rekeyTag(oldPsk = PSK, nextEra = 2u, counter = 100u).contentEquals(base),
            "era is domain-separated",
        )
        kotlin.test.assertFalse(
            rekeyTag(oldPsk = PSK, nextEra = 1u, counter = 101u).contentEquals(base),
            "counter is domain-separated",
        )
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
            aesGcmEncrypt(key = hex(KEK_HEX), nonce12 = NONCE12, aad = AAD, plaintext = ByteArray(15))
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
            aesGcmDecrypt(
                key = hex(KEK_HEX),
                nonce12 = NONCE12,
                aad = AAD,
                ct = ByteArray(15),
                tag = hex(GCM_TAG_HEX),
            )
        }
        assertFailsWith<IllegalArgumentException> {
            unwrapPairPlaintext(ByteArray(15))
        }
        assertFailsWith<IllegalArgumentException> {
            rekeyPsk(oldPsk = ByteArray(8), nextEra = 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            rekeyTag(oldPsk = ByteArray(8), nextEra = 1u, counter = 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            rekeyAckTag(newPsk = ByteArray(8), nextEra = 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            driveTag(psk = ByteArray(8), counter = 1u, mode = 0, thr = 0, str = 0, flags = 0)
        }
        assertFailsWith<IllegalArgumentException> {
            unbondTag(psk = ByteArray(8), counter = 1u)
        }
    }
}
