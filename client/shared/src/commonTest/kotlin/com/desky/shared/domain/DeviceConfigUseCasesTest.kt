package com.desky.shared.domain

import app.cash.turbine.test
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakeUdpSocketManager
import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.every
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

class DeviceConfigUseCasesTest {

    @Test
    fun validSavePersistsAndReturnsSuccess() = runTest {
        val settingsRepo = mock<SettingsRepository>()
        everySuspend { settingsRepo.save(any()) } returns Unit
        val config = RobotConfig(speedLimit = 0.8f)

        val result = SaveDeviceConfigUseCase(settingsRepo)(config)

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend { settingsRepo.save(config) }
    }

    @Test
    fun saveThrowingMapsToFailure() = runTest {
        val settingsRepo = mock<SettingsRepository>()
        everySuspend { settingsRepo.save(any()) } calls { throw IllegalStateException("db locked") }

        val result = SaveDeviceConfigUseCase(settingsRepo)(RobotConfig(speedLimit = 0.5f))

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }

    @Test
    fun illegalArgumentFromRepoMapsToFailure() = runTest {
        // RobotConfig validates in init, so an invalid config cannot be
        // constructed here; this covers the use case's IllegalArgumentException
        // mapping when a repo (or future validation move) rejects the value.
        val settingsRepo = mock<SettingsRepository>()
        everySuspend { settingsRepo.save(any()) } calls { throw IllegalArgumentException("speedLimit bogus") }

        val result = SaveDeviceConfigUseCase(settingsRepo)(RobotConfig(speedLimit = 0.5f))

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("Invalid robot config"))
        assertIs<IllegalArgumentException>(failure.cause)
    }

    @Test
    fun observePassesRepoFlowThrough() = runTest {
        val settingsRepo = mock<SettingsRepository>()
        val configs = MutableStateFlow(RobotConfig(speedLimit = 0.5f))
        every { settingsRepo.observeConfig() } returns configs

        ObserveDeviceConfigUseCase(settingsRepo)().test {
            assertEquals(RobotConfig(speedLimit = 0.5f), awaitItem())
            configs.value = RobotConfig(speedLimit = 0.9f)
            assertEquals(RobotConfig(speedLimit = 0.9f), awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }
}

class ForgetDeviceUseCaseTest {

    private val device = PairedDevice(id = "desky-1", ip = "192.168.4.1", authToken = "psk-v1")

    private fun hex(s: String): ByteArray {
        require(s.length % 2 == 0)
        return ByteArray(s.length / 2) { i ->
            s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
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
        assertEquals(3, udp.sent.size)
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
        repeat(10) { holder.next() }

        val result = useCase(repo, vault, udp, holder)()

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(3, udp.sent.size)
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
}
