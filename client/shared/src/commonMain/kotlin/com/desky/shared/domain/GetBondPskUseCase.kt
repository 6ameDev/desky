package com.desky.shared.domain

import kotlinx.coroutines.flow.first

/**
 * Session-load AND silent self-migration for the bond PSK (pairing/auth
 * Task 8): the single API Task 9's send loop calls once at startup.
 *
 * Resolution order for the currently paired device (null when unpaired):
 * 1. Vault hit: [BondSecretStore.loadPsk] returns the key — pure read, no
 *    Room write, so repeated calls are idempotent and cheap.
 * 2. Legacy Room row: when the vault is empty but the `paired_device` row
 *    still carries `pskHex` (rows written before Task 8), the key is moved —
 *    saved to the vault, then the Room column is blanked via
 *    [DeviceRepository.clearPskHex] — and returned. The second call is a
 *    pure vault hit (step 1), so migration runs at most once per device
 *    with no re-pair and no user action.
 * 3. Otherwise null (paired but keyless, e.g. a pre-v1 row).
 *
 * The vault is at-rest storage only: call this once at startup and cache
 * the result in memory — never read the vault per tick.
 */
class GetBondPskUseCase(
    private val secretStore: BondSecretStore,
    private val deviceRepo: DeviceRepository,
) {
    suspend operator fun invoke(): String? {
        val device = deviceRepo.observePaired().first() ?: return null
        secretStore.loadPsk(device.id)?.let { return it }
        val legacy = device.pskHex ?: return null
        secretStore.savePsk(device.id, legacy)
        deviceRepo.clearPskHex(device.id)
        return legacy
    }
}
