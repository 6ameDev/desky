package com.desky.shared.domain

import dev.whyoleg.cryptography.BinarySize.Companion.bits
import dev.whyoleg.cryptography.CryptographyProvider
import dev.whyoleg.cryptography.DelicateCryptographyApi
import dev.whyoleg.cryptography.algorithms.AES
import dev.whyoleg.cryptography.algorithms.HKDF
import dev.whyoleg.cryptography.algorithms.HMAC
import dev.whyoleg.cryptography.algorithms.SHA256
import dev.whyoleg.cryptography.random.CryptographyRandom

/**
 * Pairing/auth v1 crypto (frozen spec: `shared/protocol/README.md`
 * §"Pairing + authenticated drive v1"). Thin suspend wrappers over
 * `dev.whyoleg.cryptography:0.6.0` (JCA/CryptoKit backends) — no
 * hand-rolled crypto ([Sha256.kt] stays for the legacy token only).
 */

/**
 * Full 32-byte `HMAC-SHA256(key, data)`.
 *
 * @param key HMAC key bytes (16-byte PSK or any length the backend accepts).
 * @param data message bytes.
 */
suspend fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
    require(key.isNotEmpty()) { "hmacSha256: key must not be empty" }
    val hmac = CryptographyProvider.Default.get(HMAC)
    val cryptoKey = hmac.keyDecoder(SHA256).decodeFromByteArray(HMAC.Key.Format.RAW, key)
    return cryptoKey.signatureGenerator().generateSignature(data)
}

/**
 * `KEK = HKDF-SHA256(salt, IKM, info, L)` — pairing key-encryption key,
 * where `salt = client_nonce(4) || server_nonce(4)`, `IKM` = 6 ASCII code
 * digits, `info = "desky-pair-v1"`, `L = 16`.
 *
 * @param salt salt bytes (8 = both pairing nonces concatenated).
 * @param ikm input keying material (6 ASCII code digits).
 * @param info context string (spec: `"desky-pair-v1"`).
 * @param length output length in bytes (spec: 16).
 */
suspend fun hkdfSha256(salt: ByteArray, ikm: ByteArray, info: String, length: Int): ByteArray {
    require(salt.size == 8) { "hkdfSha256: salt must be 8 bytes (client_nonce||server_nonce)" }
    require(ikm.size == 6) { "hkdfSha256: IKM must be 6 ASCII code digits" }
    require(length in 1..255 * 32) { "hkdfSha256: invalid output length" }
    val hkdf = CryptographyProvider.Default.get(HKDF)
    return hkdf.secretDerivation(
        digest = SHA256,
        outputSize = (length * 8).bits,
        salt = salt,
        info = info.encodeToByteArray(),
    ).deriveSecretToByteArray(ikm)
}

/**
 * `PAIR_OK` wrap: `ct||tag = AES-128-GCM(KEK, nonce12, AAD, psk16)`,
 * where `nonce12 = server_nonce(4) || client_nonce(4) || 00 00 00 00`
 * and `AAD = 0x51 || client_nonce(4) || server_nonce(4)`.
 *
 * The backend returns `ct||tag` concatenated; the 16-byte tag is split
 * out explicitly for the audit-visible wire layout.
 *
 * @param key 16-byte KEK from [hkdfSha256].
 * @param nonce12 12-byte GCM nonce per above.
 * @param aad 9-byte additional data (`0x51 || nonces`).
 * @param plaintext 16-byte PSK.
 * @return `ct` (16B) to `tag` (16B).
 */
// Custom IV is spec-mandated here (server_nonce||client_nonce||zeros);
// never reuse a (key, nonce) pair outside the single PAIR_OK wrap.
@OptIn(DelicateCryptographyApi::class)
suspend fun aesGcmEncrypt(
    key: ByteArray,
    nonce12: ByteArray,
    aad: ByteArray,
    plaintext: ByteArray,
): Pair<ByteArray, ByteArray> {
    require(key.size == 16) { "aesGcmEncrypt: key must be 16 bytes (AES-128)" }
    require(nonce12.size == 12) { "aesGcmEncrypt: nonce must be 12 bytes" }
    require(aad.size == 9) { "aesGcmEncrypt: AAD must be 9 bytes (0x51||nonces)" }
    require(plaintext.size == 16) { "aesGcmEncrypt: plaintext must be 16 bytes (PSK)" }
    val aesGcm = CryptographyProvider.Default.get(AES.GCM)
    val cryptoKey = aesGcm.keyDecoder().decodeFromByteArray(AES.Key.Format.RAW, key)
    val ctAndTag = cryptoKey.cipher().encryptWithIv(
        iv = nonce12,
        plaintext = plaintext,
        associatedData = aad,
    )
    require(ctAndTag.size == 32) { "aesGcmEncrypt: expected 16B ct + 16B tag" }
    return ctAndTag.copyOfRange(0, 16) to ctAndTag.copyOfRange(16, 32)
}

/**
 * `PAIR_OK` unwrap: `psk16 = AES-128-GCM-decrypt(KEK, nonce12, AAD, ct||tag)`,
 * the inverse of [aesGcmEncrypt] with the same spec-mandated `nonce12` and
 * `AAD` layout.
 *
 * GCM authentication failure throws (the backend's tag-mismatch exception) —
 * callers map it to a pairing-failure outcome, never to a retry.
 *
 * @param key 16-byte KEK from [hkdfSha256].
 * @param nonce12 12-byte GCM nonce (`server_nonce(4) || client_nonce(4) || zeros`).
 * @param aad 9-byte additional data (`0x51 || client_nonce(4) || server_nonce(4)`).
 * @param ct 16-byte wrapped-PSK ciphertext.
 * @param tag 16-byte GCM authentication tag.
 * @return unwrapped 16-byte PSK.
 */
// Custom IV is spec-mandated here (server_nonce||client_nonce||zeros);
// never reuse a (key, nonce) pair outside the single PAIR_OK wrap.
@OptIn(DelicateCryptographyApi::class)
suspend fun aesGcmDecrypt(
    key: ByteArray,
    nonce12: ByteArray,
    aad: ByteArray,
    ct: ByteArray,
    tag: ByteArray,
): ByteArray {
    require(key.size == 16) { "aesGcmDecrypt: key must be 16 bytes (AES-128)" }
    require(nonce12.size == 12) { "aesGcmDecrypt: nonce must be 12 bytes" }
    require(aad.size == 9) { "aesGcmDecrypt: AAD must be 9 bytes (0x51||nonces)" }
    require(ct.size == 16) { "aesGcmDecrypt: ct must be 16 bytes" }
    require(tag.size == 16) { "aesGcmDecrypt: tag must be 16 bytes" }
    val aesGcm = CryptographyProvider.Default.get(AES.GCM)
    val cryptoKey = aesGcm.keyDecoder().decodeFromByteArray(AES.Key.Format.RAW, key)
    return cryptoKey.cipher().decryptWithIv(
        iv = nonce12,
        ciphertext = ct + tag,
        associatedData = aad,
    )
}

/**
 * Fresh 4-byte `client_nonce` from the platform CSPRNG
 * (`cryptography-random`: JVM `SecureRandom`, Apple `CCRandomGenerateBytes`,
 * Linux `getrandom` — exposed via the `kotlin.random.Random` API, never
 * hand-rolled).
 */
suspend fun pairingNonce(): ByteArray =
    CryptographyRandom.Default.nextBytes(PAIR_NONCE_SIZE)

/** Lowercase hex rendering (e.g. PSK persistence). */
fun ByteArray.toHex(): String {
    val chars = CharArray(size * 2)
    val digits = "0123456789abcdef"
    for (i in indices) {
        val v = this[i].toInt() and 0xFF
        chars[i * 2] = digits[v ushr 4]
        chars[i * 2 + 1] = digits[v and 0x0F]
    }
    return chars.concatToString()
}

/**
 * Inverse of [ByteArray.toHex]: even-length ASCII hex (upper or lower case)
 * -> bytes. Throws [IllegalArgumentException] on odd length or non-hex
 * digits. Used to rehydrate the vault-stored PSK hex into key bytes at
 * session start (Task 9: the vault is read once, the bytes cached in
 * memory).
 */
fun hexToBytes(hex: String): ByteArray {
    require(hex.length % 2 == 0) { "hexToBytes: odd length (${hex.length})" }
    return ByteArray(hex.length / 2) { i ->
        val hi = hex[i * 2].digitToIntOrNull(16)
        val lo = hex[i * 2 + 1].digitToIntOrNull(16)
        require(hi != null && lo != null) { "hexToBytes: non-hex digits at byte $i" }
        ((hi shl 4) or lo).toByte()
    }
}
/**
 * `DRIVE_V2` auth tag: `first6(HMAC-SHA256(psk16,
 * counterBE4 || mode || thr || str || flags))`.
 *
 * @param psk 16-byte bond PSK.
 * @param counter 32-bit BE drive counter (1 for the first wire packet; 0 never valid).
 * @param mode mode byte.
 * @param thr center-128 throttle byte.
 * @param str center-128 steering byte.
 * @param flags flags byte.
 * @return 6-byte tag.
 */
suspend fun driveTag(
    psk: ByteArray,
    counter: UInt,
    mode: Byte,
    thr: Byte,
    str: Byte,
    flags: Byte,
): ByteArray {
    require(psk.size == 16) { "driveTag: psk must be 16 bytes" }
    val msg = ByteArray(8)
    msg[0] = (counter shr 24).toByte()
    msg[1] = (counter shr 16).toByte()
    msg[2] = (counter shr 8).toByte()
    msg[3] = counter.toByte()
    msg[4] = mode
    msg[5] = thr
    msg[6] = str
    msg[7] = flags
    return hmacSha256(psk, msg).copyOfRange(0, 6)
}
