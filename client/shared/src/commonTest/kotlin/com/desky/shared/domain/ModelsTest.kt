package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class ModelsTest {

    @Test
    fun robotModeBitsRoundTrip() {
        assertEquals(listOf(0, 1, 2, 3), RobotMode.entries.map { it.toBits() })
        assertEquals(RobotMode.MANUAL, RobotMode.fromBits(0))
        assertEquals(RobotMode.AUTONOMOUS, RobotMode.fromBits(1))
        assertEquals(RobotMode.LOW_POWER, RobotMode.fromBits(2))
        assertEquals(RobotMode.EMERGENCY, RobotMode.fromBits(3))
        for (mode in RobotMode.entries) {
            assertEquals(mode, RobotMode.fromBits(mode.toBits()))
        }
    }

    @Test
    fun robotModeFromBitsRejectsOutOfRange() {
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(-1) }
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(4) }
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(255) }
    }

    @Test
    fun controlCommandValidatesRanges() {
        ControlCommand(RobotMode.MANUAL, -1f, 1f, flags = 255)
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 1.1f, 0f) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, -1.1f) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, 0f, flags = 256) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, 0f, flags = -1) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, Float.NaN, 0f) }
    }

    @Test
    fun telemetryValidatesDistance() {
        Telemetry(0f, 0f, 0, false, false, RobotMode.MANUAL)
        Telemetry(0f, 0f, 65535, false, false, RobotMode.MANUAL)
        assertFailsWith<IllegalArgumentException> { Telemetry(0f, 0f, -1, false, false, RobotMode.MANUAL) }
        assertFailsWith<IllegalArgumentException> { Telemetry(0f, 0f, 65536, false, false, RobotMode.MANUAL) }
    }

    @Test
    fun pairedDeviceDefaults() {
        val d = PairedDevice(id = "robot-1", ip = "192.168.4.1", authToken = "token")
        assertEquals(3333, d.port)
        assertEquals(DEFAULT_UDP_PORT, d.port)
        assertEquals("192.168.4.1", d.alias)
    }

    @Test
    fun robotConfigDefaultsAndValidation() {
        val c = RobotConfig(speedLimit = 0.8f)
        assertEquals(0, c.ledProfile)
        assertEquals(0f, c.pidP)
        assertEquals(0f, c.pidI)
        assertEquals(0f, c.pidD)
        assertEquals("", c.wifiSsid)
        RobotConfig(speedLimit = 0f)
        RobotConfig(speedLimit = 1f)
        assertFailsWith<IllegalArgumentException> { RobotConfig(speedLimit = -0.1f) }
        assertFailsWith<IllegalArgumentException> { RobotConfig(speedLimit = 1.1f) }
    }
}
