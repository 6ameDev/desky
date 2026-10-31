package com.desky.shared.core

import androidx.room3.Room
import androidx.sqlite.driver.bundled.BundledSQLiteDriver
import app.cash.turbine.test
import com.desky.shared.core.db.DeskyDatabase
import com.desky.shared.core.db.PairedDeviceEntity
import com.desky.shared.core.db.toDomain
import com.desky.shared.core.db.toEntity
import com.desky.shared.core.DEFAULT_UDP_PORT
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.core.RobotConfig
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
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
            port = DEFAULT_UDP_PORT,
            authToken = "tok",
            alias = "bench",
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    @Test
    fun deviceMappingRoundTripsLastCounter() {
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            port = DEFAULT_UDP_PORT,
            authToken = "psk-v1",
            lastCounter = TEST_COUNTER_WATERMARK,
            alias = ROBOT_HOST,
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    @Test
    fun deviceMappingRoundTripsPersistEvery() {
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            port = DEFAULT_UDP_PORT,
            authToken = "psk-v1",
            lastCounter = 0u,
            alias = ROBOT_HOST,
            persistEvery = 16,
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    @Test
    fun deviceMappingRoundTripsEra() {
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            port = DEFAULT_UDP_PORT,
            authToken = "psk-v1",
            lastCounter = 0u,
            alias = ROBOT_HOST,
            persistEvery = TEST_PERSIST_EVERY,
            era = 7u,
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    @Test
    fun deviceMappingRoundTripsLastRobotCounter() {
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            port = DEFAULT_UDP_PORT,
            authToken = "psk-v1",
            lastCounter = 100u,
            alias = ROBOT_HOST,
            persistEvery = TEST_PERSIST_EVERY,
            era = 1u,
            lastRobotCounter = 77u,
        )
        assertEquals(device, device.toEntity().toDomain())
    }

    /**
     * Stored-literal row (dotted quad from an earlier pair): reads resolve
     * via mDNS like a fresh pair instead of wedging on the dead address,
     * with the row id (vault/counter key), watermark, and port untouched.
     * No schema change, so no migration — the next save heals the row.
     */
    @Test
    fun storedLiteralRowResolvesViaHostnameKeepingId() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(
            PairedDevice(
                id = "desky-192.168.4.1",
                ip = "192.168.4.1",
                authToken = "psk-v1",
                lastCounter = 96u,
                alias = "192.168.4.1",
            ),
        )

        repo.observePaired().test {
            val device = assertNotNull(awaitItem())
            assertEquals(ROBOT_HOST, device.ip)
            assertEquals(ROBOT_HOST, device.alias)
            assertEquals("desky-192.168.4.1", device.id)
            assertEquals(96u, device.lastCounter)
            assertEquals(DEFAULT_UDP_PORT, device.port)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun configMappingRoundTrip() {        val config = RobotConfig(
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

    @Test
    fun updateCounterPersistsKeepingRow() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(
            PairedDevice(
                id = "desky-$ROBOT_HOST",
                ip = ROBOT_HOST,
                authToken = "psk-v1",
                lastCounter = 0u,
                alias = ROBOT_HOST,
            ),
        )

        repo.updateCounter("desky-$ROBOT_HOST", TEST_COUNTER_WATERMARK)

        repo.observePaired().test {
            val device = assertNotNull(awaitItem())
            assertEquals(TEST_COUNTER_WATERMARK, device.lastCounter)
            assertEquals("desky-$ROBOT_HOST", device.id)
            assertEquals(ROBOT_HOST, device.ip)
            assertEquals("psk-v1", device.authToken)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun saveThenObserveKeepsNegotiatedPersistEvery() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            authToken = "psk-v1",
            lastCounter = 0u,
            alias = ROBOT_HOST,
            persistEvery = 16,
        )

        repo.save(device)

        repo.observePaired().test {
            assertEquals(device, awaitItem())
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun updatePairRefreshPersistsEraAndBaselineKeepingRow() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(
            PairedDevice(
                id = "desky-$ROBOT_HOST",
                ip = ROBOT_HOST,
                authToken = "psk-v1",
                lastCounter = 100u,
                alias = ROBOT_HOST,
                persistEvery = TEST_PERSIST_EVERY,
                era = 0u,
                lastRobotCounter = 77u,
            ),
        )

        repo.updatePairRefresh("desky-$ROBOT_HOST", 0u, 1u)

        repo.observePaired().test {
            val device = assertNotNull(awaitItem())
            assertEquals(0u, device.lastCounter)
            assertEquals(1u, device.era)
            assertEquals(TEST_PERSIST_EVERY, device.persistEvery, "rotation keeps the negotiated cadence")
            assertEquals(0u, device.lastRobotCounter, "rotation resets the receive domain")
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun updateRobotCounterPersistsKeepingRow() = runTest {
        val repo = RoomDeviceRepository(db.pairedDeviceDao())
        repo.save(
            PairedDevice(
                id = "desky-$ROBOT_HOST",
                ip = ROBOT_HOST,
                authToken = "psk-v1",
                lastCounter = 5000u,
                alias = ROBOT_HOST,
            ),
        )

        repo.updateRobotCounter("desky-$ROBOT_HOST", 5u)

        repo.observePaired().test {
            val device = assertNotNull(awaitItem())
            assertEquals(5u, device.lastRobotCounter)
            assertEquals(5000u, device.lastCounter, "send watermark untouched by the receive persist")
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

    private companion object {
        const val TEST_PERSIST_EVERY = 128
        val TEST_COUNTER_WATERMARK = 128u
    }
}
