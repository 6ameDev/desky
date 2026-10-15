package com.desky.shared.domain

/**
 * Client-defined emergency-stop flag bit.
 *
 * The firmware ignores the control flags byte today; this bit is reserved
 * for the future failsafe extension (robot-side stop on sight of the bit,
 * plus the existing ~500 ms command-stale failsafe in [STALE_MS]).
 */
const val ESTOP_FLAG = 0x01

/**
 * Send a signed zeroed MANUAL command with [ESTOP_FLAG] set (pairing/auth
 * Task 9: e-stop rides the same authenticated `DRIVE_V2` path as every
 * other drive frame, via [SendSignedControlUseCase]).
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
        } catch (e: Exception) {
            Outcome.Failure("Failed to send emergency stop: ${e.message}", e)
        }
    }
}
