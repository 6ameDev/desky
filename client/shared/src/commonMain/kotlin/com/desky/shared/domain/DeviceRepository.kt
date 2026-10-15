package com.desky.shared.domain

import kotlinx.coroutines.flow.Flow

/** Persistence boundary for the currently paired robot (at most one). */
interface DeviceRepository {
    /** Currently paired device, or null when nothing is paired. */
    fun observePaired(): Flow<PairedDevice?>

    /** Persist the paired device, replacing any previous pairing. */
    suspend fun save(device: PairedDevice)

    /** Drop the pairing. */
    suspend fun clear()

    /**
     * Persist the drive counter for [deviceId] (signed send loop, every 128
     * sends). The session resumes from this value; a fresh pairing resets it
     * to 0 via [save] so the first wire packet uses counter 1.
     */
    suspend fun updateCounter(deviceId: String, counter: UInt)
}
