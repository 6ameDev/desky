package com.desky.shared.domain

import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private fun Byte.unsigned(): Int = this.toInt() and 0xFF

private fun xorOf(bytes: ByteArray, len: Int): Int {
    var sum = 0
    for (i in 0 until len) sum = sum xor bytes[i].unsigned()
    return sum
}

class ByteMappingTest {

    @Test
    fun byteToUnitEdgeTable() {
        // coordinator.h-exact edges.
        assertEquals(-1f, byteToUnit(0.toByte()))
        assertEquals(0f, byteToUnit(128.toByte()))
        assertEquals(1f, byteToUnit(255.toByte()))
        // Interior spot checks: (raw - 128) / 127.
        val cases = listOf(
            1 to -127f / 127f,
            64 to -64f / 127f,
            127 to -1f / 127f,
            129 to 1f / 127f,
            192 to 64f / 127f,
            254 to 126f / 127f,
        )
        for ((raw, expected) in cases) {
            assertEquals(expected, byteToUnit(raw.toByte()), 1e-6f, "raw=$raw")
        }
        // Clamp region: every raw below 1 still maps into [-1, 1].
        assertEquals(-1f, byteToUnit(1.toByte()))
    }

    @Test
    fun unitToByteEdges() {
        assertEquals(0, unitToByte(-1f).unsigned())
        assertEquals(128, unitToByte(0f).unsigned())
        assertEquals(255, unitToByte(1f).unsigned())
        // Out-of-range inputs clamp instead of wrapping.
        assertEquals(255, unitToByte(2f).unsigned())
        assertEquals(0, unitToByte(-2f).unsigned())
    }

    @Test
    fun unitByteRoundTripWithinTolerance() {
        // Round-trip error is <= 0.5 LSB on the non-negative side but up to
        // ~1.5 LSB on the negative side: bytes 0 and 1 both decode to -1.0
        // (dead bin at full reverse), and unitToByte truncates toward zero
        // (firmware-exact floor-rounding). 0.012f clears 1.5/127 ~= 0.01182.
        var v = -1f
        while (v <= 1.0001f) {
            val back = byteToUnit(unitToByte(v))
            assertTrue(abs(back - v) <= 0.012f, "v=$v back=$back")
            v += 0.01f
        }
    }

    @Test
    fun fullReverseDeadBinDocumented() {
        // Bytes 0 and 1 both decode to -1.0, so byte 1 normalizes to 0
        // through a unit round trip (firmware-exact behavior).
        assertEquals(-1f, byteToUnit(0.toByte()))
        assertEquals(-1f, byteToUnit(1.toByte()))
        assertEquals(0, unitToByte(byteToUnit(1.toByte())).unsigned())
    }
}

class DegDecidegTest {

    @Test
    fun exactConversions() {
        assertEquals(0.toShort(), degToDecideg(0f))
        assertEquals(123.toShort(), degToDecideg(12.3f))
        assertEquals((-123).toShort(), degToDecideg(-12.3f))
        assertEquals(12.3f, decidegToDeg(123), 1e-6f)
        assertEquals(-12.3f, decidegToDeg(-123), 1e-6f)
    }

    @Test
    fun clampsAtInt16Scale() {
        // udpstatus::degToDecideg clamps to [-32768, +32767] (asymmetric int16).
        assertEquals(32767.toShort(), degToDecideg(5000f))
        assertEquals((-32768).toShort(), degToDecideg(-5000f))
        assertEquals(32767.toShort(), degToDecideg(3276.7f))
        assertEquals((-32768).toShort(), degToDecideg(-3276.8f))
        // Just inside the rails still converts.
        assertEquals(32766.toShort(), degToDecideg(3276.6f))
        assertEquals((-32767).toShort(), degToDecideg(-3276.7f))
    }
}

class StatusFlagsTest {

    @Test
    fun packUnpackAllModesTimesCliffTimesDriving() {
        for (mode in RobotMode.entries) {
            for (cliff in listOf(false, true)) {
                for (driving in listOf(false, true)) {
                    val flags = packStatusFlags(cliff, driving, mode)
                    assertEquals(cliff, statusCliff(flags), "$mode cliff=$cliff driving=$driving")
                    assertEquals(driving, statusDriving(flags), "$mode cliff=$cliff driving=$driving")
                    assertEquals(mode, statusMode(flags), "$mode cliff=$cliff driving=$driving")
                    // Reserved bits 4-7 stay zero.
                    assertEquals(0, flags and 0xF0, "$mode cliff=$cliff driving=$driving")
                    // Exact bit layout: bit0=cliff, bit1=driving, bits2-3=mode.
                    val expected = (if (cliff) 1 else 0) or
                        ((if (driving) 1 else 0) shl 1) or
                        (mode.toBits() shl 2)
                    assertEquals(expected, flags)
                }
            }
        }
    }
}

class ControlPacketTest {

    @Test
    fun constants() {
        assertEquals(0xAA, CONTROL_HEADER)
        assertEquals(0xBB, TELEMETRY_HEADER)
        assertEquals(6, CONTROL_SIZE)
        assertEquals(9, TELEMETRY_SIZE)
        assertEquals(3333, DEFAULT_UDP_PORT)
        assertEquals(500, STALE_MS)
    }

    @Test
    fun centerCommandEncodesExactly() {
        val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f)
        val pkt = ControlPacketCodec.encode(cmd)
        assertEquals(CONTROL_SIZE, pkt.size)
        assertEquals(0xAA, pkt[0].unsigned())
        assertEquals(0, pkt[1].unsigned())
        // 128-center exactness: zero stick maps to byte 128.
        assertEquals(128, pkt[2].unsigned())
        assertEquals(128, pkt[3].unsigned())
        assertEquals(0, pkt[4].unsigned())
        assertEquals(xorOf(pkt, 5), pkt[5].unsigned())
        assertEquals(cmd, ControlPacketCodec.decode(pkt))
    }

    @Test
    fun fullDeflectionWireBytes() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.EMERGENCY, 1f, -1f, flags = 255))
        assertEquals(3, pkt[1].unsigned())
        assertEquals(255, pkt[2].unsigned())
        assertEquals(0, pkt[3].unsigned())
        assertEquals(255, pkt[4].unsigned())
        val back = assertNotNull(ControlPacketCodec.decode(pkt))
        assertEquals(RobotMode.EMERGENCY, back.mode)
        assertEquals(1f, back.throttle)
        assertEquals(-1f, back.steering)
        assertEquals(255, back.flags)
    }

    @Test
    fun roundTripStickSweep() {
        val modes = RobotMode.entries
        var i = 0
        // Integer steps keep endpoints exact (-1.0 and 1.0); float += drifts
        // past 1.0 and would trip ControlCommand's range check.
        for (ti in -100..100 step 5) {
            for (si in -100..100 step 5) {
                val throttle = ti / 100f
                val steering = si / 100f
                val cmd = ControlCommand(modes[i % modes.size], throttle, steering, flags = i % 256)
                val back = assertNotNull(
                    ControlPacketCodec.decode(ControlPacketCodec.encode(cmd)),
                    "throttle=$throttle steering=$steering",
                )
                assertEquals(cmd.mode, back.mode)
                assertEquals(cmd.flags, back.flags)
                // Byte quantization: 0.5 LSB on the non-negative side, up to
                // ~1.5 LSB (= 0.01182) on the negative side where bytes 0/1
                // share the -1.0 bin (see fullReverseDeadBinDocumented).
                assertEquals(cmd.throttle, back.throttle, 0.012f)
                assertEquals(cmd.steering, back.steering, 0.012f)
                i++
            }
        }
    }

    @Test
    fun rejectsBadHeader() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0.5f, -0.5f))
        pkt[0] = 0x00
        assertNull(ControlPacketCodec.decode(pkt))
        pkt[0] = TELEMETRY_HEADER.toByte()
        assertNull(ControlPacketCodec.decode(pkt))
    }

    @Test
    fun rejectsWrongLength() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f))
        assertNull(ControlPacketCodec.decode(byteArrayOf()))
        assertNull(ControlPacketCodec.decode(pkt.copyOf(5)))
        assertNull(ControlPacketCodec.decode(pkt + byteArrayOf(0x00)))
    }

    @Test
    fun rejectsCorruptedChecksum() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.AUTONOMOUS, 0.25f, 0.75f, flags = 7))
        // Corrupt the checksum byte itself.
        val badChecksum = pkt.copyOf().also { it[5] = (it[5] + 1).toByte() }
        assertNull(ControlPacketCodec.decode(badChecksum))
        // Corrupt a payload byte without fixing the checksum.
        val badPayload = pkt.copyOf().also { it[2] = (it[2] + 1).toByte() }
        assertNull(ControlPacketCodec.decode(badPayload))
    }
}

class TelemetryPacketTest {

    @Test
    fun wireLayoutForKnownValues() {
        // pitch 12.3 -> 123 = 0x007B; roll -12.3 -> -123 = 0xFF85;
        // distance 0x1234; MANUAL + driving, no cliff -> 0b010.
        val t = Telemetry(12.3f, -12.3f, 0x1234, cliff = false, driving = true, mode = RobotMode.MANUAL)
        val pkt = TelemetryPacketCodec.encode(t)
        assertEquals(TELEMETRY_SIZE, pkt.size)
        assertEquals(0xBB, pkt[0].unsigned())
        assertEquals(0x00, pkt[1].unsigned())
        assertEquals(0x7B, pkt[2].unsigned())
        assertEquals(0xFF, pkt[3].unsigned())
        assertEquals(0x85, pkt[4].unsigned())
        assertEquals(0x12, pkt[5].unsigned())
        assertEquals(0x34, pkt[6].unsigned())
        assertEquals(0b010, pkt[7].unsigned())
        assertEquals(xorOf(pkt, 8), pkt[8].unsigned())
    }

    @Test
    fun roundTripIncludingNegativeAndMaxDistance() {
        val cases = listOf(
            Telemetry(0f, 0f, 0, false, false, RobotMode.MANUAL),
            Telemetry(-12.3f, 45.6f, 1234, true, true, RobotMode.EMERGENCY),
            Telemetry(-90f, -180f, 65535, true, false, RobotMode.LOW_POWER),
            Telemetry(180f, 90f, 65535, false, true, RobotMode.AUTONOMOUS),
            Telemetry(0.05f, -0.05f, 1, false, false, RobotMode.MANUAL),
        )
        for (t in cases) {
            val back = assertNotNull(TelemetryPacketCodec.decode(TelemetryPacketCodec.encode(t)), "$t")
            // Decidegree quantization: within half a decidegree.
            assertEquals(t.pitchDeg, back.pitchDeg, 0.051f, "$t")
            assertEquals(t.rollDeg, back.rollDeg, 0.051f, "$t")
            assertEquals(t.distanceMm, back.distanceMm, "$t")
            assertEquals(t.cliff, back.cliff, "$t")
            assertEquals(t.driving, back.driving, "$t")
            assertEquals(t.mode, back.mode, "$t")
        }
    }

    @Test
    fun rejectsBadHeader() {
        val pkt = TelemetryPacketCodec.encode(Telemetry(1f, 2f, 3, false, false, RobotMode.MANUAL))
        pkt[0] = 0x00
        assertNull(TelemetryPacketCodec.decode(pkt))
        pkt[0] = CONTROL_HEADER.toByte()
        assertNull(TelemetryPacketCodec.decode(pkt))
    }

    @Test
    fun rejectsWrongLength() {
        val pkt = TelemetryPacketCodec.encode(Telemetry(1f, 2f, 3, false, false, RobotMode.MANUAL))
        assertNull(TelemetryPacketCodec.decode(byteArrayOf()))
        assertNull(TelemetryPacketCodec.decode(pkt.copyOf(8)))
        assertNull(TelemetryPacketCodec.decode(pkt + byteArrayOf(0x00)))
    }

    @Test
    fun rejectsCorruptedChecksum() {
        val pkt = TelemetryPacketCodec.encode(Telemetry(-5f, 5f, 999, true, true, RobotMode.AUTONOMOUS))
        val badChecksum = pkt.copyOf().also { it[8] = (it[8] + 1).toByte() }
        assertNull(TelemetryPacketCodec.decode(badChecksum))
        val badPayload = pkt.copyOf().also { it[6] = (it[6] + 1).toByte() }
        assertNull(TelemetryPacketCodec.decode(badPayload))
    }
}
