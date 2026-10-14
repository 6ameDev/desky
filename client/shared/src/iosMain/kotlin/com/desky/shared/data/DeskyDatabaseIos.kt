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
 */
fun createDeskyDatabase(): DeskyDatabase {
    val dbPath = NSHomeDirectory() + "/$DATABASE_NAME"
    return Room.databaseBuilder<DeskyDatabase>(
        name = dbPath,
    ).setDriver(BundledSQLiteDriver()).build()
}
