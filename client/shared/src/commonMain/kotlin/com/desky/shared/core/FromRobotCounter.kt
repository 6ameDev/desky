package com.desky.shared.core

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * Single source of truth for the robot-to-app receive counter (per-link
 * counter domains).
 *
 * The robot numbers its own sends from an independent 32-bit sequence
 * starting at 1 per pair (today only `DISCONNECT` evictions travel
 * robot -> app); the app's sends number from the separate send domain
 * ([ToRobotCounter]). The two sequences advance independently, so a busy
 * drive loop can never starve an eviction stamp (the bench bug the split
 * fixes: evictions once verified stale against the ever-advancing send
 * counter and 0/3 arrived).
 *
 * Keyed by peer id (single owner `desky-$ROBOT_HOST` today): [ensure]
 * adopts a new peer when the device id differs (seeding `lastSeen` from
 * Room's persisted watermark) and is a no-op otherwise, so a shared holder
 * never rewinds live state. Default 0 per pair (fresh Room row + [reset]
 * both land on 0, so the robot's first frame at counter 1 is fresh).
 * Evictions are rare, so [consume] persists to Room on every accepted
 * frame (direct persist, no cadence) keyed by the peer id.
 *
 * Lifecycle: [reset] on pair success ([PairDeviceWithCodeUseCase]), on
 * refresh switch ([PairRefreshUseCase]), and on local forget/unpair
 * ([ForgetDeviceUseCase], [ObserveRemoteDisconnectUseCase], and the
 * control ViewModel's unpair branch).
 *
 * Threading: internally serialized with a [Mutex].
 */
class FromRobotCounter(
    private val deviceRepo: DeviceRepository,
) {
    private val mutex = Mutex()
    private var deviceId: String? = null
    private var lastSeen: UInt = 0u

    /**
     * Adopt/reset the peer when [id] differs (`lastSeen = starting`);
     * no-op for the current peer (live receive state is never rewound
     * mid-session — call with Room's persisted `lastRobotCounter`).
     * Explicit [reset] (not a re-ensure) owns the same-peer re-pair:
     * the peer id is stable across pairs, so only a reset returns the
     * sequence to the 0 baseline.
     */
    suspend fun ensure(id: String, starting: UInt) {
        mutex.withLock {
            if (deviceId != id) {
                deviceId = id
                lastSeen = starting
            }
        }
    }

    /**
     * Consume an accepted robot counter: advance `lastSeen` and persist it
     * to Room under the peer id. The caller verified wrap-aware freshness
     * ([isFresh]) and the frame tag BEFORE calling — this never rejects.
     * No-op when there is no peer yet (caller always [ensure]s first).
     * Snapshot-outside pattern like `ToRobotCounter.next`: snapshot under
     * the mutex, Room write outside; errors propagate.
     */
    suspend fun consume(counter: UInt) {
        val id = mutex.withLock {
            val id = deviceId ?: return
            lastSeen = counter
            id
        }
        deviceRepo.updateRobotCounter(id, counter)
    }

    /** Clear the peer (unpair): the next [ensure] starts fresh at 0. */
    suspend fun reset() {
        mutex.withLock {
            deviceId = null
            lastSeen = 0u
        }
    }

    /** Last accepted robot counter, or null when there is no peer. */
    suspend fun current(): UInt? {
        mutex.withLock {
            if (deviceId == null) return null
            return lastSeen
        }
    }

    companion object {
        /**
         * Wrap-aware strictly-greater for the 32-bit receive counter.
         * Unsigned subtraction: wrap-safe across the 2^32 rollover; a diff
         * of exactly half the range is rejected as ambiguous. Exact mirror
         * of firmware `udpstatus::isFreshCounter` (same idiom as the send
         * domain — burst duplicates of an accepted counter read stale,
         * which keeps eviction handling idempotent with no special-casing).
         */
        fun isFresh(lastSeen: UInt, got: UInt): Boolean =
            got != lastSeen && (got - lastSeen) < 0x80000000u
    }
}
