package com.desky.shared.core

import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ToRobotCounterTest {

    private companion object {
        const val SMALL_TEST_CADENCE = 16
        const val MID_TEST_CADENCE = 64
        const val MIN_PERSIST_CADENCE = 8
        const val MAX_PERSIST_CADENCE = 1024
        const val BELOW_MIN_CADENCE = 7
        const val ABOVE_MAX_CADENCE = 1025
        const val POST_PERSIST_EXTRA_ALLOCS = 4
        const val CADENCE_OVERHANG = 5
    }

    @Test
    fun nextWithoutEraReturnsNull() = runTest {
        assertNull(ToRobotCounter(FakeDeviceRepository()).next())
        assertNull(ToRobotCounter(FakeDeviceRepository()).current())
    }

    @Test
    fun allocatesMonotonicallyFromStarting() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 100u)

        assertEquals(101u, holder.next())
        assertEquals(102u, holder.next())
        assertEquals(103u, holder.next())
        assertEquals(103u, holder.current())
    }

    @Test
    fun ensureIsNoOpForCurrentEra() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
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
        val holder = ToRobotCounter(FakeDeviceRepository())
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
        val holder = ToRobotCounter(repo)
        holder.ensure("desky-1", 0u)

        repeat(ToRobotCounter.COUNTER_PERSIST_EVERY - 1) { holder.next() }
        assertEquals(emptyList(), repo.counterSaves)
        // Write-ahead: the 128th allocation persists 128 + 128, not 128.
        assertEquals(128u, holder.next())
        assertEquals(listOf("desky-1" to 256u), repo.counterSaves)

        repeat(ToRobotCounter.COUNTER_PERSIST_EVERY - 1) { holder.next() }
        assertEquals(256u, holder.next())
        assertEquals(listOf("desky-1" to 256u, "desky-1" to 384u), repo.counterSaves)
    }

    @Test
    fun persistsAtNegotiatedCadence() = runTest {
        val repo = FakeDeviceRepository()
        val holder = ToRobotCounter(repo)
        holder.ensure("desky-1", 0u, persistEvery = SMALL_TEST_CADENCE)
        assertEquals(SMALL_TEST_CADENCE, holder.persistEvery())

        repeat(SMALL_TEST_CADENCE - 1) { holder.next() }
        assertEquals(emptyList(), repo.counterSaves)
        // Write-ahead: allocation 16 persists 16 + 16.
        assertEquals(16u, holder.next())
        assertEquals(listOf("desky-1" to 32u), repo.counterSaves)

        repeat(SMALL_TEST_CADENCE - 1) { holder.next() }
        assertEquals(32u, holder.next())
        assertEquals(listOf("desky-1" to 32u, "desky-1" to 48u), repo.counterSaves)
    }

    @Test
    fun cadenceAcceptedOnceAtPairOnly() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, persistEvery = SMALL_TEST_CADENCE)
        assertEquals(1u, holder.next())
        // Same era re-seeded (e.g. a Room row read mid-session): starting
        // AND cadence are both ignored — the live era never rewinds.
        holder.ensure("desky-1", 50u, persistEvery = MID_TEST_CADENCE)
        assertEquals(2u, holder.next())
        assertEquals(SMALL_TEST_CADENCE, holder.persistEvery())
    }

    @Test
    fun cadenceGuardFallsBackTo128() = runTest {
        for (bad in listOf(0, 1, BELOW_MIN_CADENCE, ABOVE_MAX_CADENCE, -1)) {
            val holder = ToRobotCounter(FakeDeviceRepository())
            holder.ensure("desky-1", 0u, persistEvery = bad)
            assertEquals(ToRobotCounter.COUNTER_PERSIST_EVERY, holder.persistEvery(), "persistEvery=$bad")
        }
        // Boundary values pass through.
        for (good in listOf(MIN_PERSIST_CADENCE, MID_TEST_CADENCE, MAX_PERSIST_CADENCE)) {
            val holder = ToRobotCounter(FakeDeviceRepository())
            holder.ensure("desky-1", 0u, persistEvery = good)
            assertEquals(good, holder.persistEvery(), "persistEvery=$good")
        }
    }

    @Test
    fun eraSwitchAdoptsNewCadence() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 0u, persistEvery = SMALL_TEST_CADENCE)
        assertEquals(SMALL_TEST_CADENCE, holder.persistEvery())

        holder.ensure("desky-b", 100u, persistEvery = MID_TEST_CADENCE)
        assertEquals(MID_TEST_CADENCE, holder.persistEvery())
        assertEquals(101u, holder.next())
    }

    @Test
    fun flushPersistsDirtyOnly() = runTest {
        // No era: no-op, nothing persisted.
        val empty = ToRobotCounter(FakeDeviceRepository())
        empty.flush()
        // Clean era (no allocs yet): no-op.
        val repo = FakeDeviceRepository()
        val holder = ToRobotCounter(repo)
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
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, persistEvery = SMALL_TEST_CADENCE)
        assertEquals(1u, holder.next())

        holder.reset()
        assertNull(holder.next())
        assertNull(holder.current())
        assertNull(holder.persistEvery())

        holder.ensure("desky-1", 0u)
        assertEquals(1u, holder.next())
        assertEquals(ToRobotCounter.COUNTER_PERSIST_EVERY, holder.persistEvery())
    }

    /**
     * Tier-2 exhaustion gate: allocation up to gate-1 succeeds, the gate
     * value itself fails closed (null + [ToRobotCounter.isExhausted] latched,
     * never allocating 0 or the wrap-adjacent half-range-ambiguous region),
     * and [ToRobotCounter.flush] still persists the last good counter.
     */
    @Test
    fun exhaustionGateFailsClosed() = runTest {
        val repo = FakeDeviceRepository()
        val holder = ToRobotCounter(repo)
        holder.ensure("desky-1", ToRobotCounter.EXHAUSTION_GATE - 2u)

        assertEquals(ToRobotCounter.EXHAUSTION_GATE - 1u, holder.next())
        assertFalse(holder.isExhausted())

        assertNull(holder.next(), "gate value is never allocated")
        assertTrue(holder.isExhausted())
        assertNull(holder.next(), "latch sticks: no allocation after exhaustion")
        assertEquals(ToRobotCounter.EXHAUSTION_GATE - 1u, holder.current())

        holder.flush()
        assertEquals(
            listOf("desky-1" to (ToRobotCounter.EXHAUSTION_GATE - 1u)),
            repo.counterSaves,
            "flush persists the last good counter, not the gate",
        )

        // Explicit re-pair (fresh era) clears the latch.
        holder.reset()
        assertFalse(holder.isExhausted())
    }

    @Test
    fun wrapFailsClosed() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", UInt.MAX_VALUE)

        assertNull(holder.next(), "0 is never valid on the wire")
        assertEquals(UInt.MAX_VALUE, holder.current())
    }

    /**
     * Tier-1 pair-refresh era: adopted once on a fresh era (Room row's era), never
     * rewound mid-session; [ToRobotCounter.completePairRefresh] rotates to the next
     * era with the counter back at the pair baseline (next wire packet is
     * 1); [ToRobotCounter.reset] clears back to 0.
     */
    @Test
    fun eraAdoptedOnceThenRotated() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 50u, era = 7u)
        assertEquals(7u, holder.currentEra())

        assertEquals(51u, holder.next())
        // Mid-session re-seed (e.g. a Room row read): era AND watermark both ignored.
        holder.ensure("desky-1", 99u, era = 9u)
        assertEquals(52u, holder.next())
        assertEquals(7u, holder.currentEra())

        holder.completePairRefresh(8u)
        assertEquals(8u, holder.currentEra())
        assertEquals(0u, holder.current(), "baseline persisted; first post-pair-refresh drive is 1")
        assertEquals(1u, holder.next())
        assertEquals(1u, holder.current())
    }

    @Test
    fun eraSwitchAdoptsNewEra() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 0u, era = 3u)
        assertEquals(1u, holder.next())

        holder.ensure("desky-b", 100u, era = 4u)
        assertEquals(4u, holder.currentEra())
        assertEquals(101u, holder.next())
    }

    @Test
    fun completePairRefreshClearsExhaustionLatch() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", ToRobotCounter.EXHAUSTION_GATE - 2u)
        assertEquals(ToRobotCounter.EXHAUSTION_GATE - 1u, holder.next())
        assertNull(holder.next(), "gate value is never allocated")
        assertTrue(holder.isExhausted())

        holder.completePairRefresh(1u)
        assertFalse(holder.isExhausted(), "rotation clears the fail-closed latch")
        assertEquals(1u, holder.next(), "post-pair-refresh drive resumes at counter 1")
    }

    @Test
    fun resetClearsEraToZero() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u, era = 5u)
        assertEquals(1u, holder.next())

        holder.reset()
        assertNull(holder.currentEra())

        holder.ensure("desky-1", 0u)
        assertEquals(0u, holder.currentEra())
    }

    /**
     * Proactive trigger: [ToRobotCounter.needsPairRefresh] is false far from the
     * gate and true once `lastSent` reaches `GATE - PAIR_REFRESH_MARGIN` — the
     * silent rotation window (no user action; Tier-2 repair copy owns the
     * gate itself).
     */
    @Test
    fun needsPairRefreshFiresAtMargin() = runTest {
        val holder = ToRobotCounter(FakeDeviceRepository())
        assertFalse(holder.needsPairRefresh(), "no era yet: no trigger")

        holder.ensure("desky-1", 0u)
        assertFalse(holder.needsPairRefresh(), "fresh pair: no trigger")

        val near = ToRobotCounter(FakeDeviceRepository())
        near.ensure("desky-1", ToRobotCounter.EXHAUSTION_GATE - ToRobotCounter.PAIR_REFRESH_MARGIN - 2u)
        assertFalse(near.needsPairRefresh(), "one frame short of the margin: no trigger")
        near.next()
        assertFalse(near.needsPairRefresh(), "margin edge not yet reached")
        near.next()
        assertTrue(near.needsPairRefresh(), "at GATE - MARGIN: rotate now")
    }

    /**
     * Write-ahead headroom uses the live negotiated cadence, not hardcoded
     * 128: allocation N persists N + livePersistEvery.
     */
    @Test
    fun writeAheadUsesLivePersistEvery() = runTest {
        for (cadence in listOf(MIN_PERSIST_CADENCE, SMALL_TEST_CADENCE, MID_TEST_CADENCE, ToRobotCounter.COUNTER_PERSIST_EVERY, MAX_PERSIST_CADENCE)) {
            val repo = FakeDeviceRepository()
            val holder = ToRobotCounter(repo)
            holder.ensure("desky-1", 0u, persistEvery = cadence)
            repeat(cadence - 1) { holder.next() }
            assertEquals(cadence.toUInt(), holder.next(), "cadence=$cadence")
            assertEquals(
                listOf("desky-1" to (cadence.toUInt() + cadence.toUInt())),
                repo.counterSaves,
                "persist = lastSent + live persistEvery (cadence=$cadence)",
            )
        }
    }

    /**
     * Kill-sim: restart from the write-ahead watermark allocates disk + 1,
     * strictly above every counter the robot could have seen — zero drops,
     * waste bounded by one cadence.
     */
    @Test
    fun killSimRestartHasZeroDrops() = runTest {
        for (cadence in listOf(SMALL_TEST_CADENCE, ToRobotCounter.COUNTER_PERSIST_EVERY, MAX_PERSIST_CADENCE)) {
            val repo = FakeDeviceRepository()
            val holder = ToRobotCounter(repo)
            holder.ensure("desky-1", 0u, persistEvery = cadence)
            // Drive past the first cadence persist plus a few unpersisted frames.
            repeat(cadence + CADENCE_OVERHANG) { holder.next() }
            val disk = repo.counterSaves.last().second
            val robotLastSeen = holder.current()!!
            assertTrue(disk >= robotLastSeen, "cadence=$cadence: disk leads live")

            // Simulate an unclean kill: fresh holder re-adopts the disk watermark.
            val restarted = ToRobotCounter(repo)
            restarted.ensure("desky-1", disk)
            val resumed = restarted.next()
            assertEquals(disk + 1u, resumed, "cadence=$cadence: restart allocates disk+1")
            assertTrue(resumed!! > robotLastSeen, "cadence=$cadence: zero drops")
            assertTrue(resumed - robotLastSeen <= cadence.toUInt() + 1u, "cadence=$cadence: waste <= N+1")
        }
    }

    /**
     * Gate clamp: a write-ahead persist that would reach the gate clamps to
     * GATE-1, never allocates the gate value, and the Tier-2 gate check
     * still owns exhaustion.
     */
    @Test
    fun writeAheadClampsAtGate() = runTest {
        for (cadence in listOf(SMALL_TEST_CADENCE, MAX_PERSIST_CADENCE)) {
            val repo = FakeDeviceRepository()
            val holder = ToRobotCounter(repo)
            // Seed so the first cadence persist lands past the gate.
            val gateMinusOne = ToRobotCounter.EXHAUSTION_GATE - 1u
            val start = gateMinusOne - cadence.toUInt() - 5u
            holder.ensure("desky-1", start, persistEvery = cadence)
            repeat(cadence - 1) { holder.next() }
            val allocated = holder.next()
            assertEquals(start + cadence.toUInt(), allocated)
            assertEquals(
                listOf("desky-1" to gateMinusOne),
                repo.counterSaves,
                "cadence=$cadence: write-ahead clamps to GATE-1",
            )
            assertFalse(holder.isExhausted(), "cadence=$cadence: clamp alone does not exhaust")
            // Drain to the gate: allocation refuses, latch sticks, gate never sent.
            while (holder.next() != null) {
                // Drain live allocations up to GATE-1.
            }
            assertTrue(holder.isExhausted(), "cadence=$cadence: Tier-2 gate still latches")
            assertEquals(gateMinusOne, holder.current())
            assertNull(holder.next(), "cadence=$cadence: latch sticks")
        }
    }

    /**
     * Flush after write-ahead still persists the exact live watermark on
     * graceful paths (collapse-ahead is safe: disk == robot's lastSeen, so
     * restart is still disk+1 with zero drops).
     */
    @Test
    fun flushAfterWriteAheadPersistsExact() = runTest {
        val repo = FakeDeviceRepository()
        val holder = ToRobotCounter(repo)
        holder.ensure("desky-1", 0u, persistEvery = SMALL_TEST_CADENCE)
        repeat(SMALL_TEST_CADENCE) { holder.next() }
        assertEquals(listOf("desky-1" to 32u), repo.counterSaves, "write-ahead first")
        repeat(POST_PERSIST_EXTRA_ALLOCS) { holder.next() }
        assertEquals(20u, holder.current())
        holder.flush()
        assertEquals(
            listOf("desky-1" to 32u, "desky-1" to 20u),
            repo.counterSaves,
            "flush persists the exact live watermark",
        )
    }
}
