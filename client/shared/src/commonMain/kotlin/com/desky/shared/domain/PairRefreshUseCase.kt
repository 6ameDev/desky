package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout

/** Default per-send wait schedule for the silent pair-refresh exchange (ms): 3x 1s. */
val DEFAULT_PAIR_REFRESH_ATTEMPT_TIMEOUTS_MS: List<Long> = listOf(1000, 1000, 1000)

/**
 * Tier-1 silent auto-pair-refresh: rotate the pair era + PSK under the old PSK,
 * reset the counter to the pair baseline, and resume gapless — with no user
 * action (see `shared/protocol/README.md` §"Tier-1 silent auto-pair-refresh").
 * Mirrors TLS `KeyUpdate` / IPsec pair-refresh: the app derives
 * `PSK_next = HKDF(oldPsk, "pair-refresh" || era+1)` ([pairRefreshPsk]), sends
 * `PAIR_REFRESH_REQ` (`[0x53 | eraBE2 | counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC(oldPsk, "PAIRREFRESH" || era || counter))`), waits for
 * `PAIR_REFRESH_ACK` (`[0x54 | eraBE2 | tag6 | XOR]`,
 * `tag = first6(HMAC(newPsk, "PAIRREFRESHACK" || era))`), then persists the vault
 * PSK + Room era/counter and resets the shared [DriveCounter] to the new
 * era — so the next drive uses counter 1 under the new key, well before
 * [DriveCounter.EXHAUSTION_GATE].
 *
 * Silent: no UI surface on success (callers log at debug only); on failure
 * the caller falls back to the Tier-2 repair copy ("session expired —
 * please re-pair") — the holder and Room are untouched, so drive keeps
 * using the old key until the gate.
 *
 * Retry policy: at most [attemptTimeoutsMs.size] `PAIR_REFRESH_REQ` datagrams leave
 * the app per [invoke] (default 3). Retries reuse the SAME counter (the
 * first allocation) and the same bytes — byte-identical like `PAIR_REQ` —
 * so a lost ACK re-triggers the robot's idempotent re-ACK (duplicate
 * `era == current` path) instead of consuming fresh counters. ANY
 * well-formed `PAIR_REFRESH_ACK` for the requested era terminates the loop
 * immediately; silence retries, a tag mismatch fails fast (never retry a
 * forgery).
 *
 * @param attemptTimeoutsMs per-send wait schedule in ms; sends == size.
 *   Must hold 1..5 positive entries.
 */
class PairRefreshUseCase(
    private val deviceRepo: DeviceRepository,
    private val secretStore: PairSecretStore,
    private val udp: UdpSocketManager,
    private val getPairPsk: GetPairPskUseCase,
    private val driveCounter: DriveCounter,
    private val attemptTimeoutsMs: List<Long> = DEFAULT_PAIR_REFRESH_ATTEMPT_TIMEOUTS_MS,
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
                ?: return Outcome.Failure("PairRefresh failed: no paired device")
            val pskHex = getPairPsk() ?: return Outcome.Failure("PairRefresh failed: no pair key")
            val oldPsk = try {
                hexToBytes(pskHex)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                return Outcome.Failure("PairRefresh failed: undecodable pair key", e)
            }
            if (oldPsk.size != 16) return Outcome.Failure("PairRefresh failed: undecodable pair key")
            driveCounter.ensure(device.id, device.lastCounter, device.persistEvery, device.era)
            val counter = driveCounter.next() ?: return Outcome.Failure("PairRefresh failed: counter exhausted")
            val nextEra: UShort = ((device.era.toInt() + 1) and 0xFFFF).toUShort()
            val nextPsk = pairRefreshPsk(oldPsk, nextEra)
            val req = encodePairRefreshReq(oldPsk, nextEra, counter)

            var ack: UdpDatagram? = null
            for (i in attemptTimeoutsMs.indices) {
                udp.send(req, device.ip, device.port)
                ack = awaitPairRefreshAck(nextEra, attemptTimeoutsMs[i])
                if (ack != null) break
            }
            val reply = ack ?: return Outcome.Failure("PairRefresh failed: no response from robot")
            val decoded = decodePairRefreshAck(reply.bytes)
                ?: return Outcome.Failure("PairRefresh failed: invalid response from robot")
            val (era, tag) = decoded
            if (era != nextEra) return Outcome.Failure("PairRefresh failed: invalid response from robot")
            val expected = pairRefreshAckTag(nextPsk, nextEra)
            if (!tag.contentEquals(expected)) {
                return Outcome.Failure("PairRefresh failed: invalid response from robot")
            }
            // Commit: vault first (same order as pair — a Room throw after
            // leaves the key resting encrypted for the next attempt), then
            // Room era/baseline, then the in-memory holder. The VM's session
            // PSK cache refresh is the caller's (see maybePairRefresh TODO).
            secretStore.savePsk(device.id, nextPsk.toHex())
            deviceRepo.updatePairRefresh(device.id, 0u, nextEra)
            driveCounter.completePairRefresh(nextEra)
            Outcome.Success(nextEra)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("PairRefresh failed: ${e.message}", e)
        }
    }

    /** First incoming datagram shaped like a `PAIR_REFRESH_ACK` for [era], or null on timeout. */
    private suspend fun awaitPairRefreshAck(era: UShort, timeoutMs: Long): UdpDatagram? {
        return try {
            withTimeout(timeoutMs) {
                udp.incoming.first { isPairRefreshAckFor(it.bytes, era) }
            }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }

    private fun isPairRefreshAckFor(bytes: ByteArray, era: UShort): Boolean {
        if (bytes.size != PAIR_REFRESH_ACK_SIZE) return false
        if ((bytes[0].toInt() and 0xFF) != PAIR_REFRESH_ACK_HEADER) return false
        val got = (((bytes[1].toInt() and 0xFF) shl 8) or (bytes[2].toInt() and 0xFF)).toUShort()
        return got == era
    }
}
