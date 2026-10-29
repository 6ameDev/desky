package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.mapNotNull

private const val BYTE_MASK = 0xFF
private const val PSK_BYTE_LENGTH = 16

/**
 * Core-originated `DISCONNECT` eviction observer (physical pair-button
 * press: the robot sends exactly one authenticated `DISCONNECT`
 * (`[0x56 | eraBE2 | counterBE4 | tag6 | XOR]`, counter from the robot's
 * OWN send sequence starting at 1 per pair, fire-and-forget, no retry)
 * before clearing its own pair and opening a window — see
 * `shared/protocol/README.md` §"ALIVE + DISCONNECT + robot alive rule").
 *
 * Dispatch rides the shared [UdpSocketManager.incoming] flow (no new
 * network stack): non-`0x56` datagrams (telemetry, pairing replies) pass
 * through untouched — only a 14-byte `0x56` frame enters verify.
 *
 * Verify order (mirrors the firmware receive funnel): len -> header ->
 * XOR ([decodeDisconnect]) -> paired (Room row + vault PSK present) ->
 * era equals the stored era -> counter wrap-aware strictly greater than
 * the RECEIVE sequence ([FromRobotCounter], same idiom as firmware
 * `isFreshCounter` — never the send sequence: the two 32-bit domains
 * advance independently, so a busy drive loop can no longer starve an
 * eviction stamp) -> tag
 * `first6(HMAC-SHA256(psk, "DISCONNECT" || era || counter))` in
 * constant time. Every `0x56` candidate logs one debug line at entry
 * (never key material — lengths and booleans only); any failure drops
 * silently with no state change. The unpaired case needs no extra branch:
 * with no Room row (hence no PSK) verify fails on its own down the same
 * path. Burst duplicates (same counter) drop as stale after the first —
 * idempotent, no special-casing.
 *
 * Rollout safety: the app ships first safely — an old core never sends
 * `0x56` robot -> app, so this verify path is inert until the firmware
 * half lands (no frame ever enters verify before then).
 *
 * On pass the accepted counter is consumed (persisted via
 * [FromRobotCounter.consume]), then the local pair is forgotten WITHOUT
 * any reply packet (the robot already cleared itself — an `UNPAIR` would
 * be a dead send, so this deliberately does NOT go through
 * [ForgetDeviceUseCase.invoke], which best-effort sends one): Room row
 * deleted first (a throw leaves the vault entry referenced by the
 * surviving row, same order as [ForgetDeviceUseCase]), then the vault
 * entry, then both counter holders reset (send [ToRobotCounter] era +
 * receive [FromRobotCounter] peer). The control ViewModel's existing
 * unpair branch (repo null -> pause the send loop, drop the session
 * cache, surface Unpaired) plus the global session gate (repo null ->
 * force to Pairing) own the UI convergence — this flow just emits [Unit]
 * per verified eviction for correlation logging.
 * Cancellation is never swallowed.
 */
class ObserveRemoteDisconnectUseCase(
    private val udp: UdpSocketManager,
    private val deviceRepo: DeviceRepository,
    private val getPairPsk: GetPairPskUseCase,
    private val secretStore: PairSecretStore,
    private val toRobotCounter: ToRobotCounter,
    private val fromRobotCounter: FromRobotCounter,
) {
    /** Hot stream of verified core-originated evictions (local forget runs per emission). */
    fun observe(): Flow<Unit> =
        udp.incoming.mapNotNull { datagram ->
            handle(datagram.bytes)
        }

    /**
     * Verify one datagram; [Unit] on a verified eviction (local forget
     * already ran), null on any drop. Internal for tests (flow-level tests
     * go through [observe]).
     */
    internal suspend fun handle(bytes: ByteArray): Unit? {
        if (bytes.size != DISCONNECT_SIZE || (bytes[0].toInt() and BYTE_MASK) != DISCONNECT_HEADER) {
            return null
        }
        // Entry log for every eviction-shaped frame (debug only, no key/tag bytes).
        println("RemoteDisconnect candidate len=${bytes.size}")
        val decoded = decodeDisconnect(bytes) ?: return null
        val (era, counter, tag) = decoded
        return try {
            val device = deviceRepo.observePaired().first() ?: return null
            val pskHex = getPairPsk() ?: return null
            val psk = try {
                hexToBytes(pskHex)
            } catch (_: Exception) {
                null
            }
            if (psk == null || psk.size != PSK_BYTE_LENGTH) return null
            if (era != device.era) return null
            // Freshness against the LIVE receive sequence, not the send
            // sequence and not the Room watermark: the watermark is the
            // last ACCEPTED robot counter (evictions are rare — persisted
            // on accept), so it only leads the live holder when the holder
            // has no peer yet (cold start, no eviction seen). Fall back to
            // the watermark only then.
            fromRobotCounter.ensure(device.id, device.lastRobotCounter)
            val lastSeen = fromRobotCounter.current() ?: device.lastRobotCounter
            if (!FromRobotCounter.isFresh(lastSeen, counter)) return null
            val expected = disconnectTag(psk, era, counter)
            if (!constantTimeEquals(tag, expected)) return null
            try {
                fromRobotCounter.consume(counter)
                deviceRepo.clear()
                secretStore.clear(device.id)
                toRobotCounter.reset()
                fromRobotCounter.reset()
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Commit-phase failure (verified frame, store error): log
                // only — the observer flow must stay alive, and the drive
                // loop's own missing-PSK/expiry convergence still fires.
                println("RemoteDisconnect clearFailed")
                return null
            }
            Unit
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
            null
        }
    }

    companion object {
        /**
         * Wrap-aware strictly-greater for the 32-bit counter. Unsigned
         * subtraction: wrap-safe across the 2^32 rollover; a diff of exactly
         * half the range is rejected as ambiguous. Exact mirror of firmware
         * `udpstatus::isFreshCounter` (canonical in [FromRobotCounter]).
         */
        fun isFreshCounter(lastSeen: UInt, got: UInt): Boolean =
            FromRobotCounter.isFresh(lastSeen, got)

        /** Length-sensitive constant-time equality (tag comparison only — never log inputs). */
        fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean {
            if (a.size != b.size) return false
            var diff = 0
            for (i in a.indices) {
                diff = diff or ((a[i].toInt() xor b[i].toInt()) and BYTE_MASK)
            }
            return diff == 0
        }
    }
}
