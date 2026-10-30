package com.desky.shared.features.pairing

import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.encodeUnpair
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.every
import dev.mokkery.everySuspend
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ForgetDeviceUseCaseTest {

    private val device = PairedDevice(id = "desky-1", ip = ROBOT_HOST, authToken = "psk-v1")

    private fun hex(s: String): ByteArray {
        require(s.length % 2 == 0)
        return ByteArray(s.length / 2) { i ->
            s.substring(i * 2, i * 2 + 2).toInt(HEX_RADIX).toByte()
        }
    }

    private fun useCase(
        repo: FakeDeviceRepository,
        vault: FakePairSecretStore,
        udp: FakeUdpSocketManager,
        holder: ToRobotCounter = ToRobotCounter(repo),
        fromRobot: FromRobotCounter = FromRobotCounter(repo),
    ) = ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), holder, fromRobot)

    @Test
    fun clearSucceeds() = runTest {
        val deviceRepo = mock<DeviceRepository>()
        every { deviceRepo.observePaired() } returns flowOf(device)
        everySuspend { deviceRepo.clear() } returns Unit
        val vault = FakePairSecretStore().apply { seed(device.id, "aa") }
        val udp = FakeUdpSocketManager()

        val result = ForgetDeviceUseCase(deviceRepo, vault, udp, GetPairPskUseCase(vault, deviceRepo), ToRobotCounter(deviceRepo), FromRobotCounter(deviceRepo))()

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend { deviceRepo.clear() }
        assertNull(vault.loadPsk(device.id))
        // "aa" is not a 16-byte PSK: the unpair encode throws, the send is
        // skipped, and the local forget still proceeds.
        assertTrue(udp.sent.isEmpty())
    }

    @Test
    fun clearThrowingMapsToFailure() = runTest {
        val deviceRepo = mock<DeviceRepository>()
        every { deviceRepo.observePaired() } returns flowOf(device)
        everySuspend { deviceRepo.clear() } calls { throw IllegalStateException("db locked") }
        val vault = FakePairSecretStore()
        val udp = FakeUdpSocketManager()

        val result = ForgetDeviceUseCase(deviceRepo, vault, udp, GetPairPskUseCase(vault, deviceRepo), ToRobotCounter(deviceRepo), FromRobotCounter(deviceRepo))()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }

    @Test
    fun forgetSendsSignedUnpairThenClears() = runTest {
        val pskHex = "00112233445566778899aabbccddeeff"
        val paired = device.copy(lastCounter = 5u)
        val repo = FakeDeviceRepository(paired)
        val vault = FakePairSecretStore().apply { seed(paired.id, pskHex) }
        val udp = FakeUdpSocketManager()

        val result = useCase(repo, vault, udp)()

        assertIs<Outcome.Success<Unit>>(result)
        // Fire-and-forget x3 at the stored endpoint, counter = lastCounter+1.
        assertEquals(EXPECTED_UNPAIR_SENDS, udp.sent.size)
        val expected = encodeUnpair(hex(pskHex), 6u)
        for (sent in udp.sent) {
            assertContentEquals(expected, sent.bytes)
            assertEquals(paired.ip, sent.host)
            assertEquals(paired.port, sent.port)
        }
        assertEquals(1, repo.clearCalls)
        assertNull(vault.loadPsk(paired.id))
        assertEquals(1, vault.clearCalls)
    }

    @Test
    fun forgetUsesLiveCounterNotWatermark() = runTest {
        val pskHex = "00112233445566778899aabbccddeeff"
        val paired = device.copy(lastCounter = 5u)
        val repo = FakeDeviceRepository(paired)
        val vault = FakePairSecretStore().apply { seed(paired.id, pskHex) }
        val udp = FakeUdpSocketManager()
        // Ten drive sends since the last persist: live is 15, watermark 5.
        val holder = ToRobotCounter(repo)
        holder.ensure(paired.id, paired.lastCounter)
        repeat(UNSAVED_DRIVE_SENDS) { holder.next() }

        val result = useCase(repo, vault, udp, holder)()

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(EXPECTED_UNPAIR_SENDS, udp.sent.size)
        val expected = encodeUnpair(hex(pskHex), 16u)
        for (sent in udp.sent) {
            assertContentEquals(expected, sent.bytes)
        }
        assertTrue(repo.counterSaves.isNotEmpty(), "Layer A: flush persists the live watermark before UNPAIR")
        assertEquals(listOf(paired.id to 15u), repo.counterSaves)
    }

    @Test
    fun forgetWithExhaustedCounterSendsNothing() = runTest {
        val paired = device.copy(lastCounter = UInt.MAX_VALUE)
        val repo = FakeDeviceRepository(paired)
        val vault = FakePairSecretStore().apply {
            seed(paired.id, "00112233445566778899aabbccddeeff")
        }
        val udp = FakeUdpSocketManager()

        val result = useCase(repo, vault, udp)()

        // Null allocation skips the network send; the local forget proceeds.
        assertIs<Outcome.Success<Unit>>(result)
        assertTrue(udp.sent.isEmpty())
        assertEquals(1, repo.clearCalls)
        assertNull(vault.loadPsk(paired.id))
    }

    @Test
    fun forgetWithMissingPskSendsNothing() = runTest {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore()
        val udp = FakeUdpSocketManager()

        val result = useCase(repo, vault, udp)()

        assertIs<Outcome.Success<Unit>>(result)
        assertTrue(udp.sent.isEmpty(), "no PSK -> no network send, local forget as before")
        assertEquals(1, repo.clearCalls)
        assertEquals(1, vault.clearCalls)
    }

    @Test
    fun forgetProceedsDespiteSendThrow() = runTest {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply {
            seed(device.id, "00112233445566778899aabbccddeeff")
        }
        val udp = FakeUdpSocketManager().apply {
            sendError = IllegalStateException("socket down")
        }

        val result = useCase(repo, vault, udp)()

        // Send errors are swallowed: the local forget proceeds regardless.
        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(1, repo.clearCalls)
        assertNull(vault.loadPsk(device.id))
    }

    @Test
    fun forgetWhenUnpairedSendsNothing() = runTest {
        val repo = FakeDeviceRepository(null)
        val vault = FakePairSecretStore()
        val udp = FakeUdpSocketManager()

        val result = useCase(repo, vault, udp)()

        assertIs<Outcome.Success<Unit>>(result)
        assertTrue(udp.sent.isEmpty())
        assertEquals(1, repo.clearCalls)
        assertEquals(0, vault.clearCalls)
    }

    private companion object {
        const val HEX_RADIX = 16
        const val UNSAVED_DRIVE_SENDS = 10
        const val EXPECTED_UNPAIR_SENDS = 3
    }
}
