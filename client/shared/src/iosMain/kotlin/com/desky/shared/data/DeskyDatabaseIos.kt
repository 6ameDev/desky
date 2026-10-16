package com.desky.shared.data

import androidx.room3.Room
import androidx.sqlite.driver.bundled.BundledSQLiteDriver
import com.desky.shared.data.db.DATABASE_NAME
import com.desky.shared.data.db.DeskyDatabase
import platform.Foundation.NSHomeDirectory

/**
 * iOS database builder (T5 data layer): file-backed Room database under
 * `NSHomeDirectory` with the bundled SQLite driver. No DI wiring here (T6);
 * call from the host app.
 *
 * Dev-stage wipe-freely: destructive migration on schema change (Task 9:
 * v2 -> v3 drops `pskHex`, adds `lastCounter`). This MUST be replaced with
 * a real `Migration` before real customers exist, or they lose their
 * pairing (and drive counter) on upgrade.
 * TODO(tracking-issue): file the v2→v3 `Migration` tracking issue and link
 * it here (see `DeskyDatabase` KDoc; firmware twin: NVS `kConfigVersion`
 * wipe in `ConfigStore::begin`).
 */
fun createDeskyDatabase(): DeskyDatabase {
    val dbPath = NSHomeDirectory() + "/$DATABASE_NAME"
    return Room.databaseBuilder<DeskyDatabase>(
        name = dbPath,
    ).setDriver(BundledSQLiteDriver()).fallbackToDestructiveMigration().build()
}
