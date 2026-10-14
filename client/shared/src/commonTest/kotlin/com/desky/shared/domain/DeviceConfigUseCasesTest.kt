package com.desky.shared.domain

import app.cash.turbine.test
import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.every
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlinx.coroutines.flow.MutableStateFlow
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
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

    @Test
    fun clearSucceeds() = runTest {
        val deviceRepo = mock<DeviceRepository>()
        everySuspend { deviceRepo.clear() } returns Unit

        val result = ForgetDeviceUseCase(deviceRepo)()

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend { deviceRepo.clear() }
    }

    @Test
    fun clearThrowingMapsToFailure() = runTest {
        val deviceRepo = mock<DeviceRepository>()
        everySuspend { deviceRepo.clear() } calls { throw IllegalStateException("db locked") }

        val result = ForgetDeviceUseCase(deviceRepo)()

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }
}
