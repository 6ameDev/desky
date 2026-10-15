package com.desky.shared.domain

import com.desky.shared.presentation.FakeBondSecretStore
import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull

private const val PSK_HEX = "00112233445566778899aabbccddeeff"
private const val DEVICE_ID = "desky-192.168.4.1"

private fun bondedDevice() = PairedDevice(
    id = DEVICE_ID,
    ip = "192.168.4.1",
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
    lastCounter = 0u,
    alias = "192.168.4.1",
)

class GetBondPskUseCaseTest {

    @Test
    fun vaultRoundTrip() = runTest {
        val vault = FakeBondSecretStore()

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
        val repo = FakeDeviceRepository(bondedDevice())
        val vault = FakeBondSecretStore().apply { seed(DEVICE_ID, PSK_HEX) }

        assertEquals(PSK_HEX, GetBondPskUseCase(vault, repo)())
        assertEquals(PSK_HEX, vault.loadPsk(DEVICE_ID))
    }

    @Test
    fun nullWhenUnpaired() = runTest {
        assertNull(GetBondPskUseCase(FakeBondSecretStore(), FakeDeviceRepository(null))())
    }

    @Test
    fun nullWhenPairedWithoutKey() = runTest {
        val repo = FakeDeviceRepository(bondedDevice())

        assertNull(GetBondPskUseCase(FakeBondSecretStore(), repo)())
    }

    @Test
    fun forgetClearsVaultAndRoom() = runTest {
        val repo = FakeDeviceRepository(bondedDevice())
        val vault = FakeBondSecretStore().apply { seed(DEVICE_ID, PSK_HEX) }

        val result = ForgetDeviceUseCase(repo, vault)()

        assertIs<Outcome.Success<Unit>>(result)
        assertNull(vault.loadPsk(DEVICE_ID))
        assertEquals(1, vault.clearCalls)
        assertEquals(1, repo.clearCalls)
    }

    @Test
    fun forgetWhenUnpairedStillSucceeds() = runTest {
        val vault = FakeBondSecretStore()

        val result = ForgetDeviceUseCase(FakeDeviceRepository(null), vault)()

        assertIs<Outcome.Success<Unit>>(result)
        assertEquals(0, vault.clearCalls)
    }
}
