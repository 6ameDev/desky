package com.desky.shared.data.db

import androidx.room3.Entity
import androidx.room3.PrimaryKey
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotConfig

/**
 * Room entities + entity <-> domain mapping (T5 data layer).
 *
 * `paired_device` holds 0..1 rows (the single current pairing; enforced by
 * [com.desky.shared.data.RoomDeviceRepository], not by schema).
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
        ip = ip,
        port = port,
        authToken = authToken,
        lastCounter = lastCounter.toUInt(),
        alias = alias,
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
