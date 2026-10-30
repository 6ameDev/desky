package com.desky.shared.core.db

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
     * Persist a Tier-1 pair-refresh rotation: the new era + the pair-baseline
     * counter (0 — the first post-pair-refresh drive uses 1, same as a fresh
     * pair). Called once per confirmed `PAIR_REFRESH_ACK` by the pair-refresh use case;
     * keeps the row (the vault PSK update is a separate call).
     * Also resets the receive-domain watermark (`lastRobotCounter = 0`):
     * the robot restarts its own send sequence with the new era.
     */
    @Query("UPDATE paired_device SET lastCounter = :counter, era = :era, lastRobotCounter = 0 WHERE id = :deviceId")
    suspend fun updatePairRefresh(deviceId: String, counter: Long, era: Int)

    /**
     * Persist an accepted robot-originated counter mid-session (called on
     * every verified robot frame by the receive domain — evictions are
     * rare, so direct persist, no cadence). Keeps the row; keyed by the
     * peer id.
     */
    @Query("UPDATE paired_device SET lastRobotCounter = :counter WHERE id = :deviceId")
    suspend fun updateRobotCounter(deviceId: String, counter: Long)
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
