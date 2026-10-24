package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout

/** Default wait for the `EXPR_LIST_RESP` answer to one `EXPR_LIST_REQ` (ms): single attempt, no retry. */
const val EXPR_LIST_TIMEOUT_MS = 1000L

/**
 * Expression catalog fetch: send one `EXPR_LIST_REQ` (`[0x59 | eraBE2 |
 * counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC-SHA256(psk, "EXPRLIST" || era || counter))`) to the
 * paired robot's address, then await the first well-formed `EXPR_LIST_RESP`
 * (`[0x5A | ver | count | (id, flags, name[10]) x count | XOR]`, ver 1,
 * count 0..12) within [timeoutMs].
 *
 * Stateless request/response: the caller (the control ViewModel) owns the
 * counter exactly like the drive path — allocated from the shared
 * [ToRobotCounter] via its session helper, so the request consumes the
 * monotonic sequence like every other signed frame — and owns the cache
 * (per caller lifecycle). Single send per call, no retry: a lost request
 * or a lost reply heals on the next screen entry. The reply carries no
 * era/counter/tag bytes (shape-only verify in [ExprListRespCodec]), so
 * nothing is consumed into [FromRobotCounter] here. Cancellation is never
 * swallowed. Key material is never logged (lengths and booleans only).
 */
class FetchExpressionsUseCase(
    private val udp: UdpSocketManager,
    private val timeoutMs: Long = EXPR_LIST_TIMEOUT_MS,
) {
    init {
        require(timeoutMs > 0) { "timeoutMs must be > 0 (got $timeoutMs)" }
    }

    suspend operator fun invoke(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
    ): Outcome<List<ExpressionEntry>> {
        return try {
            udp.send(encodeExprListReq(psk, era, counter), device.ip, device.port)
            val entries = awaitExprListResp() ?: return Outcome.Failure(
                "Expression list request timed out, reopen the screen to retry",
            )
            Outcome.Success(entries)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to fetch expression list: ${e.message}", e)
        }
    }

    /** First well-formed `EXPR_LIST_RESP` on the shared inbound flow, or null on timeout. */
    private suspend fun awaitExprListResp(): List<ExpressionEntry>? {
        return try {
            withTimeout(timeoutMs) {
                udp.incoming.first { ExprListRespCodec.decode(it.bytes) != null }
                    .let { ExprListRespCodec.decode(it.bytes) }
            }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }
}

/**
 * Expression play request: send one `EXPR_TRIGGER` (`[0x5B | id | eraBE2 |
 * counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC-SHA256(psk, "EXPRTRIG" || id || era || counter))`) to
 * the paired robot's address. Fire-and-forget: the robot gates on MANUAL
 * and replies nothing — a robot-side drop is silent by protocol.
 *
 * Unknown ids are rejected client-side against [knownIds] (the fetched
 * catalog) before anything is encoded or sent, so a stale picker tap can
 * never address a catalog entry the robot never advertised. The caller
 * owns the counter exactly like the drive path (one allocation per tap).
 * Transport errors map to [Outcome.Failure] (the caller surfaces them — a
 * tap was an explicit user action). Cancellation is never swallowed. Key
 * material is never logged (lengths and booleans only).
 */
class TriggerExpressionUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
        id: Int,
        knownIds: Set<Int>,
    ): Outcome<Unit> {
        if (id !in knownIds) {
            return Outcome.Failure("Unknown expression id: $id")
        }
        return try {
            udp.send(encodeExprTrigger(psk, id, era, counter), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to trigger expression: ${e.message}", e)
        }
    }
}
