package com.desky.shared.data

import com.desky.shared.domain.PairSecretStore
import java.util.concurrent.ConcurrentHashMap

/**
 * JVM [PairSecretStore]: process-local map. Desktop/test only — there is no
 * OS credential store equivalent wired here, so secrets do not survive
 * restart and are NOT encrypted at rest. Never use as the production vault.
 */
class InMemoryPairSecretStore : PairSecretStore {
    private val vault = ConcurrentHashMap<String, String>()

    override suspend fun savePsk(deviceId: String, pskHex: String) {
        vault[deviceId] = pskHex
    }

    override suspend fun loadPsk(deviceId: String): String? = vault[deviceId]

    override suspend fun clear(deviceId: String) {
        vault.remove(deviceId)
    }
}

/** JVM factory: no dependencies needed. */
fun createPairSecretStore(): PairSecretStore = InMemoryPairSecretStore()
