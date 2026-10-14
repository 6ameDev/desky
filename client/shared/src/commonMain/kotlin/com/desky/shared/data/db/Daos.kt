package com.desky.shared.data.db

import androidx.room3.Dao
import androidx.room3.Query
import androidx.room3.Upsert
import kotlinx.coroutines.flow.Flow

/**
 * Room DAOs (T5 data layer). Only the `observe*` queries return [Flow];
 * every other function is suspending.
 */
@Dao
interface PairedDeviceDao {
    /** All paired rows (0..1 by repository invariant), as a hot flow. */
    @Query("SELECT * FROM paired_device")
    fun observeAll(): Flow<List<PairedDeviceEntity>>

    /** Insert or replace by primary key. */
    @Upsert
    suspend fun upsert(device: PairedDeviceEntity)

    /** Drop the pairing (all rows). */
    @Query("DELETE FROM paired_device")
    suspend fun deleteAll()
}

@Dao
interface RobotConfigDao {
    /** The single config row, or null when never saved. */
    @Query("SELECT * FROM robot_config WHERE id = $SINGLE_CONFIG_ID")
    fun observe(): Flow<RobotConfigEntity?>

    /** Insert or replace the single config row. */
    @Upsert
    suspend fun upsert(config: RobotConfigEntity)
}
