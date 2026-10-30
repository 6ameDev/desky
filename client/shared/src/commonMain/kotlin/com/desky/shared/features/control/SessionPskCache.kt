package com.desky.shared.features.control

import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.pairing.GetPairPskUseCase
import kotlinx.coroutines.CancellationException

/**
 * Per-session PSK cache plus signed-frame counter allocation, shared by the
 * connection, drive, and expressions owners. Not thread-safe itself: every
 * owner serializes access under its own session mutex.
 */
class SessionPskCache(
    private val getPairPsk: GetPairPskUseCase,
    private val toRobotCounter: ToRobotCounter,
) {
    private var sessionPsk: ByteArray? = null
    private var pskMissingFor: String? = null

    /** Vault PSK cached per device; null when keyless or undecodable (latched). */
    suspend fun pskFor(deviceId: String): ByteArray? {
        var psk = sessionPsk
        if (psk == null) {
            if (pskMissingFor == deviceId) return null
            val hex = getPairPsk() ?: run {
                pskMissingFor = deviceId
                return null
            }
            val decoded = try {
                hexToBytes(hex)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                null
            }
            if (decoded == null || decoded.size != PAIR_PSK_BYTE_LENGTH) {
                pskMissingFor = deviceId
                return null
            }
            psk = decoded
            sessionPsk = psk
        }
        return psk
    }

    /**
     * Key plus next monotonic counter for [dev]; null when keyless or the
     * sequence is exhausted. Skipped allocations cost nothing.
     */
    suspend fun allocate(dev: PairedDevice): Pair<ByteArray, UInt>? {
        val psk = pskFor(dev.id) ?: return null
        toRobotCounter.ensure(dev.id, dev.lastCounter, dev.persistEvery, dev.era)
        return psk to (toRobotCounter.next() ?: return null)
    }

    /** Drop the cached key (unpair, era rotation); next allocate reloads. */
    fun invalidate() {
        sessionPsk = null
        pskMissingFor = null
    }
}

private const val PAIR_PSK_BYTE_LENGTH = 16
