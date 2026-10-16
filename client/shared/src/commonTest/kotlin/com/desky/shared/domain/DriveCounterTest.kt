package com.desky.shared.domain

import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class DriveCounterTest {

    @Test
    fun nextWithoutEraReturnsNull() = runTest {
        assertNull(DriveCounter(FakeDeviceRepository()).next())
        assertNull(DriveCounter(FakeDeviceRepository()).current())
    }

    @Test
    fun allocatesMonotonicallyFromStarting() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 100u)

        assertEquals(101u, holder.next())
        assertEquals(102u, holder.next())
        assertEquals(103u, holder.next())
        assertEquals(103u, holder.current())
    }

    @Test
    fun ensureIsNoOpForCurrentEra() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u)
        assertEquals(1u, holder.next())
        assertEquals(2u, holder.next())
        // Stale watermark must never rewind a live era.
        holder.ensure("desky-1", 50u)
        assertEquals(3u, holder.next())
        assertEquals(3u, holder.current())
    }

    @Test
    fun eraSwitchResetsSequence() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 0u)
        assertEquals(1u, holder.next())
        assertEquals(2u, holder.next())

        holder.ensure("desky-b", 100u)
        assertEquals(101u, holder.next())
        assertEquals(101u, holder.current())
    }

    @Test
    fun persistsEvery128thAllocation() = runTest {
        val repo = FakeDeviceRepository()
        val holder = DriveCounter(repo)
        holder.ensure("desky-1", 0u)

        repeat(127) { holder.next() }
        assertEquals(emptyList(), repo.counterSaves)
        assertEquals(128u, holder.next())
        assertEquals(listOf("desky-1" to 128u), repo.counterSaves)

        repeat(127) { holder.next() }
        assertEquals(256u, holder.next())
        assertEquals(listOf("desky-1" to 128u, "desky-1" to 256u), repo.counterSaves)
    }

    @Test
    fun persistsAtNegotiatedCadence() = runTest {
        val repo = FakeDeviceRepository()
        val holder = DriveCounter(repo)
        holder.ensure("desky-1", 0u, persistEvery = 16)
        assertEquals(16, holder.persistEvery())

        repeat(15) { holder.next() }
        assertEquals(emptyList(), repo.counterSaves)
        assertEquals(16u, holder.next())
        assertEquals(listOf("desky-1" to 16u), repo.counterSaves)

        repeat(15) { holder.next() }
        assertEquals(32u, holder.next())
        assertEquals(listOf("desky-1" to 16u, "desky-1" to 32u), repo.counterSaves)
    }

    @Test
    fun cadenceAcceptedOnceAtPairOnly() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, persistEvery = 16)
        assertEquals(1u, holder.next())
        // Same era re-seeded (e.g. a Room row read mid-session): starting
        // AND cadence are both ignored — the live era never rewinds.
        holder.ensure("desky-1", 50u, persistEvery = 64)
        assertEquals(2u, holder.next())
        assertEquals(16, holder.persistEvery())
    }

    @Test
    fun cadenceGuardFallsBackTo128() = runTest {
        for (bad in listOf(0, 1, 7, 1025, -1)) {
            val holder = DriveCounter(FakeDeviceRepository())
            holder.ensure("desky-1", 0u, persistEvery = bad)
            assertEquals(128, holder.persistEvery(), "persistEvery=$bad")
        }
        // Boundary values pass through.
        for (good in listOf(8, 64, 1024)) {
            val holder = DriveCounter(FakeDeviceRepository())
            holder.ensure("desky-1", 0u, persistEvery = good)
            assertEquals(good, holder.persistEvery(), "persistEvery=$good")
        }
    }

    @Test
    fun eraSwitchAdoptsNewCadence() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 0u, persistEvery = 16)
        assertEquals(16, holder.persistEvery())

        holder.ensure("desky-b", 100u, persistEvery = 64)
        assertEquals(64, holder.persistEvery())
        assertEquals(101u, holder.next())
    }

    @Test
    fun flushPersistsDirtyOnly() = runTest {
        // No era: no-op, nothing persisted.
        val empty = DriveCounter(FakeDeviceRepository())
        empty.flush()
        // Clean era (no allocs yet): no-op.
        val repo = FakeDeviceRepository()
        val holder = DriveCounter(repo)
        holder.ensure("desky-1", 0u)
        holder.flush()
        assertEquals(emptyList(), repo.counterSaves)

        // Dirty era: persists lastSent, then idempotent.
        assertEquals(1u, holder.next())
        assertEquals(2u, holder.next())
        holder.flush()
        assertEquals(listOf("desky-1" to 2u), repo.counterSaves)
        holder.flush()
        assertEquals(listOf("desky-1" to 2u), repo.counterSaves)

        // More allocs dirty again: flush persists the new watermark.
        assertEquals(3u, holder.next())
        holder.flush()
        assertEquals(listOf("desky-1" to 2u, "desky-1" to 3u), repo.counterSaves)
    }

    @Test
    fun resetClearsEra() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, persistEvery = 16)
        assertEquals(1u, holder.next())

        holder.reset()
        assertNull(holder.next())
        assertNull(holder.current())
        assertNull(holder.persistEvery())

        holder.ensure("desky-1", 0u)
        assertEquals(1u, holder.next())
        assertEquals(128, holder.persistEvery())
    }

    /**
     * Tier-2 exhaustion gate: allocation up to gate-1 succeeds, the gate
     * value itself fails closed (null + [DriveCounter.isExhausted] latched,
     * never allocating 0 or the wrap-adjacent half-range-ambiguous region),
     * and [DriveCounter.flush] still persists the last good counter.
     */
    @Test
    fun exhaustionGateFailsClosed() = runTest {
        val repo = FakeDeviceRepository()
        val holder = DriveCounter(repo)
        holder.ensure("desky-1", DriveCounter.EXHAUSTION_GATE - 2u)

        assertEquals(DriveCounter.EXHAUSTION_GATE - 1u, holder.next())
        assertFalse(holder.isExhausted())

        assertNull(holder.next(), "gate value is never allocated")
        assertTrue(holder.isExhausted())
        assertNull(holder.next(), "latch sticks: no allocation after exhaustion")
        assertEquals(DriveCounter.EXHAUSTION_GATE - 1u, holder.current())

        holder.flush()
        assertEquals(
            listOf("desky-1" to (DriveCounter.EXHAUSTION_GATE - 1u)),
            repo.counterSaves,
            "flush persists the last good counter, not the gate",
        )

        // Explicit re-pair (fresh era) clears the latch.
        holder.reset()
        assertFalse(holder.isExhausted())
    }

    @Test
    fun wrapFailsClosed() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", UInt.MAX_VALUE)

        assertNull(holder.next(), "0 is never valid on the wire")
        assertEquals(UInt.MAX_VALUE, holder.current())
    }

    /**
     * Tier-1 rekey era: adopted once on a fresh era (Room row's era), never
     * rewound mid-session; [DriveCounter.completeRekey] rotates to the next
     * era with the counter back at the bond baseline (next wire packet is
     * 1); [DriveCounter.reset] clears back to 0.
     */
    @Test
    fun eraAdoptedOnceThenRotated() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 50u, era = 7u)
        assertEquals(7u, holder.currentEra())

        assertEquals(51u, holder.next())
        // Mid-session re-seed (e.g. a Room row read): era AND watermark both ignored.
        holder.ensure("desky-1", 99u, era = 9u)
        assertEquals(52u, holder.next())
        assertEquals(7u, holder.currentEra())

        holder.completeRekey(8u)
        assertEquals(8u, holder.currentEra())
        assertEquals(0u, holder.current(), "baseline persisted; first post-rekey drive is 1")
        assertEquals(1u, holder.next())
        assertEquals(1u, holder.current())
    }

    @Test
    fun eraSwitchAdoptsNewEra() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 0u, era = 3u)
        assertEquals(1u, holder.next())

        holder.ensure("desky-b", 100u, era = 4u)
        assertEquals(4u, holder.currentEra())
        assertEquals(101u, holder.next())
    }

    @Test
    fun completeRekeyClearsExhaustionLatch() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", DriveCounter.EXHAUSTION_GATE - 2u)
        assertEquals(DriveCounter.EXHAUSTION_GATE - 1u, holder.next())
        assertNull(holder.next(), "gate value is never allocated")
        assertTrue(holder.isExhausted())

        holder.completeRekey(1u)
        assertFalse(holder.isExhausted(), "rotation clears the fail-closed latch")
        assertEquals(1u, holder.next(), "post-rekey drive resumes at counter 1")
    }

    @Test
    fun resetClearsEraToZero() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, era = 5u)
        assertEquals(1u, holder.next())

        holder.reset()
        assertNull(holder.currentEra())

        holder.ensure("desky-1", 0u)
        assertEquals(0u, holder.currentEra())
    }

    /**
     * Proactive trigger: [DriveCounter.needsRekey] is false far from the
     * gate and true once `lastSent` reaches `GATE - REKEY_MARGIN` — the
     * silent rotation window (no user action; Tier-2 repair copy owns the
     * gate itself).
     */
    @Test
    fun needsRekeyFiresAtMargin() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        assertFalse(holder.needsRekey(), "no era yet: no trigger")

        holder.ensure("desky-1", 0u)
        assertFalse(holder.needsRekey(), "fresh bond: no trigger")

        val near = DriveCounter(FakeDeviceRepository())
        near.ensure("desky-1", DriveCounter.EXHAUSTION_GATE - DriveCounter.REKEY_MARGIN - 2u)
        assertFalse(near.needsRekey(), "one frame short of the margin: no trigger")
        near.next()
        assertFalse(near.needsRekey(), "margin edge not yet reached")
        near.next()
        assertTrue(near.needsRekey(), "at GATE - MARGIN: rotate now")
    }
}
