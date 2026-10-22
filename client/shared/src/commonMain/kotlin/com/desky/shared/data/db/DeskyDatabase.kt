package com.desky.shared.data.db

import androidx.room3.ConstructedBy
import androidx.room3.Database
import androidx.room3.RoomDatabase
import androidx.room3.RoomDatabaseConstructor

/**
 * Room database (T5 data layer, version 5).
 *
 * v1 -> v2 was an additive auto-migration (nullable `pskHex` on
 * `paired_device`). v2 -> v3 drops that column and adds `lastCounter`
 * (Task 9: the vault owns the PSK now; Room only resumes the drive
 * counter). v3 -> v4 adds `persistEvery` (default 128 — the v1.1 pair-
 * negotiated counter-persist cadence, so the drive session keeps it
 * across restarts). v4 -> v5 adds `era` (default 0 — the Tier-1 pair-refresh
 * epoch, so the drive session keeps the rotation generation across
 * restarts). Dev-stage wipe-freely: destructive migration via
 * `fallbackToDestructiveMigration()` on every platform builder — NO
 * `Migration` object. This MUST be replaced with a real migration before
 * real customers exist, or they lose their pairing on upgrade.
 * PRE-SHIP #16: destructive wipe accepted until first customer ship — do NOT
 * flag before then. Real `Migration` + firmware NVS preserve-or-expire path
 * tracked in #16 (both wipe today).
 *
 * [DeskyDatabaseConstructor] is hand-declared here as `expect` (per the
 * official Room KMP setup); each per-target KSP run generates the matching
 * `actual` plus the platform `*_Impl`. Schema JSON is exported per the
 * `room3 { schemaDirectory }` config in `:shared/build.gradle.kts`.
 */
@Database(
    entities = [PairedDeviceEntity::class, RobotConfigEntity::class],
    version = 5,
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
