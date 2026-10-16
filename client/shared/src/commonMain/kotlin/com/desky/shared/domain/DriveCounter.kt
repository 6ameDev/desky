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
 * Room watermark (which is persisted only every [persistEvery]
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
    private var exhausted: Boolean = false
    private var persistEvery: Int = COUNTER_PERSIST_EVERY
    // Tier-1 rekey era (u16, Room v5 column + NVS "bera", fresh pair = 0).
    // Adopted once per era via [ensure]; rotated silently via [completeRekey].
    private var era: UShort = 0u

    /**
     * Adopt/reset the era when [id] differs (`lastSent = starting`,
     * persist count cleared, [persistEvery] adopted, [era] adopted); no-op
     * for the current era (a live cadence/era is never renegotiated
     * mid-session — both are accepted once at pair only). Call with Room's
     * persisted `lastCounter` as [starting], the row's `persistEvery`, and
     * the row's `era` — all three only take effect on a fresh era, never
     * mid-session. [persistEvery] is sanitized (8..1024 else
     * [COUNTER_PERSIST_EVERY]) at adoption.
     */
    suspend fun ensure(id: String, starting: UInt, persistEvery: Int = COUNTER_PERSIST_EVERY, era: UShort = 0u) {
        mutex.withLock {
            if (deviceId != id) {
                deviceId = id
                lastSent = starting
                sincePersist = 0
                this.persistEvery = sanitizePersistEvery(persistEvery)
                this.era = era
            }
        }
    }

    /**
     * Allocate the next counter (`lastSent + 1`), persisting to Room every
     * [persistEvery]th allocation (the era's negotiated cadence, default
     * [COUNTER_PERSIST_EVERY]). Null when there is no era yet
     * (caller skips the send), at the [EXHAUSTION_GATE] (fail-closed: latches
     * [exhausted], never allocates the gate value or beyond — so 0 and the
     * wrap-adjacent half-range-ambiguous region are unreachable), or on wrap
     * (`next == 0u` — 0 is never valid on the wire; unreachable behind the
     * gate, kept as defense-in-depth).
     */
    suspend fun next(): UInt? {
        val (id, allocated, shouldPersist) = mutex.withLock {
            val id = deviceId ?: return@withLock Triple<String?, UInt?, Boolean>(null, null, false)
            val allocated = lastSent + 1u
            if (allocated == 0u) return@withLock Triple(id, null, false)
            if (allocated >= EXHAUSTION_GATE) {
                exhausted = true
                return@withLock Triple<String?, UInt?, Boolean>(id, null, false)
            }
            lastSent = allocated
            sincePersist++
            Triple(id, allocated, sincePersist % persistEvery == 0)
        }
        if (allocated == null) return null
        if (shouldPersist && id != null) {
            deviceRepo.updateCounter(id, allocated)
        }
        return allocated
    }

    /**
     * Best-effort watermark flush: persist `lastSent` to Room when the era
     * holds unpersisted allocations (`sincePersist != 0`). Room writes cost
     * one extra I/O on graceful paths (background/forget) so a kill loses
     * nothing. Snapshot-outside pattern like [next]: snapshot + reset under
     * the mutex, Room write outside. Idempotent (second call is a no-op);
     * errors propagate — callers decide whether to swallow.
     */
    suspend fun flush() {
        val (id, watermark) = mutex.withLock {
            val id = deviceId ?: return@withLock Pair<String?, UInt?>(null, null)
            if (sincePersist == 0) return@withLock Pair<String?, UInt?>(null, null)
            sincePersist = 0
            Pair(id, lastSent)
        }
        if (id != null && watermark != null) {
            deviceRepo.updateCounter(id, watermark)
        }
    }

    /** Clear the era (unpair): the next [ensure] starts fresh. */
    suspend fun reset() {
        mutex.withLock {
            deviceId = null
            lastSent = 0u
            sincePersist = 0
            exhausted = false
            persistEvery = COUNTER_PERSIST_EVERY
            era = 0u
        }
    }

    /**
     * Tier-1 silent auto-rekey rotation: adopt [newEra] (current era + 1, u16
     * wrap) after a confirmed `REKEY_ACK`. Resets the counter to the bond
     * baseline (`lastSent = 0`, so the next wire packet uses counter 1 —
     * same as a fresh pair), clears the persist count and the [isExhausted]
     * latch, and adopts the new era — all without changing the device id
     * (unlike [ensure]'s fresh-era path). No-op when there is no era yet.
     * Room/vault persistence is the caller's ([RekeyUseCase]) — this only
     * moves the in-memory sequence.
     */
    suspend fun completeRekey(newEra: UShort) {
        mutex.withLock {
            if (deviceId == null) return
            era = newEra
            lastSent = 0u
            sincePersist = 0
            exhausted = false
        }
    }

    /**
     * Proactive rekey trigger: true when the live era has allocated at or
     * beyond `EXHAUSTION_GATE - REKEY_MARGIN` — the VM's silent background
     * rotation should fire now (gapless: ~1000 frames of margin at any
     * rate), instead of waiting for the fail-closed [isExhausted] latch
     * (which needs an explicit re-pair). False with no era. See
     * [RekeyUseCase] + `RobotControlViewModel.maybeRekey` (trigger stub).
     */
    suspend fun needsRekey(): Boolean {
        mutex.withLock {
            if (deviceId == null) return false
            return lastSent >= EXHAUSTION_GATE - REKEY_MARGIN
        }
    }

    /** Current rekey era, or null when there is no era. */
    suspend fun currentEra(): UShort? {
        mutex.withLock {
            if (deviceId == null) return null
            return era
        }
    }

    /**
     * Fail-closed exhaustion latch: true once [next] has refused the
     * [EXHAUSTION_GATE] (re-pair required — the holder never allocates
     * again until [reset]). Mutex read; callers check this to distinguish a
     * null allocation from exhaustion vs. a missing PSK.
     */
    suspend fun isExhausted(): Boolean {
        mutex.withLock {
            return exhausted
        }
    }

    /** Last allocated counter, or null when there is no era. */
    suspend fun current(): UInt? {
        mutex.withLock {
            if (deviceId == null) return null
            return lastSent
        }
    }

    /**
     * The era's negotiated persist cadence (adopted once via [ensure], never
     * renegotiated mid-era), or null when there is no era.
     */
    suspend fun persistEvery(): Int? {
        mutex.withLock {
            if (deviceId == null) return null
            return persistEvery
        }
    }

    companion object {
        /** Room counter persist cadence: every N allocations (mirrors firmware A1). */
        const val COUNTER_PERSIST_EVERY = 128

        /**
         * Fail-closed exhaustion gate (Tier-2): [next] refuses to allocate
         * this value or beyond, latching [isExhausted] so the app surfaces
         * "session expired — please re-pair" instead of sending. 0xFFFFFF00
         * stops 255 frames short of the 2^32 wrap (~2.7 yr at 50 Hz
         * continuous, so the gate itself is ~2.7 yr minus a ~5 s margin at
         * full rate) — well before both the wrap (0 is never valid on the
         * wire) and the half-range-ambiguous region (firmware
         * `isFreshCounter` rejects diffs >= 0x80000000). No protocol change:
         * the gate lives app-side only; the firmware keeps accepting the
         * full wrap-aware range.
         */
        const val EXHAUSTION_GATE: UInt = 0xFFFFFF00u

        /**
         * Tier-1 proactive rekey margin: [needsRekey] fires when `lastSent`
         * reaches `EXHAUSTION_GATE - REKEY_MARGIN`, so the silent era+PSK
         * rotation completes gaplessly before the fail-closed gate. 1000
         * frames is ~20 s at full 50 Hz drive (minutes-to-hours at idle
         * heartbeat) — ample for the 3x 1 s `REKEY_REQ`/`ACK` exchange plus
         * retries, with zero user action.
         */
        const val REKEY_MARGIN: UInt = 1000u
    }
}
