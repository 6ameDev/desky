package com.desky.shared.features.session

import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.presentation.FakeDeviceRepository
import com.desky.shared.presentation.FakePairSecretStore
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull

class SessionPskCacheTest {

    private fun pairedDevice() = PairedDevice(
        id = "desky-$ROBOT_HOST",
        ip = ROBOT_HOST,
        authToken = "psk-v1",
        era = 0u,
    )

    private fun cache(
        vault: FakePairSecretStore,
        repo: FakeDeviceRepository,
    ) = SessionPskCache(
        getPairPsk = GetPairPskUseCase(vault, repo),
        toRobotCounter = ToRobotCounter(repo),
    )

    @Test
    fun allocateReturnsKeyAndFirstCounter() = runTest {
        val device = pairedDevice()
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(device.id, "00112233445566778899aabbccddeeff") }

        val prepared = cache(vault, repo).allocate(device)

        assertNotNull(prepared)
        assertEquals(16, prepared.first.size, "32 hex chars decode to a 16-byte key")
        assertEquals(1u, prepared.second)
    }

    @Test
    fun missingKeyLatchesUntilInvalidated() = runTest {
        val device = pairedDevice()
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore()
        val pskCache = cache(vault, repo)

        assertNull(pskCache.allocate(device), "keyless vault allocates nothing")

        vault.seed(device.id, "00112233445566778899aabbccddeeff")
        assertNull(pskCache.allocate(device), "missing latch holds after late seed")

        pskCache.invalidate()
        val prepared = pskCache.allocate(device)
        assertNotNull(prepared)
        assertEquals(1u, prepared.second, "reload allocates from the start")
    }

    @Test
    fun undecodableKeyAllocatesNothing() = runTest {
        val device = pairedDevice()
        val repo = FakeDeviceRepository(device)
        val vault = FakePairSecretStore().apply { seed(device.id, "not-hex!!") }

        assertNull(cache(vault, repo).allocate(device))
    }
}
