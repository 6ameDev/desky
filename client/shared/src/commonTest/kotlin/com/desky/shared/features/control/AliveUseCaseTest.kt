package com.desky.shared.features.control

import com.desky.shared.core.ALIVE_HEADER
import com.desky.shared.core.ALIVE_SIZE
import com.desky.shared.core.DISCONNECT_HEADER
import com.desky.shared.core.DISCONNECT_SIZE
import com.desky.shared.core.Outcome
import com.desky.shared.core.aliveTag
import com.desky.shared.core.disconnectTag
import com.desky.shared.core.encodeAlive
import com.desky.shared.core.encodeDisconnect
import com.desky.shared.core.hmacSha256
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

private const val BYTE_MASK = 0xFF
private const val HEX_RADIX = 16

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

// Fixed inputs (shared/protocol/README.md §ALIVE/DISCONNECT vectors,
// firmware pairing::aliveTagMsg/disconnectTagMsg layouts):
// psk=00112233445566778899aabbccddeeff, era=1, counter=100.
private val PSK = hex("00112233445566778899aabbccddeeff")
private const val ALIVE_TAG_HEX = "dd152baa54f7"
private const val ALIVE_FRAME_HEX = "55000100000064dd152baa54f7da"
private const val DISCONNECT_TAG_HEX = "ca90ca039040"
private const val DISCONNECT_FRAME_HEX = "56000100000064ca90ca03904070"

class AliveUseCaseTest {

    private companion object {
        const val EXPECTED_ALIVE_HEADER = 0x55
        const val EXPECTED_DISCONNECT_HEADER = 0x56
        const val EXPECTED_ALIVE_SIZE = 14
        const val EXPECTED_DISCONNECT_SIZE = 14
        const val ALIVE_HMAC_MSG_SIZE = 11
        const val DISCONNECT_HMAC_MSG_SIZE = 16
        const val AUTH_TAG_SIZE = 6
        const val SHORT_PSK_SIZE = 8
        const val PSK_BYTE_LENGTH = 16
    }

    @Test
    fun constants() {
        assertEquals(EXPECTED_ALIVE_HEADER, ALIVE_HEADER)
        assertEquals(EXPECTED_ALIVE_SIZE, ALIVE_SIZE)
        assertEquals(EXPECTED_DISCONNECT_HEADER, DISCONNECT_HEADER)
        assertEquals(EXPECTED_DISCONNECT_SIZE, DISCONNECT_SIZE)
    }

    @Test
    fun aliveTagMatchesFirmwareVector() = runTest {
        // msg = "ALIVE" || eraBE2(0001) || counterBE4(00000064).
        val tag = aliveTag(psk = PSK, era = 1u, counter = 100u)
        assertContentEquals(hex(ALIVE_TAG_HEX), tag)
    }

    @Test
    fun disconnectTagMatchesFirmwareVector() = runTest {
        // msg = "DISCONNECT" || eraBE2(0001) || counterBE4(00000064).
        val tag = disconnectTag(psk = PSK, era = 1u, counter = 100u)
        assertContentEquals(hex(DISCONNECT_TAG_HEX), tag)
    }

    /**
     * Golden `ALIVE` frame (psk=`0011...eeff`, era=1, counter=100). The
     * expected tag is built from a MANUALLY assembled 11-byte HMAC input
     * (`"ALIVE" ASCII || eraBE2 || counterBE4`) through the [hmacSha256]
     * primitive — never through [encodeAlive] itself — and the frame bytes
     * are laid out literally around it.
     */
    @Test
    fun aliveFrameGolden() = runTest {
        val hmacInput = "ALIVE".encodeToByteArray() + hex("000100000064")
        assertEquals(ALIVE_HMAC_MSG_SIZE, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, AUTH_TAG_SIZE)
        assertContentEquals(hex(ALIVE_TAG_HEX), tag)

        val body = byteArrayOf(0x55.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(ALIVE_SIZE - 1, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeAlive(PSK, 1u, 100u)

        assertEquals(ALIVE_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (README §ALIVE vectors): header + era + counter + tag + XOR.
        assertContentEquals(hex(ALIVE_FRAME_HEX), actual)
        assertEquals(xorOf(actual, ALIVE_SIZE - 1), actual[ALIVE_SIZE - 1].unsigned())
    }

    /**
     * Golden `DISCONNECT` frame (same fixed inputs). The expected tag is
     * built from a MANUALLY assembled 16-byte HMAC input (`"DISCONNECT"`
     * ASCII || eraBE2 || counterBE4) through [hmacSha256] — never through
     * [encodeDisconnect] itself.
     */
    @Test
    fun disconnectFrameGolden() = runTest {
        val hmacInput = "DISCONNECT".encodeToByteArray() + hex("000100000064")
        assertEquals(DISCONNECT_HMAC_MSG_SIZE, hmacInput.size)
        val tag = hmacSha256(PSK, hmacInput).copyOfRange(0, AUTH_TAG_SIZE)
        assertContentEquals(hex(DISCONNECT_TAG_HEX), tag)

        val body = byteArrayOf(0x56.toByte(), 0x00, 0x01, 0x00, 0x00, 0x00, 0x64) + tag
        assertEquals(DISCONNECT_SIZE - 1, body.size)
        val expected = body + byteArrayOf(xorOf(body, body.size).toByte())

        val actual = encodeDisconnect(PSK, 1u, 100u)

        assertEquals(DISCONNECT_SIZE, actual.size)
        assertContentEquals(expected, actual)
        // Literal golden (README §DISCONNECT vectors).
        assertContentEquals(hex(DISCONNECT_FRAME_HEX), actual)
        assertEquals(xorOf(actual, DISCONNECT_SIZE - 1), actual[DISCONNECT_SIZE - 1].unsigned())
    }

    @Test
    fun eraAndCounterLayOutBigEndian() = runTest {
        val psk = ByteArray(PSK_BYTE_LENGTH) { it.toByte() }
        val alive = encodeAlive(psk, 0x0102u, 0x01020304u)
        assertEquals(0x55, alive[0].unsigned())
        assertEquals(0x01, alive[1].unsigned())
        assertEquals(0x02, alive[2].unsigned())
        assertEquals(0x01, alive[3].unsigned())
        assertEquals(0x02, alive[4].unsigned())
        assertEquals(0x03, alive[5].unsigned())
        assertEquals(0x04, alive[6].unsigned())
        assertEquals(xorOf(alive, ALIVE_SIZE - 1), alive[ALIVE_SIZE - 1].unsigned())

        val disc = encodeDisconnect(psk, 0x0102u, 0x01020304u)
        assertEquals(0x56, disc[0].unsigned())
        assertEquals(0x01, disc[1].unsigned())
        assertEquals(0x02, disc[2].unsigned())
        assertEquals(0x01, disc[3].unsigned())
        assertEquals(0x02, disc[4].unsigned())
        assertEquals(0x03, disc[5].unsigned())
        assertEquals(0x04, disc[6].unsigned())
        assertEquals(xorOf(disc, DISCONNECT_SIZE - 1), disc[DISCONNECT_SIZE - 1].unsigned())
    }

    @Test
    fun rejectsShortPsk() = runTest {
        assertFailsWith<IllegalArgumentException> {
            encodeAlive(ByteArray(SHORT_PSK_SIZE), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            encodeDisconnect(ByteArray(SHORT_PSK_SIZE), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            aliveTag(ByteArray(SHORT_PSK_SIZE), 1u, 1u)
        }
        assertFailsWith<IllegalArgumentException> {
            disconnectTag(ByteArray(SHORT_PSK_SIZE), 1u, 1u)
        }
    }

    @Test
    fun sendsAliveWithGivenCounterToPairedEndpoint() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val useCase = AliveUseCase(udp)

        val result = useCase(device, PSK, 1u, 100u)

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, udp.sent.size)
        val sent = udp.sent[0]
        assertContentEquals(hex(ALIVE_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)
    }

    @Test
    fun sendsDisconnectWithGivenCounterToPairedEndpoint() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager()
        val useCase = AliveUseCase(udp)

        val result = useCase.sendDisconnect(device, PSK, 1u, 100u)

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, udp.sent.size)
        val sent = udp.sent[0]
        assertContentEquals(hex(DISCONNECT_FRAME_HEX), sent.bytes)
        assertEquals(device.ip, sent.host)
        assertEquals(device.port, sent.port)
    }

    @Test
    fun sendErrorsMapToFailureWithoutThrowing() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = IllegalStateException("dead socket") }
        val useCase = AliveUseCase(udp)

        // Silent by contract (telemetry is the ACK): failures map, never throw.
        val alive = useCase(device, PSK, 1u, 100u)
        assertIs<Outcome.Failure>(alive)
        val disc = useCase.sendDisconnect(device, PSK, 1u, 100u)
        assertIs<Outcome.Failure>(disc)
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun cancellationPropagates() = runTest {
        val device = testDevice()
        val udp = FakeUdpSocketManager().apply { sendError = CancellationException("cancelled") }
        val useCase = AliveUseCase(udp)

        assertFailsWith<CancellationException> {
            useCase(device, PSK, 1u, 100u)
        }
        assertFailsWith<CancellationException> {
            useCase.sendDisconnect(device, PSK, 1u, 100u)
        }
    }

    @Test
    fun tagsDifferByEraCounterAndDomain() = runTest {
        val base = aliveTag(psk = PSK, era = 1u, counter = 100u)
        assertTrue(
            aliveTag(psk = PSK, era = 2u, counter = 100u).contentEquals(base).not(),
            "era is domain-separated",
        )
        assertTrue(
            aliveTag(psk = PSK, era = 1u, counter = 101u).contentEquals(base).not(),
            "counter is domain-separated",
        )
        assertTrue(
            disconnectTag(psk = PSK, era = 1u, counter = 100u).contentEquals(base).not(),
            "ALIVE and DISCONNECT domains differ",
        )
    }
}
