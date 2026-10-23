package com.desky.shared.data

import com.desky.shared.data.db.PairedDeviceDao
import com.desky.shared.data.db.toDomain
import com.desky.shared.data.db.toEntity
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.PairedDevice
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

/**
 * [DeviceRepository] backed by the `paired_device` table (T5 data layer).
 *
 * The table holds 0..1 rows: [observePaired] maps the single-row flow to
 * `firstOrNull`, [save] upserts after clearing any previous pairing (device
 * ids vary per robot, so a bare upsert could leave two rows), and [clear]
 * deletes all rows. No DI wiring here (T6); construct manually.
 */
class RoomDeviceRepository(
    private val dao: PairedDeviceDao,
) : DeviceRepository {
    override fun observePaired(): Flow<PairedDevice?> =
        dao.observeAll().map { rows -> rows.firstOrNull()?.toDomain() }

    override suspend fun save(device: PairedDevice) {
        dao.deleteAll()
        dao.upsert(device.toEntity())
    }

    override suspend fun clear() {
        dao.deleteAll()
    }

    override suspend fun updateCounter(deviceId: String, counter: UInt) {
        dao.updateCounter(deviceId, counter.toLong())
    }

    override suspend fun updatePairRefresh(deviceId: String, counter: UInt, era: UShort) {
        dao.updatePairRefresh(deviceId, counter.toLong(), era.toInt())
    }

    override suspend fun updateRobotCounter(deviceId: String, counter: UInt) {
        dao.updateRobotCounter(deviceId, counter.toLong())
    }
}
