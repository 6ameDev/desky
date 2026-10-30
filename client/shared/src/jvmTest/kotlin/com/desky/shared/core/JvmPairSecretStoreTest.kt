package com.desky.shared.core

import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

/**
 * Round-trip against the REAL JVM backend ([createPairSecretStore]):
 * save -> load -> clear -> missing reads null (and clearing an absent
 * entry is a no-op). Entries are isolated per device id.
 */
class JvmPairSecretStoreTest {

    @Test
    fun roundTrip() = runTest {
        val store = createPairSecretStore()

        assertNull(store.loadPsk("d1"))
        store.savePsk("d1", "00112233445566778899aabbccddeeff")
        assertEquals("00112233445566778899aabbccddeeff", store.loadPsk("d1"))
        assertNull(store.loadPsk("d2"))

        store.savePsk("d1", "ffeeddccbbaa99887766554433221100")
        assertEquals("ffeeddccbbaa99887766554433221100", store.loadPsk("d1"))

        store.clear("d1")
        assertNull(store.loadPsk("d1"))
        store.clear("d1")
    }
}
