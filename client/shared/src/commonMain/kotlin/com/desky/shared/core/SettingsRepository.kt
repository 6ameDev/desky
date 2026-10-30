package com.desky.shared.core

import kotlinx.coroutines.flow.Flow

/** Persistence boundary for robot tuning/settings. */
interface SettingsRepository {
    /** Current config as a hot flow (emits on every change). */
    fun observeConfig(): Flow<RobotConfig>

    /** Persist the config, replacing the previous one. */
    suspend fun save(config: RobotConfig)
}
