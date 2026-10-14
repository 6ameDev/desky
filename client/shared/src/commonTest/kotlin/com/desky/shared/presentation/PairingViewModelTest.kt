package com.desky.shared.presentation

import app.cash.turbine.test
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.every
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.flowOf
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class PairingViewModelTest {

    private fun vm(repo: DeviceRepository) = PairingViewModel(
        PairDeviceWithCodeUseCase(repo),
        ForgetDeviceUseCase(repo),
        repo,
    )

    @Test
    fun textIntentsUpdateState() = runVmTest { main ->
        val viewModel = vm(FakeDeviceRepository())
        viewModel.state.test {
            assertEquals(PairingUiState(), awaitItem())
            viewModel.onIpChange("192.168.4.1")
            assertEquals("192.168.4.1", awaitItem().ip)
            viewModel.onPinChange("1234")
            assertEquals("1234", awaitItem().pin)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun pairSuccessStoresDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onIpChange("192.168.4.1")
        viewModel.onPinChange("1234")
        viewModel.onPair()
        assertTrue(viewModel.state.value.isPairing)
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.error)
        val device = assertNotNull(state.pairedDevice)
        assertEquals("192.168.4.1", device.ip)
        assertEquals(1, repo.saveCalls)
    }

    @Test
    fun pairFailureMapsReasonToError() = runVmTest { main ->
        val viewModel = vm(FakeDeviceRepository())
        main.runCurrent()

        viewModel.onIpChange("999.999.999.999")
        viewModel.onPinChange("1234")
        viewModel.onPair()
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        val error = assertNotNull(state.error)
        assertTrue(error.isNotBlank())
    }

    @Test
    fun pairGuardsReentryWhileInFlight() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onIpChange("192.168.4.1")
        viewModel.onPinChange("1234")
        // Both taps land before the scheduler runs the first pairing: the
        // synchronous isPairing flag makes the second a no-op.
        viewModel.onPair()
        viewModel.onPair()
        main.advanceUntilIdle()

        assertEquals(1, repo.saveCalls)
        assertFalse(viewModel.state.value.isPairing)
        assertNotNull(viewModel.state.value.pairedDevice)
    }

    @Test
    fun persistFailureSurfacesError() = runVmTest { main ->
        // Mokkery is genuinely simpler here: one throwing stub without a
        // dedicated fake subclass.
        val repo = mock<DeviceRepository>()
        every { repo.observePaired() } returns flowOf(null)
        everySuspend { repo.save(any()) } calls { throw IllegalStateException("disk full") }
        everySuspend { repo.clear() } calls { /* no-op */ }

        val viewModel = vm(repo)
        main.runCurrent()
        viewModel.onIpChange("192.168.4.1")
        viewModel.onPinChange("1234")
        viewModel.onPair()
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        assertTrue(state.error!!.contains("disk full"))
    }

    @Test
    fun unpairClearsDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice())
        val viewModel = vm(repo)
        main.runCurrent()
        assertNotNull(viewModel.state.value.pairedDevice)

        viewModel.onUnpair()
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertNull(state.pairedDevice)
        assertNull(state.error)
        assertEquals(1, repo.clearCalls)
    }

    @Test
    fun unpairFailureSurfacesError() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice()).apply {
            clearError = IllegalStateException("db locked")
        }
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onUnpair()
        main.advanceUntilIdle()

        assertNotNull(viewModel.state.value.pairedDevice)
        assertIs<String>(viewModel.state.value.error)
    }

    @Test
    fun repoEmissionUpdatesPairedDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val viewModel = vm(repo)
        main.runCurrent()
        assertNull(viewModel.state.value.pairedDevice)

        repo.emit(testDevice("10.0.0.9"))
        main.runCurrent()

        assertEquals("10.0.0.9", viewModel.state.value.pairedDevice?.ip)
    }
}
