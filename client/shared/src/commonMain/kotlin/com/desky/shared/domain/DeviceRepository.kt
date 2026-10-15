package com.desky.shared.domain

import kotlinx.coroutines.flow.Flow

/** Persistence boundary for the currently paired robot (at most one). */
interface DeviceRepository {
    /** Currently paired device, or null when nothing is paired. */
    fun observePaired(): Flow<PairedDevice?>

    /** Persist the paired device, replacing any previous pairing. */
    suspend fun save(device: PairedDevice)

    /** Drop the pairing. */
    suspend fun clear()

    /**
     * Blank the legacy Room `pskHex` column for [deviceId] after its key
     * has been moved to the [BondSecretStore] vault. Migration-only
     * ([GetBondPskUseCase]); new pairings are written with a null column.
     */
    suspend fun clearPskHex(deviceId: String)
}
