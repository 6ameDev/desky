package com.desky.shared.core.db

import androidx.room3.Entity
import androidx.room3.PrimaryKey
import com.desky.shared.core.LEGACY_ROBOT_HOST
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotConfig

/**
 * Room entities + entity <-> domain mapping (T5 data layer).
 *
 * `paired_device` holds 0..1 rows (the single current pairing; enforced by
 * [com.desky.shared.core.RoomDeviceRepository], not by schema).
 * `robot_config` holds exactly one row with fixed [RobotConfigEntity.id] 0.
 */

/** File name used by both platform `createDeskyDatabase` builders. */
const val DATABASE_NAME = "desky.db"

/** Single-row id for the robot config table. */
const val SINGLE_CONFIG_ID = 0

@Entity(tableName = "paired_device")
data class PairedDeviceEntity(
    @PrimaryKey val id: String,
    val ip: String,
    val port: Int,
    val authToken: String,
    /**
     * Last drive counter persisted by the signed send loop (every 128 sends,
     * mirroring firmware A1). Stored as [Long] — SQLite INTEGER is 64-bit and
     * Room has no `UInt` column type; the domain mapping narrows to `UInt`
     * (the wire counter is 32-bit, so every stored value fits).
     */
    val lastCounter: Long = 0L,
    val alias: String,
    /**
     * Counter-persist cadence negotiated once at pair (v1.1 `PAIR_OK`
     * interval byte; v4 column, default 128 = [ToRobotCounter.COUNTER_PERSIST_EVERY]).
     * The drive session reads it back on restart so the negotiated cadence
     * survives app kills (producers sanitize 8..1024 else 128 before save).
     */
    val persistEvery: Int = 128,
    /**
     * Tier-1 pair-refresh epoch (u16 on the wire as `eraBE2`, v5 column, default 0
     * = fresh-pair era). Stored as [Int] — SQLite INTEGER is 64-bit and Room
     * has no `UShort` column type; the domain mapping narrows to `UShort`
     * (every stored value fits: producers mask to 0..65535). Incremented by
     * each silent era+PSK rotation (which resets [lastCounter] to the pair
     * baseline); the drive session reads it back on restart so the era
     * survives app kills.
     */
    val era: Int = 0,
    /**
     * Receive-domain watermark (v6 column, default 0 = fresh-pair
     * baseline): the last robot-originated counter accepted. Stored as
     * [Long] like [lastCounter] (SQLite INTEGER is 64-bit; the domain
     * mapping narrows to `UInt`). Reset to 0 on every pair success and
     * refresh switch alongside the send baseline; evictions persist here
     * on accept (rare frames — direct persist, no cadence).
     */
    val lastRobotCounter: Long = 0L,
)

@Entity(tableName = "robot_config")
data class RobotConfigEntity(
    @PrimaryKey val id: Int = SINGLE_CONFIG_ID,
    val speedLimit: Float,
    val ledProfile: Int,
    val pidP: Float,
    val pidI: Float,
    val pidD: Float,
    val wifiSsid: String,
)

/** Entity -> domain. */
fun PairedDeviceEntity.toDomain(): PairedDevice =
    PairedDevice(
        id = id,
        // AP-era rows stored the fixed literal: resolve them via mDNS like
        // fresh pairs instead of wedging on a dead address. The row id is
        // untouched (vault/counter key), and the next save heals the row.
        ip = if (ip == LEGACY_ROBOT_HOST) ROBOT_HOST else ip,
        port = port,
        authToken = authToken,
        lastCounter = lastCounter.toUInt(),
        alias = if (alias == LEGACY_ROBOT_HOST) ROBOT_HOST else alias,
        persistEvery = persistEvery,
        era = era.toUShort(),
        lastRobotCounter = lastRobotCounter.toUInt(),
    )

/** Domain -> entity. */
fun PairedDevice.toEntity(): PairedDeviceEntity =
    PairedDeviceEntity(
        id = id,
        ip = ip,
        port = port,
        authToken = authToken,
        lastCounter = lastCounter.toLong(),
        alias = alias,
        persistEvery = persistEvery,
        era = era.toInt(),
        lastRobotCounter = lastRobotCounter.toLong(),
    )

/** Entity -> domain. */
fun RobotConfigEntity.toDomain(): RobotConfig =
    RobotConfig(
        speedLimit = speedLimit,
        ledProfile = ledProfile,
        pidP = pidP,
        pidI = pidI,
        pidD = pidD,
        wifiSsid = wifiSsid,
    )

/** Domain -> entity (always the single fixed-id row). */
fun RobotConfig.toEntity(): RobotConfigEntity =
    RobotConfigEntity(
        id = SINGLE_CONFIG_ID,
        speedLimit = speedLimit,
        ledProfile = ledProfile,
        pidP = pidP,
        pidI = pidI,
        pidD = pidD,
        wifiSsid = wifiSsid,
    )
