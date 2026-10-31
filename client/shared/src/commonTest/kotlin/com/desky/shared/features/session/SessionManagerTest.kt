package com.desky.shared.features.session

import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val PSK_HEX = "00112233445566778899aabbccddeeff"

private fun pairedDevice(lastCounter: UInt = 0u) = PairedDevice(
    id = "desky-$ROBOT_HOST",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "token",
    lastCounter = lastCounter,
    era = 0u,
)

@OptIn(ExperimentalCoroutinesApi::class)
class SessionManagerTest {

    private data class Setup(
        val repo: FakeDeviceRepository,
        val vault: FakePairSecretStore,
        val sessions: SessionManager,
    )

    private fun CoroutineScope.setup(
        device: PairedDevice?,
        seedKey: Boolean,
    ): Setup {
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore()
        if (seedKey && device != null) vault.seed(device.id, PSK_HEX)
        val udp = FakeUdpSocketManager()
        val toRobot = ToRobotCounter(repo)
        val fromRobot = FromRobotCounter(repo)
        val getPsk = GetPairPskUseCase(vault, repo)
        val forget = ForgetDeviceUseCase(repo, vault, udp, getPsk, toRobot, fromRobot)
        val sessions = SessionManager(repo, getPsk, toRobot, forget, this)
        return Setup(repo, vault, sessions)
    }

    @Test
    fun missingKeyMarksInvalidAndForgetsOnce() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice()
        val s = scope.setup(device, seedKey = false)
        try {
            runCurrent()
            assertFalse(s.sessions.invalid.value)

            assertNull(s.sessions.allocate(device))
            runCurrent()
            runCurrent()
            assertTrue(s.sessions.invalid.value)
            assertNull(s.repo.observePaired().first())
            assertEquals(1, s.repo.clearCalls)

            assertNull(s.sessions.allocate(device))
            runCurrent()
            assertEquals(1, s.repo.clearCalls, "forget fires once per device")
        } finally {
            s.sessions.stop()
        }
    }

    @Test
    fun exhaustedCounterMarksInvalidAndForgetsOnce() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice(lastCounter = ToRobotCounter.EXHAUSTION_GATE - 1u)
        val s = scope.setup(device, seedKey = true)
        try {
            runCurrent()
            assertNull(s.sessions.allocate(device))
            runCurrent()
            runCurrent()
            assertTrue(s.sessions.invalid.value)
            assertEquals(1, s.repo.clearCalls)

            assertNull(s.sessions.allocate(device))
            runCurrent()
            assertEquals(1, s.repo.clearCalls, "exhausted forget fires once")
        } finally {
            s.sessions.stop()
        }
    }

    @Test
    fun validAllocateClearsInvalid() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice()
        val s = scope.setup(device, seedKey = true)
        try {
            runCurrent()
            val prepared = s.sessions.allocate(device)
            assertTrue(prepared != null)
            assertFalse(s.sessions.invalid.value)
            assertEquals(0, s.repo.clearCalls)
        } finally {
            s.sessions.stop()
        }
    }

    @Test
    fun unpairRepairUsesNewKey() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice()
        val s = scope.setup(device, seedKey = true)
        try {
            runCurrent()
            val first = s.sessions.allocate(device)
            assertTrue(first != null)

            s.repo.emit(null)
            runCurrent()
            s.repo.emit(device)
            runCurrent()
            s.vault.seed(device.id, "ffeeddccbbaa99887766554433221100")
            val second = s.sessions.allocate(device)
            assertTrue(second != null)
            assertTrue(!first.first.contentEquals(second.first), "re-pair reloads the new vault key")
            assertFalse(s.sessions.invalid.value)
        } finally {
            s.sessions.stop()
        }
    }

    @Test
    fun refreshInvalidateUsesNewKey() = runTest {
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler))
        val device = pairedDevice()
        val s = scope.setup(device, seedKey = true)
        try {
            runCurrent()
            val first = s.sessions.allocate(device)
            assertTrue(first != null)

            s.vault.seed(device.id, "ffeeddccbbaa99887766554433221100")
            s.sessions.invalidate()
            val second = s.sessions.allocate(device)
            assertTrue(second != null)
            assertTrue(!first.first.contentEquals(second.first), "post-refresh allocate uses the rotated key")
        } finally {
            s.sessions.stop()
        }
    }
}
