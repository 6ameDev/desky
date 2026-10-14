package com.desky.shared.domain

/**
 * Client-defined emergency-stop flag bit.
 *
 * The firmware ignores the control flags byte today; this bit is reserved
 * for the future failsafe extension (robot-side stop on sight of the bit,
 * plus the existing ~500 ms command-stale failsafe in [STALE_MS]).
 */
const val ESTOP_FLAG = 0x01

/** Send a zeroed MANUAL command with [ESTOP_FLAG] set. */
class EmergencyStopUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(device: PairedDevice): Outcome<Unit> {
        return try {
            val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG)
            udp.send(ControlPacketCodec.encode(cmd), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: Exception) {
            Outcome.Failure("Failed to send emergency stop: ${e.message}", e)
        }
    }
}
