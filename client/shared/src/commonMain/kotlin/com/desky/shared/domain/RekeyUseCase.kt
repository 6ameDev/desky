package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout

/** Default per-send wait schedule for the silent rekey exchange (ms): 3x 1s. */
val DEFAULT_REKEY_ATTEMPT_TIMEOUTS_MS: List<Long> = listOf(1000, 1000, 1000)

/**
 * Tier-1 silent auto-rekey: rotate the bond era + PSK under the old PSK,
 * reset the counter to the bond baseline, and resume gapless — with no user
 * action (see `shared/protocol/README.md` §"Tier-1 silent auto-rekey").
 * Mirrors TLS `KeyUpdate` / IPsec rekey: the app derives
 * `PSK_next = HKDF(oldPsk, "rekey" || era+1)` ([rekeyPsk]), sends
 * `REKEY_REQ` (`[0x53 | eraBE2 | counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC(oldPsk, "REKEY" || era || counter))`), waits for
 * `REKEY_ACK` (`[0x54 | eraBE2 | tag6 | XOR]`,
 * `tag = first6(HMAC(newPsk, "REKEYACK" || era))`), then persists the vault
 * PSK + Room era/counter and resets the shared [DriveCounter] to the new
 * era — so the next drive uses counter 1 under the new key, well before
 * [DriveCounter.EXHAUSTION_GATE].
 *
 * Silent: no UI surface on success (callers log at debug only); on failure
 * the caller falls back to the Tier-2 repair copy ("session expired —
 * please re-pair") — the holder and Room are untouched, so drive keeps
 * using the old key until the gate.
 *
 * Retry policy: at most [attemptTimeoutsMs.size] `REKEY_REQ` datagrams leave
 * the app per [invoke] (default 3). Retries reuse the SAME counter (the
 * first allocation) and the same bytes — byte-identical like `PAIR_REQ` —
 * so a lost ACK re-triggers the robot's idempotent re-ACK (duplicate
 * `era == current` path) instead of consuming fresh counters. ANY
 * well-formed `REKEY_ACK` for the requested era terminates the loop
 * immediately; silence retries, a tag mismatch fails fast (never retry a
 * forgery).
 *
 * @param attemptTimeoutsMs per-send wait schedule in ms; sends == size.
 *   Must hold 1..5 positive entries.
 */
class RekeyUseCase(
    private val deviceRepo: DeviceRepository,
    private val secretStore: BondSecretStore,
    private val udp: UdpSocketManager,
    private val getBondPsk: GetBondPskUseCase,
    private val driveCounter: DriveCounter,
    private val attemptTimeoutsMs: List<Long> = DEFAULT_REKEY_ATTEMPT_TIMEOUTS_MS,
) {
    init {
        require(attemptTimeoutsMs.size in 1..5) {
            "attemptTimeoutsMs must hold 1..5 entries (got ${attemptTimeoutsMs.size})"
        }
        require(attemptTimeoutsMs.all { it > 0 }) {
            "attemptTimeoutsMs entries must all be > 0"
        }
    }

    /**
     * Attempt one silent rotation. Returns the new era on success; on any
     * failure ([Outcome.Failure]) nothing was persisted (vault, Room, and
     * holder keep the old era — safe to keep driving until the gate, then
     * surface the Tier-2 repair copy).
     */
    suspend operator fun invoke(): Outcome<UShort> {
        return try {
            val device = deviceRepo.observePaired().first()
                ?: return Outcome.Failure("Rekey failed: no paired device")
            val pskHex = getBondPsk() ?: return Outcome.Failure("Rekey failed: no bond key")
            val oldPsk = try {
                hexToBytes(pskHex)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                return Outcome.Failure("Rekey failed: undecodable bond key", e)
            }
            if (oldPsk.size != 16) return Outcome.Failure("Rekey failed: undecodable bond key")
            driveCounter.ensure(device.id, device.lastCounter, device.persistEvery, device.era)
            val counter = driveCounter.next() ?: return Outcome.Failure("Rekey failed: counter exhausted")
            val nextEra: UShort = ((device.era.toInt() + 1) and 0xFFFF).toUShort()
            val nextPsk = rekeyPsk(oldPsk, nextEra)
            val req = encodeRekeyReq(oldPsk, nextEra, counter)

            var ack: UdpDatagram? = null
            for (i in attemptTimeoutsMs.indices) {
                udp.send(req, device.ip, device.port)
                ack = awaitRekeyAck(nextEra, attemptTimeoutsMs[i])
                if (ack != null) break
            }
            val reply = ack ?: return Outcome.Failure("Rekey failed: no response from robot")
            val decoded = decodeRekeyAck(reply.bytes)
                ?: return Outcome.Failure("Rekey failed: invalid response from robot")
            val (era, tag) = decoded
            if (era != nextEra) return Outcome.Failure("Rekey failed: invalid response from robot")
            val expected = rekeyAckTag(nextPsk, nextEra)
            if (!tag.contentEquals(expected)) {
                return Outcome.Failure("Rekey failed: invalid response from robot")
            }
            // Commit: vault first (same order as pair — a Room throw after
            // leaves the key resting encrypted for the next attempt), then
            // Room era/baseline, then the in-memory holder. The VM's session
            // PSK cache refresh is the caller's (see maybeRekey TODO).
            secretStore.savePsk(device.id, nextPsk.toHex())
            deviceRepo.updateRekey(device.id, 0u, nextEra)
            driveCounter.completeRekey(nextEra)
            Outcome.Success(nextEra)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Rekey failed: ${e.message}", e)
        }
    }

    /** First incoming datagram shaped like a `REKEY_ACK` for [era], or null on timeout. */
    private suspend fun awaitRekeyAck(era: UShort, timeoutMs: Long): UdpDatagram? {
        return try {
            withTimeout(timeoutMs) {
                udp.incoming.first { isRekeyAckFor(it.bytes, era) }
            }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }

    private fun isRekeyAckFor(bytes: ByteArray, era: UShort): Boolean {
        if (bytes.size != REKEY_ACK_SIZE) return false
        if ((bytes[0].toInt() and 0xFF) != REKEY_ACK_HEADER) return false
        val got = (((bytes[1].toInt() and 0xFF) shl 8) or (bytes[2].toInt() and 0xFF)).toUShort()
        return got == era
    }
}
