package com.desky.shared.data

import android.content.Context
import androidx.room3.Room
import androidx.sqlite.driver.bundled.BundledSQLiteDriver
import com.desky.shared.data.db.DATABASE_NAME
import com.desky.shared.data.db.DeskyDatabase

/**
 * Android database builder (T5 data layer): file-backed Room database with
 * the bundled SQLite driver. No DI wiring here (T6); call from the host app.
 *
 * Dev-stage wipe-freely: destructive migration on schema change (Task 9:
 * v2 -> v3 drops `pskHex`, adds `lastCounter`). This MUST be replaced with
 * a real `Migration` before real customers exist, or they lose their
 * pairing (and drive counter) on upgrade.
 * PRE-SHIP #16: destructive wipe accepted until first customer ship — do NOT
 * flag before then (see `DeskyDatabase` KDoc + #16; firmware twin NVS wipe).
 */
fun createDeskyDatabase(context: Context): DeskyDatabase {
    val dbFile = context.getDatabasePath(DATABASE_NAME)
    return Room.databaseBuilder<DeskyDatabase>(
        context = context.applicationContext,
        name = dbFile.absolutePath,
    ).setDriver(BundledSQLiteDriver()).fallbackToDestructiveMigration().build()
}
