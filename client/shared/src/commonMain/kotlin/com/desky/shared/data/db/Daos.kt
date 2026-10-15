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

    /**
     * Persist the drive counter mid-session (called every 128 sends by the
     * signed send loop, mirroring firmware A1). Keeps the row; the session
     * resumes from this value after a restart.
     */
    @Query("UPDATE paired_device SET lastCounter = :counter WHERE id = :deviceId")
    suspend fun updateCounter(deviceId: String, counter: Long)
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
