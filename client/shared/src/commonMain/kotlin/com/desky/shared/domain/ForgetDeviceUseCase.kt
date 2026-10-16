package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.first

/**
 * Drop the current pairing: tell the robot (best-effort signed `UNBOND`),
 * then clear the Room row AND the vault entry.
 *
 * Symmetric forget: when a paired device with a vault PSK exists, the app
 * sends `UNBOND` (`[0xAD | counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC-SHA256(psk16, "UNBOND" || counterBE4))`,
 * `counter` allocated from the shared [DriveCounter] — the live in-memory
 * sequence, never Room's stale persisted watermark) up to 3x
 * fire-and-forget at the stored device endpoint — the firmware never
 * replies, so there is nothing to wait for. Every send error is swallowed
 * (graceful degradation: the local forget below proceeds regardless, so the
 * app can never strand itself paired); a missing PSK — or an exhausted
 * counter (null allocation) — skips the network send entirely and forgets
 * locally exactly as before.
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
    private val udp: UdpSocketManager,
    private val getBondPsk: GetBondPskUseCase,
    private val driveCounter: DriveCounter,
) {
    suspend operator fun invoke(): Outcome<Unit> {
        return try {
            val current = deviceRepo.observePaired().first()
            if (current != null) {
                sendUnbondBestEffort(current)
            }
            deviceRepo.clear()
            if (current != null) secretStore.clear(current.id)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to forget paired device: ${e.message}", e)
        }
    }

    /**
     * Best-effort signed `UNBOND`: null PSK (or an undecodable vault entry),
     * or a null counter allocation, skips the send; every transmit error is
     * swallowed so the local forget in [invoke] always runs. Cancellation
     * is never swallowed.
     *
     * The counter comes from the shared [DriveCounter] (live sequence),
     * never `device.lastCounter` (the stale Room watermark): after any
     * unpersisted drive frames the watermark lags the robot, and the robot
     * drops the `UNBOND` as a replay. A best-effort [DriveCounter.flush]
     * runs first so the watermark is fresh even when all sends fail (flush
     * errors swallowed — graceful degradation preserved).
     */
    private suspend fun sendUnbondBestEffort(device: PairedDevice) {
        val pskHex = getBondPsk() ?: return
        try {
            driveCounter.ensure(device.id, device.lastCounter, device.persistEvery)
            try {
                driveCounter.flush()
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Swallowed: forget proceeds regardless.
            }
            val counter = driveCounter.next() ?: return
            val unbond = encodeUnbond(hexToBytes(pskHex), counter)
            repeat(MAX_UNBOND_SENDS) {
                try {
                    udp.send(unbond, device.ip, device.port)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // UDP loss / dead socket: the robot keeps a stale bond
                    // until its own expiry, but the app must forget locally.
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Malformed vault hex and friends: forget locally regardless.
        }
    }

    companion object {
        /** Fire-and-forget `UNBOND` transmissions (no reply ever comes back). */
        const val MAX_UNBOND_SENDS = 3
    }
}
