package com.desky.shared.core

import com.desky.shared.core.db.RobotConfigDao
import com.desky.shared.core.db.toDomain
import com.desky.shared.core.db.toEntity
import com.desky.shared.core.RobotConfig
import com.desky.shared.core.SettingsRepository
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

/**
 * [SettingsRepository] backed by the single-row `robot_config` table
 * (T5 data layer).
 *
 * [observeConfig] maps a missing row to the default [RobotConfig]; [save]
 * upserts the fixed-id row. No DI wiring here (T6); construct manually.
 */
class RoomSettingsRepository(
    private val dao: RobotConfigDao,
) : SettingsRepository {
    override fun observeConfig(): Flow<RobotConfig> =
        dao.observe().map { entity -> entity?.toDomain() ?: RobotConfig() }

    override suspend fun save(config: RobotConfig) {
        dao.upsert(config.toEntity())
    }
}
