package com.desky.shared.domain

import kotlinx.coroutines.flow.first

/**
 * Drop the current pairing: the Room row AND the vault entry.
 *
 * The Room row is deleted first: if it throws, the vault entry is left
 * untouched (still referenced by the surviving row — no bond loss, retry is
 * safe). If the vault clear throws afterwards, the failure surfaces but the
 * row is already gone, so at worst a stale vault entry lingers until the
 * next pairing overwrites it.
 */
class ForgetDeviceUseCase(
    private val deviceRepo: DeviceRepository,
    private val secretStore: BondSecretStore,
) {
    suspend operator fun invoke(): Outcome<Unit> {
        return try {
            val current = deviceRepo.observePaired().first()
            deviceRepo.clear()
            if (current != null) secretStore.clear(current.id)
            Outcome.Success(Unit)
        } catch (e: Exception) {
            Outcome.Failure("Failed to forget paired device: ${e.message}", e)
        }
    }
}
