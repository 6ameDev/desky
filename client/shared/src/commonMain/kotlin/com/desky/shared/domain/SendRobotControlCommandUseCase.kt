package com.desky.shared.domain

/** Encode [cmd] and send it to the paired robot's address. */
class SendRobotControlCommandUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(device: PairedDevice, cmd: ControlCommand): Outcome<Unit> {
        return try {
            udp.send(ControlPacketCodec.encode(cmd), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: Exception) {
            Outcome.Failure("Failed to send control command: ${e.message}", e)
        }
    }
}
