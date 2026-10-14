package com.desky.shared.data

import android.content.Context
import androidx.room3.Room
import androidx.sqlite.driver.bundled.BundledSQLiteDriver
import com.desky.shared.data.db.DATABASE_NAME
import com.desky.shared.data.db.DeskyDatabase

/**
 * Android database builder (T5 data layer): file-backed Room database with
 * the bundled SQLite driver. No DI wiring here (T6); call from the host app.
 */
fun createDeskyDatabase(context: Context): DeskyDatabase {
    val dbFile = context.getDatabasePath(DATABASE_NAME)
    return Room.databaseBuilder<DeskyDatabase>(
        context = context.applicationContext,
        name = dbFile.absolutePath,
    ).setDriver(BundledSQLiteDriver()).build()
}
