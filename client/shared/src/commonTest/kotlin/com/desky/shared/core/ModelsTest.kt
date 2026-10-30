package com.desky.shared.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class ModelsTest {

    @Test
    fun robotModeBitsRoundTrip() {
        assertEquals(listOf(0, 1, 2, MAX_MODE_BITS), RobotMode.entries.map { it.toBits() })
        assertEquals(RobotMode.MANUAL, RobotMode.fromBits(0))
        assertEquals(RobotMode.AUTONOMOUS, RobotMode.fromBits(1))
        assertEquals(RobotMode.LOW_POWER, RobotMode.fromBits(2))
        assertEquals(RobotMode.EMERGENCY, RobotMode.fromBits(MAX_MODE_BITS))
        for (mode in RobotMode.entries) {
            assertEquals(mode, RobotMode.fromBits(mode.toBits()))
        }
    }

    @Test
    fun robotModeFromBitsRejectsOutOfRange() {
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(-1) }
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(FIRST_INVALID_MODE_BITS) }
        assertFailsWith<IllegalArgumentException> { RobotMode.fromBits(FAR_INVALID_MODE_BITS) }
    }

    @Test
    fun controlCommandValidatesRanges() {
        ControlCommand(RobotMode.MANUAL, -1f, 1f, flags = 255)
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, JUST_ABOVE_UNIT_MAX, 0f) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, JUST_BELOW_UNIT_MIN) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, 0f, flags = 256) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, 0f, 0f, flags = -1) }
        assertFailsWith<IllegalArgumentException> { ControlCommand(RobotMode.MANUAL, Float.NaN, 0f) }
    }

    @Test
    fun telemetryValidatesDistance() {
        Telemetry(0f, 0f, 0, false, false, RobotMode.MANUAL)
        Telemetry(0f, 0f, MAX_DISTANCE_MM, false, false, RobotMode.MANUAL)
        assertFailsWith<IllegalArgumentException> { Telemetry(0f, 0f, -1, false, false, RobotMode.MANUAL) }
        assertFailsWith<IllegalArgumentException> { Telemetry(0f, 0f, JUST_ABOVE_MAX_DISTANCE_MM, false, false, RobotMode.MANUAL) }
    }

    @Test
    fun pairedDeviceDefaults() {
        val d = PairedDevice(id = "robot-1", ip = ROBOT_HOST, authToken = "token")
        assertEquals(DEFAULT_UDP_PORT, d.port)
        assertEquals(DEFAULT_UDP_PORT, d.port)
        assertEquals(ROBOT_HOST, d.alias)
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

    private companion object {
        const val MAX_MODE_BITS = 3
        const val FIRST_INVALID_MODE_BITS = 4
        const val FAR_INVALID_MODE_BITS = 255
        const val JUST_ABOVE_UNIT_MAX = 1.1f
        const val JUST_BELOW_UNIT_MIN = -1.1f
        const val MAX_DISTANCE_MM = 65535
        const val JUST_ABOVE_MAX_DISTANCE_MM = 65536
    }
}
