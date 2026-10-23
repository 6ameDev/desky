package com.desky.shared.domain

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

// Fixed inputs (independent python hmac reference: HMAC-SHA256(psk,
// "MANUALTAKE"/"MANUALRELEASE" || eraBE2 || counterBE4), first 6 bytes):
// psk=00112233445566778899aabbccddeeff, era=1, counter=100.
private val PSK = hex("00112233445566778899aabbccddeeff")
private const val TAKE_TAG_HEX = "1ee989586f8e"
private const val TAKE_FRAME_HEX = "570001000000641ee989586f8ef5"
private const val RELEASE_TAG_HEX = "e2ce426715b5"
private const val RELEASE_FRAME_HEX = "58000100000064e2ce426715b594"

class ManualModeUseCasesTest {

    @Test
    fun constants() {
        assertEquals(0x57, MANUAL_TAKE_HEADER)
        assertEquals(14, MANUAL_TAKE_SIZE)
        assertEquals(0x58, MANUAL_RELEASE_HEADER)
        assertEquals(14, MANUAL_RELEASE_SIZE)
    }

    @Test
    fun takeTagMatchesIndependentVector() = runTest {
        // msg = "MANUALTAKE" || eraBE2(0001) || counterBE4(00000064), 16 bytes.
        val tag = manualTakeTag(psk = PSK, era = 1u, counter = 100u)
        assertContentEquals(hex(TAKE_TAG_HEX), tag)
    }

    @Test
    fun releaseTagMatchesIndependentVector() = runTest {
        // msg = "MANUALRELEASE" || eraBE2(0001) || counterBE4(00000064), 19 bytes.
        val tag = manualReleaseTag(psk = PSK, era = 1u, counter = 100u)
        assertContentEquals(hex(RELEASE_TAG_HEX), tag)
    }

    /**
     * Golden `MANUAL_TAKE` frame (psk=`0011...eeff`, era=1, counter=100).
     * The expected tag is built from a MANUALLY assembled 16-byte HMAC
     * input (`"MANUALTAKE"` ASCII || eraBE2 || counterBE4) through the
     * [hmacSha256] primitive — never through [encodeManualTake] itself —
     * and the frame bytes are laid out literally around it.
     */
    @Test
    fun takeFrameGolden() = runTest {
        val hmacInput = "MANUALTAKE".encodeToByteArray() + hex("000100000064")
        assertEquals(16, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, 6)
        assertContentEquals(hex(TAKE_TAG_HEX), tag)

        val body = byteArrayOf(0x57.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(13, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeManualTake(PSK, 1u, 100u)

        assertEquals(MANUAL_TAKE_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (independent python reference).
        assertContentEquals(hex(TAKE_FRAME_HEX), actual)
        assertEquals(xorOf(actual, MANUAL_TAKE_SIZE - 1), actual[MANUAL_TAKE_SIZE - 1].unsigned())
    }

    /**
     * Golden `MANUAL_RELEASE` frame (same fixed inputs). The expected tag is
     * built from a MANUALLY assembled 19-byte HMAC input (`"MANUALRELEASE"`
     * ASCII || eraBE2 || counterBE4) through [hmacSha256] — never through
     * [encodeManualRelease] itself.
     */
    @Test
    fun releaseFrameGolden() = runTest {
        val hmacInput = "MANUALRELEASE".encodeToByteArray() + hex("000100000064")
        assertEquals(19, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, 6)
        assertContentEquals(hex(RELEASE_TAG_HEX), tag)

        val body = byteArrayOf(0x58.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(13, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeManualRelease(PSK, 1u, 100u)

        assertEquals(MANUAL_RELEASE_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (independent python reference).
        assertContentEquals(hex(RELEASE_FRAME_HEX), actual)
        assertEquals(xorOf(actual, MANUAL_RELEASE_SIZE - 1), actual[MANUAL_RELEASE_SIZE - 1].unsigned())
    }

    @Test
    fun eraAndCounterLayOutBigEndian() = runTest {
        val psk = ByteArray(16) { it.toByte() }
        val take = encodeManualTake(psk, 0x0102u, 0x01020304u)
        assertEquals(0x57, take[0].unsigned())
        assertEquals(0x01, take[1].unsigned())
        assertEquals(0x02, take[2].unsigned())
        assertEquals(0x01, take[3].unsigned())
        assertEquals(0x02, take[4].unsigned())
        assertEquals(0x03, take[5].unsigned())
        assertEquals(0x04, take[6].unsigned())
        assertEquals(xorOf(take, MANUAL_TAKE_SIZE - 1), take[MANUAL_TAKE_SIZE - 1].unsigned())

        val release = encodeManualRelease(psk, 0x0102u, 0x01020304u)
        assertEquals(0x58, release[0].unsigned())
        assertEquals(0x01, release[1].unsigned())
        assertEquals(0x02, release[2].unsigned())
        assertEquals(0x01, release[3].unsigned())
        assertEquals(0x02, release[4].unsigned())
        assertEquals(0x03, release[5].unsigned())
        assertEquals(0x04, release[6].unsigned())
        assertEquals(xorOf(release, MANUAL_RELEASE_SIZE - 1), release[MANUAL_RELEASE_SIZE - 1].unsigned())
    }

    @Test
    fun rejectsShortPsk() = runTest {
        assertFailsWith<IllegalArgumentException> {
            encodeManualTake(ByteArray(8), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeManualRelease(ByteArray(8), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            manualTakeTag(ByteArray(8), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            manualReleaseTag(ByteArray(8), 1u, 1u)
        }
    }

    @Test
    fun takeSendsExactlyOnceToPairedEndpoint() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val useCase = TakeManualUseCase(udp)

        val result = useCase(device, PSK, 1u, 100u)

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, udp.sent.size, "entry is deliberate: one frame per action, never repeated")
        val sent = udp.sent[0]
        assertContentEquals(hex(TAKE_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)
    }

    @Test
    fun releaseSendsTwiceByteIdentical() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val useCase = ReleaseManualUseCase(udp)

        val result = useCase(device, PSK, 1u, 100u)

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(ReleaseManualUseCase.MAX_RELEASE_SENDS, udp.sent.size)
        assertEquals(2, udp.sent.size)
        assertContentEquals(hex(RELEASE_FRAME_HEX), udp.sent[0].bytes)
        assertContentEquals(udp.sent[0].bytes, udp.sent[1].bytes, "one allocation: byte-identical repeat")
        udp.sent.forEach { sent ->
            assertEquals(device.ip, sent.host)
            assertEquals(device.port, sent.port)
        }
    }

    @Test
    fun takeSendErrorMapsToFailureWithoutThrowing() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val useCase = TakeManualUseCase(udp)

        val result = useCase(device, PSK, 1u, 100u)

        assertIs<Outcome.Failure>(result)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun releaseSendErrorsSwallowedBestEffort() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val useCase = ReleaseManualUseCase(udp)

        // Best-effort by contract: transmit errors never surface, never throw.
        val result = useCase(device, PSK, 1u, 100u)

        assertIs<Outcome.Success<Unit>>(result)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun cancellationPropagates() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = CancellationException("cancelled") }

        assertFailsWith<CancellationException> {
            TakeManualUseCase(udp)(device, PSK, 1u, 100u)
        }
        assertFailsWith<CancellationException> {
            ReleaseManualUseCase(udp)(device, PSK, 1u, 100u)
        }
    }

    @Test
    fun tagsDifferByEraCounterAndDomain() = runTest {
        val base = manualTakeTag(psk = PSK, era = 1u, counter = 100u)
        assertTrue(
            manualTakeTag(psk = PSK, era = 2u, counter = 100u).contentEquals(base).not(),
            "era is domain-separated",
        )
        assertTrue(
            manualTakeTag(psk = PSK, era = 1u, counter = 101u).contentEquals(base).not(),
            "counter is domain-separated",
        )
        assertTrue(
            manualReleaseTag(psk = PSK, era = 1u, counter = 100u).contentEquals(base).not(),
            "TAKE and RELEASE domains differ",
        )
        assertTrue(
            aliveTag(psk = PSK, era = 1u, counter = 100u).contentEquals(base).not(),
            "ALIVE and MANUAL_TAKE domains differ",
        )
        assertTrue(
            disconnectTag(psk = PSK, era = 1u, counter = 100u).contentEquals(
                manualReleaseTag(psk = PSK, era = 1u, counter = 100u),
            ).not(),
            "DISCONNECT and MANUAL_RELEASE domains differ",
        )
    }
}
