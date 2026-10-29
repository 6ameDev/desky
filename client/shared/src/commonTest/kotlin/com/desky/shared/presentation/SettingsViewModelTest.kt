package com.desky.shared.presentation

import app.cash.turbine.test
import com.desky.shared.domain.ToRobotCounter
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.ObserveDeviceConfigUseCase
import com.desky.shared.domain.RobotConfig
import com.desky.shared.domain.SaveDeviceConfigUseCase
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class SettingsViewModelTest {

    private fun vm(repo: FakeSettingsRepository, forgetRepo: FakeDeviceRepository = FakeDeviceRepository()) =
        SettingsViewModel(
            SaveDeviceConfigUseCase(repo),
            ObserveDeviceConfigUseCase(repo),
            forgetUseCase(forgetRepo),
        )

    private fun forgetUseCase(forgetRepo: FakeDeviceRepository): ForgetDeviceUseCase {
        val vault = FakePairSecretStore()
        return ForgetDeviceUseCase(
            forgetRepo,
            vault,
            FakeUdpSocketManager(),
            GetPairPskUseCase(vault, forgetRepo),
            ToRobotCounter(forgetRepo),
            FromRobotCounter(forgetRepo),
        )
    }

    @Test
    fun initCollectsStoredConfig() = runVmTest { main ->
        val stored = RobotConfig(speedLimit = 0.6f, ledProfile = 2, pidP = 1f, wifiSsid = "bench")
        val viewModel = vm(FakeSettingsRepository(stored))
        viewModel.state.test {
            assertEquals(SettingsUiState(), awaitItem())
            main.advanceUntilIdle()
            assertEquals(stored, awaitItem().config)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun speedLimitUpdatesStateAndPersists() = runVmTest { main ->
        val repo = FakeSettingsRepository()
        val viewModel = vm(repo)
        main.runCurrent()

        // Gate the save so the in-flight window is observable.
        repo.saveGate = CompletableDeferred()
        viewModel.onSpeedLimit(TEST_SPEED_LIMIT)
        // State updates synchronously; the save is parked on the gate.
        assertEquals(TEST_SPEED_LIMIT, viewModel.state.value.config.speedLimit)
        main.runCurrent()
        assertTrue(viewModel.state.value.isSaving)
        assertTrue(repo.saved.isEmpty())

        repo.saveGate!!.complete(Unit)
        main.advanceUntilIdle()

        assertFalse(viewModel.state.value.isSaving)
        assertNull(viewModel.state.value.error)
        assertEquals(1, repo.saved.size)
        assertEquals(TEST_SPEED_LIMIT, repo.saved.last().speedLimit)
    }

    @Test
    fun allEditorsPersist() = runVmTest { main ->
        val repo = FakeSettingsRepository()
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onLedProfile(TEST_LED_PROFILE)
        viewModel.onPid(1f, TEST_PID_I, TEST_PID_D)
        viewModel.onWifiSsid("desky-bench")
        main.advanceUntilIdle()

        val config = viewModel.state.value.config
        assertEquals(TEST_LED_PROFILE, config.ledProfile)
        assertEquals(1f, config.pidP)
        assertEquals(TEST_PID_I, config.pidI)
        assertEquals(TEST_PID_D, config.pidD)
        assertEquals("desky-bench", config.wifiSsid)
        // Every edit persisted (serialized, none dropped).
        assertEquals(EXPECTED_SAVE_COUNT, repo.saved.size)
        assertEquals(config, repo.saved.last())
        assertFalse(viewModel.state.value.isSaving)
    }

    @Test
    fun saveFailureSurfacesError() = runVmTest { main ->
        val repo = FakeSettingsRepository().apply {
            saveError = IllegalStateException("db locked")
        }
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onSpeedLimit(FAILING_SPEED_LIMIT)
        main.advanceUntilIdle()

        // State still reflects the edit; the error reports the failed save.
        assertEquals(FAILING_SPEED_LIMIT, viewModel.state.value.config.speedLimit)
        assertFalse(viewModel.state.value.isSaving)
        assertTrue(viewModel.state.value.error!!.contains("db locked"))
    }

    @Test
    fun invalidEditMapsToErrorState() = runVmTest { main ->
        val viewModel = vm(FakeSettingsRepository())
        main.runCurrent()

        viewModel.onSpeedLimit(INVALID_SPEED_LIMIT)
        main.advanceUntilIdle()

        // Rejected by model validation: config untouched, error set.
        assertEquals(1f, viewModel.state.value.config.speedLimit)
        assertNotNull(viewModel.state.value.error)
    }

    @Test
    fun forgetDeviceSetsUnpaired() = runVmTest { main ->
        val forgetRepo = FakeDeviceRepository(testDevice())
        val viewModel = vm(FakeSettingsRepository(), forgetRepo)
        main.runCurrent()
        assertFalse(viewModel.state.value.unpaired)

        viewModel.onForgetDevice()
        main.advanceUntilIdle()

        assertTrue(viewModel.state.value.unpaired)
        assertNull(viewModel.state.value.error)
        assertEquals(1, forgetRepo.clearCalls)
    }

    @Test
    fun forgetFailureSurfacesError() = runVmTest { main ->
        val forgetRepo = FakeDeviceRepository(testDevice()).apply {
            clearError = IllegalStateException("db locked")
        }
        val viewModel = vm(FakeSettingsRepository(), forgetRepo)
        main.runCurrent()

        viewModel.onForgetDevice()
        main.advanceUntilIdle()

        assertFalse(viewModel.state.value.unpaired)
        assertNotNull(viewModel.state.value.error)
    }

    private companion object {
        const val TEST_SPEED_LIMIT = 0.5f
        const val FAILING_SPEED_LIMIT = 0.4f
        const val INVALID_SPEED_LIMIT = 9f
        const val TEST_LED_PROFILE = 3
        const val EXPECTED_SAVE_COUNT = 3
        const val TEST_PID_I = 0.5f
        const val TEST_PID_D = 0.1f
    }
}
