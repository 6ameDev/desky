package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.RobotMode
import com.desky.shared.core.UdpSocketManager
import com.desky.shared.core.encodeManualRelease
import com.desky.shared.core.encodeManualTake
import kotlinx.coroutines.CancellationException

/**
 * Manual mode entry: send one `MANUAL_TAKE` (`[0x57 | eraBE2 | counterBE4
 * | tag6 | XOR]`, `tag = first6(HMAC-SHA256(psk, "MANUALTAKE" || era ||
 * counter))`) to the paired robot's address. Fire-and-forget: the robot
 * never replies — the telemetry `statusFlags` MANUAL bit is the ACK. The
 * caller (the control ViewModel's take action) owns the counter exactly
 * like the drive path: allocated from the shared [ToRobotCounter][com.desky.shared.core.ToRobotCounter] via its
 * session helper, so `MANUAL_TAKE` consumes the monotonic sequence like
 * every other signed frame. Entry is deliberate: one frame per user
 * action, never repeated.
 *
 * Transport errors map to [Outcome.Failure] (the caller surfaces them —
 * entry was an explicit user action); a lost take just leaves the robot
 * out of MANUAL. Cancellation is never swallowed. Key material is never
 * logged (lengths and booleans only).
 */
class TakeManualUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
    ): Outcome<Unit> {
        return try {
            udp.send(encodeManualTake(psk, era, counter), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send manual take: ${e.message}", e)
        }
    }
}

/**
 * Manual mode exit: send `MANUAL_RELEASE` (`[0x58 | eraBE2 | counterBE4 |
 * tag6 | XOR]`, `tag = first6(HMAC-SHA256(psk, "MANUALRELEASE" || era ||
 * counter))`) 2x byte-identical (one encoding, same counter — one
 * allocation, like `DISCONNECT`'s 2x goodbye) to the paired robot's
 * address. Fire-and-forget: the robot never replies. Best-effort: every
 * transmit error is swallowed (a lost release just leaves the robot in
 * MANUAL until its own command-stale exit); cancellation is never
 * swallowed. Key material is never logged (lengths and booleans only).
 */
class ReleaseManualUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
    ): Outcome<Unit> {
        return try {
            val frame = encodeManualRelease(psk, era, counter)
            repeat(MAX_RELEASE_SENDS) {
                try {
                    udp.send(frame, device.ip, device.port)
                } catch (e: CancellationException) {
                    throw e
                } catch (_: Exception) {
                    // UDP loss / dead socket: the robot's own command-stale
                    // exit covers the stop; release stays best-effort.
                }
            }
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send manual release: ${e.message}", e)
        }
    }

    companion object {
        /** Fire-and-forget `MANUAL_RELEASE` transmissions (no reply ever comes back). */
        const val MAX_RELEASE_SENDS = 2
    }
}
