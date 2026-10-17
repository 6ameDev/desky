package com.desky.shared.domain

import kotlinx.coroutines.flow.first

/**
 * Session-load for the bond PSK (pairing/auth Task 9, vault-only).
 *
 * Returns the vault-stored key hex for the currently paired device, or null
 * when unpaired / keyless (paired but never bonded under pairing/auth v1 —
 * the caller must surface "please re-pair", never fall back to unsigned
 * frames). The Task 8 legacy Room migration branch is gone: Room no longer
 * has a `pskHex` column at all (v3 drops it).
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
        return secretStore.loadPsk(device.id)
    }
}
