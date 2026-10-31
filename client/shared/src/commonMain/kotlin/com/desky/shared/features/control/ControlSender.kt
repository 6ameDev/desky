package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.RobotMode
import com.desky.shared.core.UdpSocketManager
import com.desky.shared.core.encodeSigned
import kotlinx.coroutines.CancellationException

/**
 * Encode [cmd] as an authenticated `DRIVE` frame and send it to the
 * paired robot's address.
 *
 * The caller owns the counter: strictly increasing per device, 1 for the
 * first wire packet (0 is never valid), persisted to Room every 128 sends
 * by the drive session. Counter allocation failures (exhaustion) are the
 * caller's to report — this use case only maps transport errors.
 */
class SendSignedControlUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(
        device: PairedDevice,
        cmd: ControlCommand,
        psk: ByteArray,
        counter: UInt,
    ): Outcome<Unit> {
        return try {
            udp.send(encodeSigned(cmd, counter, psk), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send control command: ${e.message}", e)
        }
    }
}

/**
 * Client-defined emergency-stop flag bit.
 *
 * The firmware ignores the control flags byte today; this bit is reserved
 * for the future failsafe extension (robot-side stop on sight of the bit,
 * plus the existing ~500 ms command-stale failsafe
 * ([STALE_MS][com.desky.shared.core.STALE_MS]).
 */
const val ESTOP_FLAG = 0x01

/**
 * Send a signed zeroed MANUAL command with [ESTOP_FLAG] set: e-stop rides
 * the same authenticated `DRIVE` path as every other drive frame, via
 * [SendSignedControlUseCase].
 *
 * The caller owns the counter exactly like the drive loop (strictly
 * increasing, 1 first, never 0); the drive session allocates it so the
 * immediate frame and the loop frames share one sequence.
 */
class EmergencyStopUseCase(
    private val sendSigned: SendSignedControlUseCase,
) {
    suspend operator fun invoke(device: PairedDevice, psk: ByteArray, counter: UInt): Outcome<Unit> {
        return try {
            val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG)
            sendSigned(device, cmd, psk, counter)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send emergency stop: ${e.message}", e)
        }
    }
}
