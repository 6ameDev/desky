package com.desky.shared.domain

import com.desky.shared.presentation.FakePairSecretStore
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakeUdpSocketManager
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val DEVICE_ID = "desky-desky.local"

private fun pairedDevice() = PairedDevice(
    id = DEVICE_ID,
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = 0u,
    alias = ROBOT_HOST,
)

class GetPairPskUseCaseTest {

    @Test
    fun vaultRoundTrip() = runTest {
        val vault = FakePairSecretStore()

        assertNull(vault.loadPsk(DEVICE_ID))
        vault.savePsk(DEVICE_ID, PSK_HEX)
        assertEquals(PSK_HEX, vault.loadPsk(DEVICE_ID))
        vault.clear(DEVICE_ID)
        assertNull(vault.loadPsk(DEVICE_ID))
        // Clearing an absent entry is a no-op, not an error.
        vault.clear(DEVICE_ID)
    }

    @Test
    fun vaultHitReturnsKey() = runTest {
        val repo = FakeDeviceRepository(pairedDevice())
        val vault = FakePairSecretStore().apply { seed(DEVICE_ID, PSK_HEX) }

        assertEquals(PSK_HEX, GetPairPskUseCase(vault, repo)())
        assertEquals(PSK_HEX, vault.loadPsk(DEVICE_ID))
    }

    @Test
    fun nullWhenUnpaired() = runTest {
        assertNull(GetPairPskUseCase(FakePairSecretStore(), FakeDeviceRepository(null))())
    }

    @Test
    fun nullWhenPairedWithoutKey() = runTest {
        val repo = FakeDeviceRepository(pairedDevice())

        assertNull(GetPairPskUseCase(FakePairSecretStore(), repo)())
    }

    @Test
    fun forgetClearsVaultAndRoom() = runTest {
        val repo = FakeDeviceRepository(pairedDevice())
        val vault = FakePairSecretStore().apply { seed(DEVICE_ID, PSK_HEX) }
        val udp = FakeUdpSocketManager()

        val result = ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), ToRobotCounter(repo), FromRobotCounter(repo))()

        assertIs<Outcome.Success<Unit>>(result)
        assertNull(vault.loadPsk(DEVICE_ID))
        assertEquals(1, vault.clearCalls)
        assertEquals(1, repo.clearCalls)
    }

    @Test
    fun forgetWhenUnpairedStillSucceeds() = runTest {
        val vault = FakePairSecretStore()
        val repo = FakeDeviceRepository(null)
        val udp = FakeUdpSocketManager()

        val result = ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), ToRobotCounter(repo), FromRobotCounter(repo))()

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(0, vault.clearCalls)
    }
}
