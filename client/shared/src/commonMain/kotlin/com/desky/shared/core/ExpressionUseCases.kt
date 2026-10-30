package com.desky.shared.core

import kotlinx.coroutines.CancellationException

/**
 * App-side expression catalog: hardcoded mirror of the shared firmware
 * catalog (`embedded/shared/link/expression_catalog.h`, normative source).
 * The `EXPR_LIST_REQ`/`EXPR_LIST_RESP` round trip is retired — the picker
 * lists these entries directly, and `EXPR_TRIGGER` (see
 * [TriggerExpressionUseCase]) carries the tapped id.
 *
 * Single source: trigger validation ([TriggerExpressionUseCase]) and the
 * catalog pin test read the full list; the picker
 * ([com.desky.shared.presentation.RobotControlUiState.expressions]) reads
 * [PICKER_EXPRESSIONS] below. Mirror id + flags + name exactly; if the
 * firmware catalog grows, extend the full list and the pin test together.
 * Flags mirror the retired `EXPR_LIST_RESP` entry-flags byte (bit 0 =
 * play-once), so only `surprised` carries flags 1.
 */
val STATIC_EXPRESSIONS: List<ExpressionEntry> = listOf(
    ExpressionEntry(id = 0, flags = 0, name = "boot"),
    ExpressionEntry(id = 1, flags = 0, name = "happy"),
    ExpressionEntry(id = 2, flags = 0, name = "sad"),
    ExpressionEntry(id = 3, flags = 0, name = "blink"),
    ExpressionEntry(id = 4, flags = 0, name = "alert"),
    ExpressionEntry(id = 6, flags = 0, name = "curious"),
    ExpressionEntry(id = 7, flags = 1, name = "surprised"),
    ExpressionEntry(id = 8, flags = 0, name = "angry"),
    ExpressionEntry(id = 9, flags = 0, name = "scared"),
    ExpressionEntry(id = 10, flags = 0, name = "excited"),
    ExpressionEntry(id = 11, flags = 0, name = "confused"),
)

/** Id set of [STATIC_EXPRESSIONS]: the trigger allow-list. */
val STATIC_EXPRESSION_IDS: Set<Int> = STATIC_EXPRESSIONS.map { it.id }.toSet()

/**
 * Picker subset of [STATIC_EXPRESSIONS]: blink and alert stay triggerable
 * (validation reads the full allow-list) but leave the picker, which lists
 * the eight moods plus Neutral.
 */
val PICKER_EXPRESSIONS: List<ExpressionEntry> = listOf(
    ExpressionEntry(id = 0, flags = 0, name = "Neutral"),
    ExpressionEntry(id = 1, flags = 0, name = "happy"),
    ExpressionEntry(id = 2, flags = 0, name = "sad"),
    ExpressionEntry(id = 6, flags = 0, name = "curious"),
    ExpressionEntry(id = 7, flags = 1, name = "surprised"),
    ExpressionEntry(id = 8, flags = 0, name = "angry"),
    ExpressionEntry(id = 9, flags = 0, name = "scared"),
    ExpressionEntry(id = 10, flags = 0, name = "excited"),
    ExpressionEntry(id = 11, flags = 0, name = "confused"),
)

/**
 * Expression play request: send one `EXPR_TRIGGER` (`[0x5B | id | eraBE2 |
 * counterBE4 | tag6 | XOR]`,
 * `tag = first6(HMAC-SHA256(psk, "EXPRTRIG" || id || era || counter))`) to
 * the paired robot's address. Fire-and-forget: the robot gates on MANUAL
 * and replies nothing — a robot-side drop is silent by protocol.
 *
 * Unknown ids are rejected client-side against [knownIds] (the static
 * catalog by default) before anything is encoded or sent, so a stale picker
 * tap can never address a catalog entry outside the mirrored set. The
 * caller owns the counter exactly like the drive path (one allocation per
 * tap). Transport errors map to [Outcome.Failure] (the caller surfaces
 * them — a tap was an explicit user action). Cancellation is never
 * swallowed. Key material is never logged (lengths and booleans only).
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
        knownIds: Set<Int> = STATIC_EXPRESSION_IDS,
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
