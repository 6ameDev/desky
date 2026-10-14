package com.desky.shared.domain

/** Drop the current pairing. */
class ForgetDeviceUseCase(
    private val deviceRepo: DeviceRepository,
) {
    suspend operator fun invoke(): Outcome<Unit> {
        return try {
            deviceRepo.clear()
            Outcome.Success(Unit)
        } catch (e: Exception) {
            Outcome.Failure("Failed to forget paired device: ${e.message}", e)
        }
    }
}
