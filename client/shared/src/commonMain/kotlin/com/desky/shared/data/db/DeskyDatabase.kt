package com.desky.shared.data.db

import androidx.room3.ConstructedBy
import androidx.room3.Database
import androidx.room3.RoomDatabase
import androidx.room3.RoomDatabaseConstructor

/**
 * Room database (T5 data layer, version 1).
 *
 * [DeskyDatabaseConstructor] is hand-declared here as `expect` (per the
 * official Room KMP setup); each per-target KSP run generates the matching
 * `actual` plus the platform `*_Impl`. Schema JSON is exported per the
 * `room3 { schemaDirectory }` config in `:shared/build.gradle.kts`.
 */
@Database(
    entities = [PairedDeviceEntity::class, RobotConfigEntity::class],
    version = 1,
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
