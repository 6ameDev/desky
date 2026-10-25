package com.desky.shared.domain

import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class FromRobotCounterTest {

    @Test
    fun noPeerReturnsNull() = runTest {
        assertNull(FromRobotCounter(FakeDeviceRepository()).current())
    }

    @Test
    fun ensureAdoptsPeerThenNeverRewinds() = runTest {
        val holder = FromRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-desky.local", 41u)
        assertEquals(41u, holder.current())

        // Mid-session re-seed (e.g. a Room row read): ignored — live
        // receive state never rewinds.
        holder.ensure("desky-desky.local", 99u)
        assertEquals(41u, holder.current())
    }

    @Test
    fun peerSwitchAdoptsNewWatermark() = runTest {
        val holder = FromRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-a", 41u)
        assertEquals(41u, holder.current())

        holder.ensure("desky-b", 7u)
        assertEquals(7u, holder.current())
    }

    @Test
    fun consumeAdvancesAndPersistsPerPeer() = runTest {
        val repo = FakeDeviceRepository(
            PairedDevice(id = "desky-desky.local", ip = "desky.local", authToken = "psk-v1"),
        )
        val holder = FromRobotCounter(repo)
        holder.ensure("desky-desky.local", 0u)

        holder.consume(1u)
        assertEquals(1u, holder.current())
        holder.consume(2u)
        assertEquals(2u, holder.current())

        assertEquals(
            listOf("desky-desky.local" to 1u, "desky-desky.local" to 2u),
            repo.robotCounterSaves,
            "every accepted frame persists under the peer id",
        )
        assertEquals(2u, repo.observePaired().first()!!.lastRobotCounter)
    }

    @Test
    fun consumeWithoutPeerIsNoOp() = runTest {
        val repo = FakeDeviceRepository()
        FromRobotCounter(repo).consume(1u)
        assertEquals(emptyList(), repo.robotCounterSaves)
    }

    @Test
    fun resetClearsPeerToBaseline() = runTest {
        val holder = FromRobotCounter(FakeDeviceRepository())
        holder.ensure("desky-desky.local", 41u)
        assertEquals(41u, holder.current())

        holder.reset()
        assertNull(holder.current())

        // Same peer id re-pairs fresh: adoption (not a no-op) at 0.
        holder.ensure("desky-desky.local", 0u)
        assertEquals(0u, holder.current())
    }

    @Test
    fun isFreshMirrorsFirmwareIdiom() {
        assertTrue(FromRobotCounter.isFresh(0u, 1u))
        assertTrue(FromRobotCounter.isFresh(41u, 42u))
        assertTrue(FromRobotCounter.isFresh(0xFFFFFFFFu, 0u), "wrap is fresh")
        assertTrue(FromRobotCounter.isFresh(0xFFFFFFFFu, 1u))
        assertTrue(FromRobotCounter.isFresh(0u, 0x7FFFFFFFu))
        assertFalse(FromRobotCounter.isFresh(7u, 7u), "equal is stale (burst duplicate)")
        assertFalse(FromRobotCounter.isFresh(10u, 9u), "older is stale")
        assertFalse(FromRobotCounter.isFresh(1u, 0u))
        assertFalse(FromRobotCounter.isFresh(0u, 0x80000000u), "half-range ambiguous")
        assertFalse(FromRobotCounter.isFresh(0u, 0x80000001u))
    }
}
