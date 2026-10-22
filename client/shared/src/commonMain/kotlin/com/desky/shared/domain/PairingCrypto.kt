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
 * `PAIR_OK` wrap: `ct||tag = AES-128-GCM(KEK, nonce12, AAD, plain)`,
 * where `nonce12 = server_nonce(4) || client_nonce(4) || 00 00 00 00`,
 * `AAD = 0x51 || client_nonce(4) || server_nonce(4)`, and `plain` is
 * `psk16` (v1, 38B frame) or `psk16 || persist_u8` (v1.1, 39B frame).
 *
 * The backend returns `ct||tag` concatenated; the 16-byte tag is split
 * out explicitly for the audit-visible wire layout.
 *
 * @param key 16-byte KEK from [hkdfSha256].
 * @param nonce12 12-byte GCM nonce per above.
 * @param aad 9-byte additional data (`0x51 || nonces`).
 * @param plaintext 16-byte PSK, or 17-byte `PSK || persist_u8` (v1.1).
 * @return `ct` (16/17B, matching [plaintext]) to `tag` (16B).
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
    require(plaintext.size == 16 || plaintext.size == 17) {
        "aesGcmEncrypt: plaintext must be 16 bytes (PSK) or 17 (PSK||persist_u8)"
    }
    val aesGcm = CryptographyProvider.Default.get(AES.GCM)
    val cryptoKey = aesGcm.keyDecoder().decodeFromByteArray(AES.Key.Format.RAW, key)
    val ctAndTag = cryptoKey.cipher().encryptWithIv(
        iv = nonce12,
        plaintext = plaintext,
        associatedData = aad,
    )
    require(ctAndTag.size == plaintext.size + 16) { "aesGcmEncrypt: expected ct + 16B tag" }
    return ctAndTag.copyOfRange(0, plaintext.size) to ctAndTag.copyOfRange(plaintext.size, plaintext.size + 16)
}

/**
 * `PAIR_OK` unwrap: `plain = AES-128-GCM-decrypt(KEK, nonce12, AAD, ct||tag)`,
 * the inverse of [aesGcmEncrypt] with the same spec-mandated `nonce12` and
 * `AAD` layout. `plain` is 16 bytes (v1 frame: PSK only) or 17 (v1.1:
 * `PSK || persist_u8`) — split it with [unwrapPairPlaintext].
 *
 * GCM authentication failure throws (the backend's tag-mismatch exception) —
 * callers map it to a pairing-failure outcome, never to a retry.
 *
 * @param key 16-byte KEK from [hkdfSha256].
 * @param nonce12 12-byte GCM nonce (`server_nonce(4) || client_nonce(4) || zeros`).
 * @param aad 9-byte additional data (`0x51 || client_nonce(4) || server_nonce(4)`).
 * @param ct 16-byte wrapped PSK (v1) or 17-byte wrapped `PSK || persist_u8` (v1.1).
 * @param tag 16-byte GCM authentication tag.
 * @return unwrapped plaintext (16 or 17 bytes, matching [ct]).
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
    require(ct.size == 16 || ct.size == 17) { "aesGcmDecrypt: ct must be 16 or 17 bytes" }
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
 * Minimal guard for the negotiated counter-persist cadence (v1.1): N in
 * 8..1024 passes, anything else (0..7 incl. div-zero 0, absurd values)
 * falls back to [DriveCounter.COUNTER_PERSIST_EVERY]. Not an allowlist —
 * just enough to kill 0/div-zero + absurd values. Accepted once at pair
 * only (see [PairDeviceWithCodeUseCase]); mirrors firmware
 * `pairing::sanitizePersistEvery` (whose u8 wire holds the 8..255 subset —
 * 256..1024 are reserved for a future u16 field and map to 128 on wire).
 */
fun sanitizePersistEvery(n: Int): Int =
    if (n in 8..1024) n else DriveCounter.COUNTER_PERSIST_EVERY

/**
 * Split unwrapped `PAIR_OK` plaintext into `(psk16, persistEvery)`: 17
 * bytes (v1.1) carry the trailing interval byte (sanitized via
 * [sanitizePersistEvery]); 16 bytes (v1 frame from an old robot) default
 * to [DriveCounter.COUNTER_PERSIST_EVERY].
 */
fun unwrapPairPlaintext(plain: ByteArray): Pair<ByteArray, Int> {
    require(plain.size == 16 || plain.size == 17) {
        "unwrapPairPlaintext: plain must be 16 or 17 bytes (got ${plain.size})"
    }
    return if (plain.size == 17) {
        plain.copyOfRange(0, 16) to sanitizePersistEvery(plain[16].toInt() and 0xFF)
    } else {
        plain to DriveCounter.COUNTER_PERSIST_EVERY
    }
}

/**
 * Tier-1 silent auto-pair-refresh key schedule (see `shared/protocol/README.md`
 * §"Tier-1 silent auto-pair-refresh"): `PSK_next = HKDF-SHA256(
 * salt = "desky-pair-refresh-v1", IKM = oldPsk16, info = eraBE2(new), L = 16)`.
 *
 * No key transport — both sides derive deterministically under the old PSK;
 * only the era needs sync + confirm (`PAIR_REFRESH_REQ`/`PAIR_REFRESH_ACK`). Mirrors TLS
 * `KeyUpdate` / IPsec pair-refresh: the epoch (`era`, u16 wrap mod 65536) is the
 * key id, the counter resets to the pair baseline (0 persisted, first drive
 * uses 1) so the session resumes gapless before [DriveCounter.EXHAUSTION_GATE].
 *
 * @param oldPsk 16-byte current pair PSK.
 * @param nextEra requested new era (current era + 1, u16 wrap).
 * @return 16-byte next PSK.
 */
suspend fun pairRefreshPsk(oldPsk: ByteArray, nextEra: UShort): ByteArray {
    require(oldPsk.size == 16) { "pairRefreshPsk: oldPsk must be 16 bytes" }
    val hkdf = CryptographyProvider.Default.get(HKDF)
    val eraBE = byteArrayOf(
        ((nextEra.toInt() shr 8) and 0xFF).toByte(),
        (nextEra.toInt() and 0xFF).toByte(),
    )
    return hkdf.secretDerivation(
        digest = SHA256,
        outputSize = (16 * 8).bits,
        salt = PAIR_REFRESH_SALT,
        info = eraBE,
    ).deriveSecretToByteArray(oldPsk)
}

/** HKDF salt for [pairRefreshPsk] (`"desky-pair-refresh-v1"`, 21 bytes). */
val PAIR_REFRESH_SALT: ByteArray = "desky-pair-refresh-v1".encodeToByteArray()

/**
 * `PAIR_REFRESH_REQ` auth tag: `first6(HMAC-SHA256(oldPsk,
 * "PAIRREFRESH" || eraBE2 || counterBE4))`. The 17-byte message is the 11 ASCII
 * bytes of `"PAIRREFRESH"` followed by the 2-byte big-endian new era and the
 * 4-byte big-endian counter — must match firmware `pairing::pairRefreshTagMsg`.
 *
 * @param oldPsk 16-byte current pair PSK (the request authenticates under
 *   the OLD key; the ACK authenticates under the new one).
 * @param nextEra requested new era (current era + 1).
 * @param counter fresh drive counter (allocated from [DriveCounter], so the
 *   robot's `isFreshCounter` accepts it).
 * @return 6-byte tag.
 */
suspend fun pairRefreshTag(oldPsk: ByteArray, nextEra: UShort, counter: UInt): ByteArray {
    require(oldPsk.size == 16) { "pairRefreshTag: oldPsk must be 16 bytes" }
    val msg = "PAIRREFRESH".encodeToByteArray() + byteArrayOf(
        ((nextEra.toInt() shr 8) and 0xFF).toByte(),
        (nextEra.toInt() and 0xFF).toByte(),
        (counter shr 24).toByte(),
        (counter shr 16).toByte(),
        (counter shr 8).toByte(),
        counter.toByte(),
    )
    return hmacSha256(oldPsk, msg).copyOfRange(0, 6)
}

/**
 * `PAIR_REFRESH_ACK` auth tag: `first6(HMAC-SHA256(newPsk,
 * "PAIRREFRESHACK" || eraBE2))`. The 16-byte message is the 14 ASCII bytes of
 * `"PAIRREFRESHACK"` followed by the 2-byte big-endian new era — must match
 * firmware `pairing::pairRefreshAckTagMsg`. Authenticating under the NEW key
 * proves the robot switched.
 *
 * @param newPsk 16-byte derived next PSK (from [pairRefreshPsk]).
 * @param nextEra confirmed new era.
 * @return 6-byte tag.
 */
suspend fun pairRefreshAckTag(newPsk: ByteArray, nextEra: UShort): ByteArray {
    require(newPsk.size == 16) { "pairRefreshAckTag: newPsk must be 16 bytes" }
    val msg = "PAIRREFRESHACK".encodeToByteArray() + byteArrayOf(
        ((nextEra.toInt() shr 8) and 0xFF).toByte(),
        (nextEra.toInt() and 0xFF).toByte(),
    )
    return hmacSha256(newPsk, msg).copyOfRange(0, 6)
}

/**
 * `UNPAIR` auth tag: `first6(HMAC-SHA256(psk16, "UNPAIR" || counterBE4))`.
 * The 10-byte message is the 6 ASCII bytes of `"UNPAIR"` followed by the
 * 4-byte big-endian counter — must match the firmware `UNPAIR` frame exactly.
 *
 * @param psk 16-byte pair PSK.
 * @param counter next drive counter (`lastCounter + 1` at forget time).
 * @return 6-byte tag.
 */
suspend fun unpairTag(psk: ByteArray, counter: UInt): ByteArray {
    require(psk.size == 16) { "unpairTag: psk must be 16 bytes" }
    val msg = "UNPAIR".encodeToByteArray() + byteArrayOf(
        (counter shr 24).toByte(),
        (counter shr 16).toByte(),
        (counter shr 8).toByte(),
        counter.toByte(),
    )
    return hmacSha256(psk, msg).copyOfRange(0, 6)
}

/**
 * `ALIVE` auth tag: `first6(HMAC-SHA256(psk16, "ALIVE" || eraBE2 ||
 * counterBE4))`. The 11-byte message is the 5 ASCII bytes of `"ALIVE"`
 * followed by the 2-byte big-endian CURRENT era and the 4-byte big-endian
 * counter — must match firmware `pairing::aliveTagMsg`. The era here is
 * the stored era (a pair-refresh moves it); the counter is allocated from the
 * shared [DriveCounter], so a valid `ALIVE` consumes `lastCounter_` exactly
 * like drive.
 *
 * @param psk 16-byte pair PSK.
 * @param era current paired era.
 * @param counter next drive counter (strictly increasing, never 0).
 * @return 6-byte tag.
 */
suspend fun aliveTag(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "aliveTag: psk must be 16 bytes" }
    val msg = "ALIVE".encodeToByteArray() + byteArrayOf(
        ((era.toInt() shr 8) and 0xFF).toByte(),
        (era.toInt() and 0xFF).toByte(),
        (counter shr 24).toByte(),
        (counter shr 16).toByte(),
        (counter shr 8).toByte(),
        counter.toByte(),
    )
    return hmacSha256(psk, msg).copyOfRange(0, 6)
}

/**
 * `DISCONNECT` auth tag: `first6(HMAC-SHA256(psk16, "DISCONNECT" ||
 * eraBE2 || counterBE4))`. The 16-byte message is the 10 ASCII bytes of
 * `"DISCONNECT"` followed by the 2-byte big-endian CURRENT era and the
 * 4-byte big-endian counter — must match firmware
 * `pairing::disconnectTagMsg`. Graceful goodbye: the pair is KEPT (unlike
 * `UNPAIR`), so the next launch rejoins instantly.
 *
 * @param psk 16-byte pair PSK.
 * @param era current paired era.
 * @param counter next drive counter (strictly increasing, never 0).
 * @return 6-byte tag.
 */
suspend fun disconnectTag(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "disconnectTag: psk must be 16 bytes" }
    val msg = "DISCONNECT".encodeToByteArray() + byteArrayOf(
        ((era.toInt() shr 8) and 0xFF).toByte(),
        (era.toInt() and 0xFF).toByte(),
        (counter shr 24).toByte(),
        (counter shr 16).toByte(),
        (counter shr 8).toByte(),
        counter.toByte(),
    )
    return hmacSha256(psk, msg).copyOfRange(0, 6)
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
 * `DRIVE` auth tag: `first6(HMAC-SHA256(psk16,
 * counterBE4 || mode || thr || str || flags))`.
 *
 * @param psk 16-byte pair PSK.
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
