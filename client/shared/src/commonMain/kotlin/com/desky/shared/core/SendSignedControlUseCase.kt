package com.desky.shared.core

import kotlinx.coroutines.CancellationException

/**
 * Encode [cmd] as an authenticated `DRIVE` frame and send it to the
 * paired robot's address (pairing/auth Task 9).
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
