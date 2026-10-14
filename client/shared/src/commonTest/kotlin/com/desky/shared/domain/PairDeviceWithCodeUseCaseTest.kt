package com.desky.shared.domain

import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

private val HEX64 = Regex("[0-9a-f]{64}")

class PairDeviceWithCodeUseCaseTest {

    private fun repo() = mock<DeviceRepository>()

    @Test
    fun validIpAndPinYieldsExpectedIdAndToken() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } returns Unit
        val useCase = PairDeviceWithCodeUseCase(deviceRepo)

        val result = useCase("192.168.4.1", "1234")

        val outcome = assertIs<Outcome.Success<PairedDevice>>(result)
        // Token recomputed independently (Python hashlib reference vector).
        assertEquals("desky-192.168.4.1", outcome.data.id)
        assertEquals("192.168.4.1", outcome.data.ip)
        assertEquals(DEFAULT_UDP_PORT, outcome.data.port)
        assertEquals(
            "709cc6d2999c78d4a3ed9a9fa1a071cc6ddc13299c23423b4cc14936b8db263f",
            outcome.data.authToken,
        )
        verifySuspend { deviceRepo.save(outcome.data) }
    }

    @Test
    fun validVectorsCoverIpEdgesPinLengthsAndHostnames() = runTest {
        val cases = listOf(
            "10.0.0.1" to "0000", // min PIN length
            "255.255.255.255" to "12345678", // max octets + max PIN length
            "0.0.0.0" to "12345",
            "desky-robot" to "9999", // hostname
            "desky.local" to "0123456", // dotted hostname
            "ROBOT-01.lan" to "4242",
        )
        for ((ip, pin) in cases) {
            val deviceRepo = repo()
            everySuspend { deviceRepo.save(any()) } returns Unit
            val result = PairDeviceWithCodeUseCase(deviceRepo)(ip, pin)
            val outcome = assertIs<Outcome.Success<PairedDevice>>(result, "$ip / $pin")
            assertEquals("desky-$ip", outcome.data.id, "$ip / $pin")
            // Self-consistency: token binds exactly ip|pin|desky-v1.
            assertEquals(
                sha256Hex("$ip|$pin|desky-v1".encodeToByteArray()),
                outcome.data.authToken,
                "$ip / $pin",
            )
            assertTrue(HEX64.matches(outcome.data.authToken), "$ip / $pin")
            verifySuspend { deviceRepo.save(outcome.data) }
        }
    }

    @Test
    fun invalidVectorsReturnFailureWithReason() = runTest {
        val cases = listOf(
            "" to "1234", // blank IP
            "   " to "1234", // whitespace-only IP
            "999.1.1.1" to "1234", // octet > 255 (must not pass as hostname)
            "1.2.3" to "1234", // too few octets
            "1.2.3.4.5" to "1234", // too many octets
            "1.2.3.x" to "1234", // non-numeric octet
            "192.168.4.1" to "123", // PIN too short
            "192.168.4.1" to "123456789", // PIN too long
            "192.168.4.1" to "12a4", // non-digit PIN
            "192.168.4.1" to "", // empty PIN
            "192.168.4.1" to "１２３４", // full-width digits are not ASCII
            "-bad-.local" to "1234", // leading-hyphen label
            "bad..dots" to "1234", // empty label
            "under_score" to "1234", // underscore not in LDH
        )
        for ((ip, pin) in cases) {
            val deviceRepo = repo()
            val result = PairDeviceWithCodeUseCase(deviceRepo)(ip, pin)
            val failure = assertIs<Outcome.Failure>(result, "$ip / $pin")
            assertTrue(failure.reason.isNotBlank(), "$ip / $pin")
        }
    }

    @Test
    fun saveThrowingMapsToFailure() = runTest {
        val deviceRepo = repo()
        everySuspend { deviceRepo.save(any()) } calls { throw IllegalStateException("disk gone") }

        val result = PairDeviceWithCodeUseCase(deviceRepo)("192.168.4.1", "1234")

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }
}
