package com.desky.shared.domain

import com.desky.shared.presentation.FakeDeviceRepository
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

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
    fun resetClearsEra() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", 0u)
        assertEquals(1u, holder.next())

        holder.reset()
        assertNull(holder.next())
        assertNull(holder.current())

        holder.ensure("desky-1", 0u)
        assertEquals(1u, holder.next())
    }

    @Test
    fun wrapFailsClosed() = runTest {
        val holder = DriveCounter(FakeDeviceRepository())
        holder.ensure("desky-1", UInt.MAX_VALUE)

        assertNull(holder.next(), "0 is never valid on the wire")
        assertEquals(UInt.MAX_VALUE, holder.current())
    }
}
