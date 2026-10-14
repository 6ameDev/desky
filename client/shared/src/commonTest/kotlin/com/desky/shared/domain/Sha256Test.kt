package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class Sha256Test {

    @Test
    fun abcStandardVector() {
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            sha256Hex("abc".encodeToByteArray()),
        )
    }

    @Test
    fun emptyStringVector() {
        // Second NIST vector: guards the padding path for empty input.
        assertEquals(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            sha256Hex(ByteArray(0)),
        )
    }

    @Test
    fun outputIsAlways64LowerHex() {
        for (input in listOf("", "a", "desky", "192.168.4.1|1234|desky-v1")) {
            val hex = sha256Hex(input.encodeToByteArray())
            assertEquals(64, hex.length, input)
            assertTrue(hex.all { it in '0'..'9' || it in 'a'..'f' }, input)
        }
    }

    @Test
    fun deterministicAndAvalanche() {
        val a = sha256Hex("192.168.4.1|1234|desky-v1".encodeToByteArray())
        val b = sha256Hex("192.168.4.1|1234|desky-v1".encodeToByteArray())
        assertEquals(a, b)
        // One-char PIN change must flip the digest (not asserting bit counts,
        // just that the token actually binds the PIN).
        val c = sha256Hex("192.168.4.1|1235|desky-v1".encodeToByteArray())
        assertTrue(a != c)
    }
}
