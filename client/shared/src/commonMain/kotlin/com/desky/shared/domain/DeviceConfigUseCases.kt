package com.desky.shared.domain

import kotlinx.coroutines.flow.Flow

/** Persist robot tuning/settings. */
class SaveDeviceConfigUseCase(
    private val settingsRepo: SettingsRepository,
) {
    suspend operator fun invoke(config: RobotConfig): Outcome<Unit> {
        // Note: RobotConfig validates itself in init, so an invalid config
        // cannot be constructed — IllegalArgumentException from model
        // validation can only surface here if a caller builds the config
        // reflectively or validation moves; still mapped, never thrown.
        return try {
            settingsRepo.save(config)
            Outcome.Success(Unit)
        } catch (e: IllegalArgumentException) {
            Outcome.Failure("Invalid robot config: ${e.message}", e)
        } catch (e: Exception) {
            Outcome.Failure("Failed to save robot config: ${e.message}", e)
        }
    }
}

/** Pass-through hot flow of the stored robot config. */
class ObserveDeviceConfigUseCase(
    private val settingsRepo: SettingsRepository,
) {
    operator fun invoke(): Flow<RobotConfig> = settingsRepo.observeConfig()
}
