package com.desky.shared.features.session

import app.cash.turbine.test
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.runVmTest
import com.desky.shared.presentation.testDevice
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class SessionGateViewModelTest {

    @Test
    fun coldNullFirstEmissionStaysFalse() = runVmTest { main ->
        // Cold-start null must NOT navigate: startDestination=PAIRING already
        // shows pairing.
        val repo = FakeDeviceRepository(null)
        val viewModel = SessionGateViewModel(repo)
        main.runCurrent()

        assertFalse(viewModel.provenDead.value)
    }

    @Test
    fun coldPairedFirstEmissionStaysFalse() = runVmTest { main ->
        // Cold-start non-null likewise stays false (already paired).
        val repo = FakeDeviceRepository(testDevice())
        val viewModel = SessionGateViewModel(repo)
        main.runCurrent()

        assertFalse(viewModel.provenDead.value)
    }

    @Test
    fun clearProvesDeadThenRepairClears() = runVmTest { main ->
        val repo = FakeDeviceRepository(null)
        val viewModel = SessionGateViewModel(repo)
        main.runCurrent()
        assertFalse(viewModel.provenDead.value)

        // Emit device -> still false (paired).
        repo.save(testDevice())
        main.runCurrent()
        assertFalse(viewModel.provenDead.value)

        // Clear (ForgetDeviceUseCase path) -> proven dead, force to Pairing.
        repo.clear()
        main.runCurrent()
        assertTrue(viewModel.provenDead.value)

        // Re-pair -> fence releases.
        repo.save(testDevice())
        main.runCurrent()
        assertFalse(viewModel.provenDead.value)
    }

    @Test
    fun rapidFlapEndsDeadDistinct() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice())
        val viewModel = SessionGateViewModel(repo)

        viewModel.provenDead.test {
            // Initial StateFlow value; the arming emission keeps it false,
            // so no second false item arrives.
            assertFalse(awaitItem())
            main.runCurrent()
            expectNoEvents()

            // Flap: null/device/null.
            repo.emit(null)
            main.runCurrent()
            assertTrue(awaitItem())

            // Duplicate null is swallowed by distinctUntilChanged upstream
            // (plus StateFlow dedup): no second true.
            repo.emit(null)
            main.runCurrent()
            expectNoEvents()

            repo.emit(testDevice())
            main.runCurrent()
            assertFalse(awaitItem())

            repo.emit(null)
            main.runCurrent()
            assertTrue(awaitItem())

            // Final state + emission count: exactly [false, true, false, true]
            // — no duplicate true flaps.
            assertTrue(viewModel.provenDead.value)
            cancelAndIgnoreRemainingEvents()
        }
        // Count check outside turbine via a fresh collector is redundant;
        // the awaitItem sequence above already pins the distinct count.
        assertEquals(true, viewModel.provenDead.value)
    }
}
