package com.desky.shared.core

private val K = longArrayOf(
    0x428a2f98L, 0x71374491L, 0xb5c0fbcfL, 0xe9b5dba5L,
    0x3956c25bL, 0x59f111f1L, 0x923f82a4L, 0xab1c5ed5L,
    0xd807aa98L, 0x12835b01L, 0x243185beL, 0x550c7dc3L,
    0x72be5d74L, 0x80deb1feL, 0x9bdc06a7L, 0xc19bf174L,
    0xe49b69c1L, 0xefbe4786L, 0x0fc19dc6L, 0x240ca1ccL,
    0x2de92c6fL, 0x4a7484aaL, 0x5cb0a9dcL, 0x76f988daL,
    0x983e5152L, 0xa831c66dL, 0xb00327c8L, 0xbf597fc7L,
    0xc6e00bf3L, 0xd5a79147L, 0x06ca6351L, 0x14292967L,
    0x27b70a85L, 0x2e1b2138L, 0x4d2c6dfcL, 0x53380d13L,
    0x650a7354L, 0x766a0abbL, 0x81c2c92eL, 0x92722c85L,
    0xa2bfe8a1L, 0xa81a664bL, 0xc24b8b70L, 0xc76c51a3L,
    0xd192e819L, 0xd6990624L, 0xf40e3585L, 0x106aa070L,
    0x19a4c116L, 0x1e376c08L, 0x2748774cL, 0x34b0bcb5L,
    0x391c0cb3L, 0x4ed8aa4aL, 0x5b9cca4fL, 0x682e6ff3L,
    0x748f82eeL, 0x78a5636fL, 0x84c87814L, 0x8cc70208L,
    0x90befffaL, 0xa4506cebL, 0xbef9a3f7L, 0xc67178f2L,
)

private val H0 = longArrayOf(
    0x6a09e667L, 0xbb67ae85L, 0x3c6ef372L, 0xa54ff53aL,
    0x510e527fL, 0x9b05688cL, 0x1f83d9abL, 0x5be0cd19L,
)

private fun rotr(x: Long, n: Int): Long = ((x ushr n) or (x shl (32 - n))) and 0xFFFFFFFFL

private fun sha256(input: ByteArray): ByteArray {
    val bitLen = input.size.toLong() * 8L
    // Padding: 0x80 then zeros until length == 56 (mod 64), then 8-byte length.
    val paddedLen = ((input.size + 1 + 8 + 63) / 64) * 64
    val msg = ByteArray(paddedLen)
    input.copyInto(msg)
    msg[input.size] = 0x80.toByte()
    for (i in 0 until 8) {
        msg[paddedLen - 1 - i] = ((bitLen ushr (8 * i)) and 0xFF).toByte()
    }

    val h = H0.copyOf()
    val w = LongArray(64)
    var off = 0
    while (off < paddedLen) {
        for (i in 0 until 16) {
            w[i] = (
                ((msg[off + i * 4].toLong() and 0xFF) shl 24) or
                    ((msg[off + i * 4 + 1].toLong() and 0xFF) shl 16) or
                    ((msg[off + i * 4 + 2].toLong() and 0xFF) shl 8) or
                    (msg[off + i * 4 + 3].toLong() and 0xFF)
                )
        }
        for (i in 16 until 64) {
            val s0 = rotr(w[i - 15], 7) xor rotr(w[i - 15], 18) xor (w[i - 15] ushr 3)
            val s1 = rotr(w[i - 2], 17) xor rotr(w[i - 2], 19) xor (w[i - 2] ushr 10)
            w[i] = (w[i - 16] + s0 + w[i - 7] + s1) and 0xFFFFFFFFL
        }
        var a = h[0]
        var b = h[1]
        var c = h[2]
        var d = h[3]
        var e = h[4]
        var f = h[5]
        var g = h[6]
        var hh = h[7]
        for (i in 0 until 64) {
            val s1 = rotr(e, 6) xor rotr(e, 11) xor rotr(e, 25)
            val ch = (e and f) xor ((e xor 0xFFFFFFFFL) and g)
            val t1 = (hh + s1 + ch + K[i] + w[i]) and 0xFFFFFFFFL
            val s0 = rotr(a, 2) xor rotr(a, 13) xor rotr(a, 22)
            val maj = (a and b) xor (a and c) xor (b and c)
            val t2 = (s0 + maj) and 0xFFFFFFFFL
            hh = g
            g = f
            f = e
            e = (d + t1) and 0xFFFFFFFFL
            d = c
            c = b
            b = a
            a = (t1 + t2) and 0xFFFFFFFFL
        }
        h[0] = (h[0] + a) and 0xFFFFFFFFL
        h[1] = (h[1] + b) and 0xFFFFFFFFL
        h[2] = (h[2] + c) and 0xFFFFFFFFL
        h[3] = (h[3] + d) and 0xFFFFFFFFL
        h[4] = (h[4] + e) and 0xFFFFFFFFL
        h[5] = (h[5] + f) and 0xFFFFFFFFL
        h[6] = (h[6] + g) and 0xFFFFFFFFL
        h[7] = (h[7] + hh) and 0xFFFFFFFFL
        off += 64
    }

    val out = ByteArray(32)
    for (i in 0 until 8) {
        out[i * 4] = ((h[i] ushr 24) and 0xFF).toByte()
        out[i * 4 + 1] = ((h[i] ushr 16) and 0xFF).toByte()
        out[i * 4 + 2] = ((h[i] ushr 8) and 0xFF).toByte()
        out[i * 4 + 3] = (h[i] and 0xFF).toByte()
    }
    return out
}

private val HEX = "0123456789abcdef".toCharArray()

/**
 * SHA-256 digest rendered as 64 lowercase hex chars. Dependency-free
 * (multiplatform; no JCA/MessageDigest) for pairing-token derivation.
 */
fun sha256Hex(input: ByteArray): String {
    val digest = sha256(input)
    val chars = CharArray(64)
    for (i in digest.indices) {
        val v = digest[i].toInt() and 0xFF
        chars[i * 2] = HEX[v ushr 4]
        chars[i * 2 + 1] = HEX[v and 0x0F]
    }
    return chars.concatToString()
}
