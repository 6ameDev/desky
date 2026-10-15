package com.desky.shared.presentation

import app.cash.turbine.test
import com.desky.shared.domain.ForgetDeviceUseCase
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
            ForgetDeviceUseCase(forgetRepo, FakeBondSecretStore()),
        )

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
        viewModel.onSpeedLimit(0.5f)
        // State updates synchronously; the save is parked on the gate.
        assertEquals(0.5f, viewModel.state.value.config.speedLimit)
        main.runCurrent()
        assertTrue(viewModel.state.value.isSaving)
        assertTrue(repo.saved.isEmpty())

        repo.saveGate!!.complete(Unit)
        main.advanceUntilIdle()

        assertFalse(viewModel.state.value.isSaving)
        assertNull(viewModel.state.value.error)
        assertEquals(1, repo.saved.size)
        assertEquals(0.5f, repo.saved.last().speedLimit)
    }

    @Test
    fun allEditorsPersist() = runVmTest { main ->
        val repo = FakeSettingsRepository()
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onLedProfile(3)
        viewModel.onPid(1f, 0.5f, 0.1f)
        viewModel.onWifiSsid("desky-bench")
        main.advanceUntilIdle()

        val config = viewModel.state.value.config
        assertEquals(3, config.ledProfile)
        assertEquals(1f, config.pidP)
        assertEquals(0.5f, config.pidI)
        assertEquals(0.1f, config.pidD)
        assertEquals("desky-bench", config.wifiSsid)
        // Every edit persisted (serialized, none dropped).
        assertEquals(3, repo.saved.size)
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

        viewModel.onSpeedLimit(0.4f)
        main.advanceUntilIdle()

        // State still reflects the edit; the error reports the failed save.
        assertEquals(0.4f, viewModel.state.value.config.speedLimit)
        assertFalse(viewModel.state.value.isSaving)
        assertTrue(viewModel.state.value.error!!.contains("db locked"))
    }

    @Test
    fun invalidEditMapsToErrorState() = runVmTest { main ->
        val viewModel = vm(FakeSettingsRepository())
        main.runCurrent()

        viewModel.onSpeedLimit(9f)
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
}
