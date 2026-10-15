package com.desky.shared.domain

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * Single source of truth for the app's drive counter (pairing/auth).
 *
 * Every counter-bearing send app-wide allocates here — the drive-loop tick,
 * the immediate e-stop frame (both via `RobotControlViewModel`), and the
 * signed `UNBOND` (via [ForgetDeviceUseCase]) — so [next] always exceeds the
 * robot's `lastCounter`. The robot lags or equals the app, never leads: each
 * allocation is `lastSent + 1` from the live in-memory era, never from the
 * Room watermark (which is persisted only every [COUNTER_PERSIST_EVERY]
 * allocations and therefore stale in between).
 *
 * Era: one device at a time. [ensure] adopts a new era when the device id
 * differs (seeding `lastSent` from Room's persisted watermark) and is a
 * no-op otherwise, so a shared holder never rewinds a live session.
 *
 * Threading: internally serialized with a [Mutex]; callers that also need
 * the session mutex (the VM's loop tick vs. immediate e-stop) hold that one
 * outside — lock order is always session-then-holder, never the reverse.
 */
class DriveCounter(
    private val deviceRepo: DeviceRepository,
) {
    private val mutex = Mutex()
    private var deviceId: String? = null
    private var lastSent: UInt = 0u
    private var sincePersist: Int = 0

    /**
     * Adopt/reset the era when [id] differs (`lastSent = starting`,
     * persist count cleared); no-op for the current era. Call with Room's
     * persisted `lastCounter` as [starting] — it only takes effect on a
     * fresh era, never mid-session.
     */
    suspend fun ensure(id: String, starting: UInt) {
        mutex.withLock {
            if (deviceId != id) {
                deviceId = id
                lastSent = starting
                sincePersist = 0
            }
        }
    }

    /**
     * Allocate the next counter (`lastSent + 1`), persisting to Room every
     * [COUNTER_PERSIST_EVERY]th allocation. Null when there is no era yet
     * (caller skips the send) or on wrap (`next == 0u` — 0 is never valid
     * on the wire; practically unreachable at 2^32 sends, fail-closed).
     */
    suspend fun next(): UInt? {
        mutex.withLock {
            val id = deviceId ?: return null
            val allocated = lastSent + 1u
            if (allocated == 0u) return null
            lastSent = allocated
            sincePersist++
            if (sincePersist % COUNTER_PERSIST_EVERY == 0) {
                deviceRepo.updateCounter(id, allocated)
            }
            return allocated
        }
    }

    /** Clear the era (unpair): the next [ensure] starts fresh. */
    suspend fun reset() {
        mutex.withLock {
            deviceId = null
            lastSent = 0u
            sincePersist = 0
        }
    }

    /** Last allocated counter, or null when there is no era. */
    suspend fun current(): UInt? {
        mutex.withLock {
            if (deviceId == null) return null
            return lastSent
        }
    }

    companion object {
        /** Room counter persist cadence: every N allocations (mirrors firmware A1). */
        const val COUNTER_PERSIST_EVERY = 128
    }
}
