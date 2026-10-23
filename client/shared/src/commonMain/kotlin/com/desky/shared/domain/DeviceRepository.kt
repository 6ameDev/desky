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
     * Persist the drive counter for [deviceId] (signed send loop, every
     * [PairedDevice.persistEvery] sends — negotiated once at pair, default
     * 128). The session resumes from this value; a fresh pairing resets it
     * to 0 via [save] so the first wire packet uses counter 1.
     */
    suspend fun updateCounter(deviceId: String, counter: UInt)

    /**
     * Persist a Tier-1 pair-refresh rotation for [deviceId]: the new [era] + the
     * pair-baseline [counter] (0 — the first post-pair-refresh drive uses 1).
     * Called once per confirmed `PAIR_REFRESH_ACK`; keeps the row (the vault PSK
     * update is a separate call). Also resets the receive-domain watermark to
     * 0 (the robot restarts its own send sequence with the new era).
     */
    suspend fun updatePairRefresh(deviceId: String, counter: UInt, era: UShort)

    /**
     * Persist an accepted robot-originated counter for [deviceId] (receive
     * domain — called on every verified robot frame; evictions are rare, so
     * direct persist, no cadence). A fresh pairing resets it to 0 via [save].
     */
    suspend fun updateRobotCounter(deviceId: String, counter: UInt)
}
