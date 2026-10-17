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
     * Persist the drive counter mid-session (called every negotiated persist
     * cadence by the signed send loop, default 128, mirroring the
     * firmware). Keeps the row; the session resumes from this value after
     * a restart.
     */
    @Query("UPDATE paired_device SET lastCounter = :counter WHERE id = :deviceId")
    suspend fun updateCounter(deviceId: String, counter: Long)

    /**
     * Persist a Tier-1 rekey rotation: the new era + the bond-baseline
     * counter (0 — the first post-rekey drive uses 1, same as a fresh
     * pair). Called once per confirmed `REKEY_ACK` by the rekey use case;
     * keeps the row (the vault PSK update is a separate call).
     */
    @Query("UPDATE paired_device SET lastCounter = :counter, era = :era WHERE id = :deviceId")
    suspend fun updateRekey(deviceId: String, counter: Long, era: Int)
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
