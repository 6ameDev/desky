package com.desky.shared.domain

import kotlinx.coroutines.CancellationException

/**
 * Robot alive rule keepalive + goodbye (see `shared/protocol/README.md`
 * §"ALIVE + DISCONNECT + robot alive rule").
 *
 * - [invoke] sends one `ALIVE` (`[0x55 | eraBE2 | counterBE4 | tag6 |
 *   XOR]`, `tag = first6(HMAC-SHA256(psk, "ALIVE" || era || counter))`)
 *   to the paired robot's address. Fire-and-forget: the robot never
 *   replies — steady telemetry is the ACK. The caller (the control
 *   ViewModel's send loop) owns the counter exactly like the drive path:
 *   allocated from the shared [DriveCounter] via its session helper, so
 *   `ALIVE` + `DRIVE_V2` + `REKEY_REQ` share one strictly increasing
 *   sequence (a valid `ALIVE` consumes `lastCounter_` exactly like drive).
 * - [sendDisconnect] sends one `DISCONNECT` (`[0x56 | eraBE2 | counterBE4
 *   | tag6 | XOR]`); the caller repeats it 2x byte-identical (same
 *   counter — one allocation, like `UNBOND`'s 3x in [ForgetDeviceUseCase])
 *   on backgrounding. The robot refresh-then-clears (net peer dropped,
 *   bond KEPT).
 *
 * Silent: transport errors map to [Outcome.Failure] and the caller
 * deliberately never surfaces them (no error banner — a lost keepalive
 * just means telemetry stays silent and the link demotes). Cancellation
 * is never swallowed.
 */
class AliveUseCase(
    private val udp: UdpSocketManager,
) {
    suspend operator fun invoke(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
    ): Outcome<Unit> {
        return try {
            udp.send(encodeAlive(psk, era, counter), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send alive keepalive: ${e.message}", e)
        }
    }

    suspend fun sendDisconnect(
        device: PairedDevice,
        psk: ByteArray,
        era: UShort,
        counter: UInt,
    ): Outcome<Unit> {
        return try {
            udp.send(encodeDisconnect(psk, era, counter), device.ip, device.port)
            Outcome.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send disconnect goodbye: ${e.message}", e)
        }
    }
}
