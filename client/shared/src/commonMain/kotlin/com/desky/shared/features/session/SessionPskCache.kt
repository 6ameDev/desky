package com.desky.shared.features.session

import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.pairing.GetPairPskUseCase
import kotlinx.coroutines.CancellationException

/** Vault-backed key plus counter allocator. Not thread-safe: call only via [SessionManager]. */
class SessionPskCache(
    private val getPairPsk: GetPairPskUseCase,
    private val toRobotCounter: ToRobotCounter,
) {
    private var sessionPsk: ByteArray? = null
    private var pskMissingFor: String? = null

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

    suspend fun allocate(dev: PairedDevice): Pair<ByteArray, UInt>? {
        val psk = pskFor(dev.id) ?: return null
        toRobotCounter.ensure(dev.id, dev.lastCounter, dev.persistEvery, dev.era)
        return psk to (toRobotCounter.next() ?: return null)
    }

    fun invalidate() {
        sessionPsk = null
        pskMissingFor = null
    }
}

private const val PAIR_PSK_BYTE_LENGTH = 16
