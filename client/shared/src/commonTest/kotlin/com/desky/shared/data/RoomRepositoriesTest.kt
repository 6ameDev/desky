package com.desky.shared.data

import androidx.room3.Room
import androidx.sqlite.driver.bundled.BundledSQLiteDriver
import app.cash.turbine.test
import com.desky.shared.data.db.DeskyDatabase
import com.desky.shared.data.db.toDomain
import com.desky.shared.data.db.toEntity
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotConfig
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

/**
 * Repository tests against a REAL Room database: JVM in-memory builder
 * (`Room.inMemoryDatabaseBuilder` + bundled SQLite driver), fresh per test.
 */
class RoomRepositoriesTest {

    private lateinit var db: DeskyDatabase

    @BeforeTest
    fun setup() {
        db = Room.inMemoryDatabaseBuilder<DeskyDatabase>()
            .setDriver(BundledSQLiteDriver())
            .build()
    }

    @AfterTest
    fun teardown() {
        db.close()
    }

    // -- mapping round-trips (also the fallback coverage if Room ever
    //    stops resolving on JVM; see final report) --

    @Test
    fun deviceMappingRoundTrip() {
        val device = PairedDevice(
            id = "desky-01",
            ip = "192.168.4.2",
            port = 3333,
            authToken = "tok",
            alias = "bench",
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    @Test
    fun configMappingRoundTrip() {
        val config = RobotConfig(
            speedLimit = 0.6f,
            ledProfile = 2,
            pidP = 1f,
            pidI = 0.5f,
            pidD = 0.1f,
            wifiSsid = "desky-ap",
        )
        assertEquals(config, config.toEntity().toDomain())
    }

    // -- DeviceRepository against real Room --

    @Test
    fun pairedStartsEmpty() = runTest {
        RoomDeviceRepository(db.pairedDeviceDao()).observePaired().test {
            assertNull(awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun saveThenObserveEmitsDevice() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        val device = PairedDevice(id = "a", ip = "192.168.4.2", authToken = "t1")

        repo.save(device)

        repo.observePaired().test {
            assertEquals(device, awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun secondSaveReplacesFirstKeepingSingleRow() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(PairedDevice(id = "a", ip = "192.168.4.2", authToken = "t1"))
        val second = PairedDevice(id = "b", ip = "192.168.4.3", authToken = "t2", alias = "field")

        repo.save(second)

        repo.observePaired().test {
            assertEquals(second, awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun clearDropsPairing() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(PairedDevice(id = "a", ip = "192.168.4.2", authToken = "t1"))

        repo.clear()

        repo.observePaired().test {
            assertNull(awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    // -- SettingsRepository against real Room --

    @Test
    fun configStartsAsDefault() = runTest {
        RoomSettingsRepository(db.robotConfigDao()).observeConfig().test {
            assertEquals(RobotConfig(), awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun saveThenObserveEmitsConfig() = runTest {
        val repo = RoomSettingsRepository(db.robotConfigDao())
        val config = RobotConfig(speedLimit = 0.4f, wifiSsid = "desky-ap")

        repo.save(config)

        repo.observeConfig().test {
            assertEquals(config, awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }
}
