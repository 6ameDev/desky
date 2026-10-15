package com.desky.shared.data.db

import androidx.room3.AutoMigration
import androidx.room3.ConstructedBy
import androidx.room3.Database
import androidx.room3.RoomDatabase
import androidx.room3.RoomDatabaseConstructor

/**
 * Room database (T5 data layer, version 2).
 *
 * v1 -> v2 is an additive auto-migration: nullable `pskHex` on
 * `paired_device` (pairing/auth v1 bond secret; rows written before v1 read
 * back null). No manual spec needed.
 *
 * [DeskyDatabaseConstructor] is hand-declared here as `expect` (per the
 * official Room KMP setup); each per-target KSP run generates the matching
 * `actual` plus the platform `*_Impl`. Schema JSON is exported per the
 * `room3 { schemaDirectory }` config in `:shared/build.gradle.kts`.
 */
@Database(
    entities = [PairedDeviceEntity::class, RobotConfigEntity::class],
    version = 2,
    autoMigrations = [AutoMigration(from = 1, to = 2)],
)
@ConstructedBy(DeskyDatabaseConstructor::class)
abstract class DeskyDatabase : RoomDatabase() {
    abstract fun pairedDeviceDao(): PairedDeviceDao

    abstract fun robotConfigDao(): RobotConfigDao
}

/**
 * Hand-written `expect` constructor stub. The Room compiler generates the
 * `actual` implementations (one per target); there is intentionally no
 * hand-written `actual` anywhere.
 */
@Suppress("KotlinNoActualForExpect", "NO_ACTUAL_FOR_EXPECT")
expect object DeskyDatabaseConstructor : RoomDatabaseConstructor<DeskyDatabase> {
    override fun initialize(): DeskyDatabase
}
