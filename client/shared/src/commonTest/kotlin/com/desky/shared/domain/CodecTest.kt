package com.desky.shared.domain

import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val BYTE_MASK = 0xFF
private const val HEX_RADIX = 16
private const val STICK_CENTER = 128
private const val STICK_BYTE_MAX = 255
private const val PSK_BYTE_LENGTH = 16
private const val SHORT_PSK_SIZE = 8
private const val EXACT_TOLERANCE = 1e-6f
private const val ROUND_TRIP_TOLERANCE = 0.012f
private const val TEST_ANGLE_DEG = 12.3f
private const val TEST_ANGLE_DECIDEG: Short = 123
private const val OUT_OF_RANGE_DEG = 5000f
private const val OUT_OF_RANGE_UNIT = 2f
private const val INT16_MAX = 32767
private const val INT16_MIN = -32768
private const val RAIL_TOP_DEG = 3276.7f
private const val RAIL_BOTTOM_DEG = 3276.8f
private const val JUST_INSIDE_TOP_DEG = 3276.6f
private const val JUST_INSIDE_BOTTOM_DEG = 3276.7f
private const val SWEEP_LIMIT = 100
private const val SWEEP_STEP = 5
private const val UNIT_SWEEP_STEP = 0.01f
private const val UNIT_SWEEP_END = 1.0001f
private const val PERCENT_SCALE = 100f
private const val EMERGENCY_MODE_BITS = 3
private const val ALL_FLAGS_SET = 255
private const val TEST_DISTANCE = 0x1234
private const val DECIDEG_TOLERANCE = 0.051f
private const val EXPECTED_MANUAL_BIT = 4
private const val EXPECTED_MANUAL_MASK = 0x10
private const val RESERVED_BITS_MASK = 0xE0
private const val EXPECTED_CONTROL_HEADER = 0xAA
private const val EXPECTED_TELEMETRY_HEADER = 0xBB
private const val EXPECTED_CONTROL_SIZE = 6
private const val EXPECTED_TELEMETRY_SIZE = 9
private const val EXPECTED_DEFAULT_UDP_PORT = 3333
private const val EXPECTED_STALE_MS = 500
private const val EXPECTED_DRIVE_HEADER = 0xAC
private const val EXPECTED_DRIVE_SIZE = 16

private fun Byte.unsigned(): Int = this.toInt() and BYTE_MASK

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
        assertEquals(0f, byteToUnit(STICK_CENTER.toByte()))
        assertEquals(1f, byteToUnit(STICK_BYTE_MAX.toByte()))
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
            assertEquals(expected, byteToUnit(raw.toByte()), EXACT_TOLERANCE, "raw=$raw")
        }
        // Clamp region: every raw below 1 still maps into [-1, 1].
        assertEquals(-1f, byteToUnit(1.toByte()))
    }

    @Test
    fun unitToByteEdges() {
        assertEquals(0, unitToByte(-1f).unsigned())
        assertEquals(STICK_CENTER, unitToByte(0f).unsigned())
        assertEquals(STICK_BYTE_MAX, unitToByte(1f).unsigned())
        // Out-of-range inputs clamp instead of wrapping.
        assertEquals(STICK_BYTE_MAX, unitToByte(OUT_OF_RANGE_UNIT).unsigned())
        assertEquals(0, unitToByte(-OUT_OF_RANGE_UNIT).unsigned())
    }

    @Test
    fun unitByteRoundTripWithinTolerance() {
        // Round-trip error is <= 0.5 LSB on the non-negative side but up to
        // ~1.5 LSB on the negative side: bytes 0 and 1 both decode to -1.0
        // (dead bin at full reverse), and unitToByte truncates toward zero
        // (firmware-exact floor-rounding). 0.012f clears 1.5/127 ~= 0.01182.
        var v = -1f
        while (v <= UNIT_SWEEP_END) {
            val back = byteToUnit(unitToByte(v))
            assertTrue(abs(back - v) <= ROUND_TRIP_TOLERANCE, "v=$v back=$back")
            v += UNIT_SWEEP_STEP
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
        assertEquals(TEST_ANGLE_DECIDEG, degToDecideg(TEST_ANGLE_DEG))
        assertEquals((-TEST_ANGLE_DECIDEG).toShort(), degToDecideg(-TEST_ANGLE_DEG))
        assertEquals(TEST_ANGLE_DEG, decidegToDeg(TEST_ANGLE_DECIDEG), EXACT_TOLERANCE)
        assertEquals(-TEST_ANGLE_DEG, decidegToDeg((-TEST_ANGLE_DECIDEG).toShort()), EXACT_TOLERANCE)
    }

    @Test
    fun clampsAtInt16Scale() {
        // udpstatus::degToDecideg clamps to [-32768, +32767] (asymmetric int16).
        assertEquals(INT16_MAX.toShort(), degToDecideg(OUT_OF_RANGE_DEG))
        assertEquals(INT16_MIN.toShort(), degToDecideg(-OUT_OF_RANGE_DEG))
        assertEquals(INT16_MAX.toShort(), degToDecideg(RAIL_TOP_DEG))
        assertEquals(INT16_MIN.toShort(), degToDecideg(-RAIL_BOTTOM_DEG))
        // Just inside the rails still converts.
        assertEquals(32766.toShort(), degToDecideg(JUST_INSIDE_TOP_DEG))
        assertEquals((-32767).toShort(), degToDecideg(-JUST_INSIDE_BOTTOM_DEG))
    }
}

class StatusFlagsTest {

    @Test
    fun packUnpackAllModesTimesCliffTimesDriving() {
        for (mode in RobotMode.entries) {
            for (cliff in listOf(false, true)) {
                for (driving in listOf(false, true)) {
                    for (manual in listOf(false, true)) {
                        val flags = packStatusFlags(cliff, driving, mode, manual)
                        assertEquals(cliff, statusCliff(flags), "$mode cliff=$cliff driving=$driving manual=$manual")
                        assertEquals(driving, statusDriving(flags), "$mode cliff=$cliff driving=$driving manual=$manual")
                        assertEquals(mode, statusMode(flags), "$mode cliff=$cliff driving=$driving manual=$manual")
                        assertEquals(manual, statusManual(flags), "$mode cliff=$cliff driving=$driving manual=$manual")
                        // Reserved bits 5-7 stay zero.
                        assertEquals(0, flags and RESERVED_BITS_MASK, "$mode cliff=$cliff driving=$driving manual=$manual")
                        // Exact bit layout: bit0=cliff, bit1=driving, bits2-3=mode, bit4=manual.
                        val expected = (if (cliff) 1 else 0) or
                            ((if (driving) 1 else 0) shl 1) or
                            (mode.toBits() shl 2) or
                            ((if (manual) 1 else 0) shl MANUAL_BIT)
                        assertEquals(expected, flags)
                    }
                }
            }
        }
    }

    @Test
    fun manualDefaultsOffForLegacyCallers() {
        // The default keeps every pre-manual call site (bench fixtures, old
        // robots) encoding bit 4 clear.
        for (mode in RobotMode.entries) {
            val flags = packStatusFlags(cliff = false, driving = false, mode = mode)
            assertEquals(false, statusManual(flags), "$mode")
            assertEquals(0, flags and (1 shl MANUAL_BIT), "$mode")
        }
        assertEquals(EXPECTED_MANUAL_BIT, MANUAL_BIT)
        assertEquals(EXPECTED_MANUAL_MASK, 1 shl MANUAL_BIT)
    }
}

class ControlPacketTest {

    @Test
    fun constants() {
        assertEquals(EXPECTED_CONTROL_HEADER, CONTROL_HEADER)
        assertEquals(EXPECTED_TELEMETRY_HEADER, TELEMETRY_HEADER)
        assertEquals(EXPECTED_CONTROL_SIZE, CONTROL_SIZE)
        assertEquals(EXPECTED_TELEMETRY_SIZE, TELEMETRY_SIZE)
        assertEquals(EXPECTED_DEFAULT_UDP_PORT, DEFAULT_UDP_PORT)
        assertEquals(EXPECTED_STALE_MS, STALE_MS)
    }

    @Test
    fun centerCommandEncodesExactly() {
        val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f)
        val pkt = ControlPacketCodec.encode(cmd)
        assertEquals(CONTROL_SIZE, pkt.size)
        assertEquals(0xAA, pkt[0].unsigned())
        assertEquals(0, pkt[1].unsigned())
        // 128-center exactness: zero stick maps to byte 128.
        assertEquals(STICK_CENTER, pkt[2].unsigned())
        assertEquals(STICK_CENTER, pkt[3].unsigned())
        assertEquals(0, pkt[4].unsigned())
        assertEquals(xorOf(pkt, CONTROL_SIZE - 1), pkt[5].unsigned())
        assertEquals(cmd, ControlPacketCodec.decode(pkt))
    }

    @Test
    fun fullDeflectionWireBytes() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.EMERGENCY, 1f, -1f, flags = ALL_FLAGS_SET))
        assertEquals(EMERGENCY_MODE_BITS, pkt[1].unsigned())
        assertEquals(STICK_BYTE_MAX, pkt[2].unsigned())
        assertEquals(0, pkt[3].unsigned())
        assertEquals(ALL_FLAGS_SET, pkt[4].unsigned())
        val back = assertNotNull(ControlPacketCodec.decode(pkt))
        assertEquals(RobotMode.EMERGENCY, back.mode)
        assertEquals(1f, back.throttle)
        assertEquals(-1f, back.steering)
        assertEquals(ALL_FLAGS_SET, back.flags)
    }

    @Test
    fun roundTripStickSweep() {
        val modes = RobotMode.entries
        var i = 0
        // Integer steps keep endpoints exact (-1.0 and 1.0); float += drifts
        // past 1.0 and would trip ControlCommand's range check.
        for (ti in -SWEEP_LIMIT..SWEEP_LIMIT step SWEEP_STEP) {
            for (si in -SWEEP_LIMIT..SWEEP_LIMIT step SWEEP_STEP) {
                val throttle = ti / PERCENT_SCALE
                val steering = si / PERCENT_SCALE
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
                assertEquals(cmd.throttle, back.throttle, ROUND_TRIP_TOLERANCE)
                assertEquals(cmd.steering, back.steering, ROUND_TRIP_TOLERANCE)
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
        assertNull(ControlPacketCodec.decode(pkt.copyOf(CONTROL_SIZE - 1)))
        assertNull(ControlPacketCodec.decode(pkt + byteArrayOf(0x00)))
    }

    @Test
    fun rejectsCorruptedChecksum() {
        val pkt = ControlPacketCodec.encode(ControlCommand(RobotMode.AUTONOMOUS, 0.25f, 0.75f, flags = 7))
        // Corrupt the checksum byte itself.
        val badChecksum = pkt.copyOf().also { it[CONTROL_SIZE - 1] = (it[CONTROL_SIZE - 1] + 1).toByte() }
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
        val t = Telemetry(12.3f, -12.3f, TEST_DISTANCE, cliff = false, driving = true, mode = RobotMode.MANUAL)
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
        assertEquals(xorOf(pkt, TELEMETRY_SIZE - 1), pkt[8].unsigned())
    }

    @Test
    fun roundTripIncludingNegativeAndMaxDistance() {
        val cases = listOf(
            Telemetry(0f, 0f, 0, false, false, RobotMode.MANUAL),
            Telemetry(-12.3f, 45.6f, 1234, true, true, RobotMode.EMERGENCY),
            Telemetry(-90f, -180f, 65535, true, false, RobotMode.LOW_POWER),
            Telemetry(180f, 90f, 65535, false, true, RobotMode.AUTONOMOUS),
            Telemetry(0.05f, -0.05f, 1, false, false, RobotMode.MANUAL),
            Telemetry(1f, 2f, 3, false, true, RobotMode.MANUAL, manual = true),
            Telemetry(-4f, 7f, 42, true, false, RobotMode.AUTONOMOUS, manual = true),
        )
        for (t in cases) {
            val back = assertNotNull(TelemetryPacketCodec.decode(TelemetryPacketCodec.encode(t)), "$t")
            // Decidegree quantization: within half a decidegree.
            assertEquals(t.pitchDeg, back.pitchDeg, DECIDEG_TOLERANCE, "$t")
            assertEquals(t.rollDeg, back.rollDeg, DECIDEG_TOLERANCE, "$t")
            assertEquals(t.distanceMm, back.distanceMm, "$t")
            assertEquals(t.cliff, back.cliff, "$t")
            assertEquals(t.driving, back.driving, "$t")
            assertEquals(t.mode, back.mode, "$t")
            assertEquals(t.manual, back.manual, "$t")
        }
    }

    @Test
    fun manualBitLandsOnWireBit4() {
        // MANUAL hold, no cliff, no driving, MANUAL mode -> 0b10000.
        val t = Telemetry(0f, 0f, 0, cliff = false, driving = false, mode = RobotMode.MANUAL, manual = true)
        val pkt = TelemetryPacketCodec.encode(t)
        assertEquals(EXPECTED_MANUAL_MASK, pkt[7].unsigned())
        val back = assertNotNull(TelemetryPacketCodec.decode(pkt))
        assertEquals(true, back.manual)
        // Bit clear decodes false (old robots never set it).
        val off = Telemetry(0f, 0f, 0, cliff = false, driving = false, mode = RobotMode.MANUAL)
        val offPkt = TelemetryPacketCodec.encode(off)
        assertEquals(0x00, offPkt[7].unsigned())
        assertEquals(false, assertNotNull(TelemetryPacketCodec.decode(offPkt)).manual)
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
        assertNull(TelemetryPacketCodec.decode(pkt.copyOf(TELEMETRY_SIZE - 1)))
        assertNull(TelemetryPacketCodec.decode(pkt + byteArrayOf(0x00)))
    }

    @Test
    fun rejectsCorruptedChecksum() {
        val pkt = TelemetryPacketCodec.encode(Telemetry(-5f, 5f, 999, true, true, RobotMode.AUTONOMOUS))
        val badChecksum = pkt.copyOf().also { it[TELEMETRY_SIZE - 1] = (it[TELEMETRY_SIZE - 1] + 1).toByte() }
        assertNull(TelemetryPacketCodec.decode(badChecksum))
        val badPayload = pkt.copyOf().also { it[6] = (it[6] + 1).toByte() }
        assertNull(TelemetryPacketCodec.decode(badPayload))
    }
}

class SignedControlPacketTest {

    private fun hex(s: String): ByteArray {
        require(s.length % 2 == 0)
        return ByteArray(s.length / 2) { i ->
            s.substring(i * 2, i * 2 + 2).toInt(HEX_RADIX).toByte()
        }
    }

    @Test
    fun constants() {
        assertEquals(EXPECTED_DRIVE_HEADER, DRIVE_HEADER)
        assertEquals(EXPECTED_DRIVE_SIZE, DRIVE_SIZE)
    }

    /**
     * Full 16B golden frame through the Task 6 frozen `drive_tag`
     * (`3ec8b625c9c1` for counter=1, mode=`0x00`, thr/str=`0x80`,
     * flags=`0x00`, psk=`0011...eeff`).
     */
    @Test
    fun goldenFrameBytesAndTag() = kotlinx.coroutines.test.runTest {
        val psk = hex("00112233445566778899aabbccddeeff")
        val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f, 0)
        val pkt = encodeSigned(cmd, 1u, psk)

        assertEquals(DRIVE_SIZE, pkt.size)
        assertEquals(0xAC, pkt[0].unsigned())
        assertEquals(0x00, pkt[1].unsigned())
        // Sticks identical to the legacy positions: 0 -> byte 128.
        assertEquals(STICK_CENTER, pkt[2].unsigned())
        assertEquals(STICK_CENTER, pkt[3].unsigned())
        assertEquals(0x00, pkt[4].unsigned())
        // Counter BE bytes 5..8 = 1.
        assertEquals(0, pkt[5].unsigned())
        assertEquals(0, pkt[6].unsigned())
        assertEquals(0, pkt[7].unsigned())
        assertEquals(1, pkt[8].unsigned())
        // Tag bytes 9..14 = frozen drive_tag vector.
        assertTrue(
            pkt.copyOfRange(9, 15).contentEquals(hex("3ec8b625c9c1")),
            "tag bytes, got ${pkt.copyOfRange(9, 15).joinToString("") { (it.toInt() and BYTE_MASK).toString(HEX_RADIX).padStart(2, '0') }}",
        )
        // Trailing XOR over bytes 0..14.
        assertEquals(xorOf(pkt, DRIVE_SIZE - 1), pkt[15].unsigned())
    }

    @Test
    fun counterLaysOutBigEndian() = kotlinx.coroutines.test.runTest {
        val psk = ByteArray(PSK_BYTE_LENGTH) { it.toByte() }
        val pkt = encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f), 0x01020304u, psk)
        assertEquals(0x01, pkt[5].unsigned())
        assertEquals(0x02, pkt[6].unsigned())
        assertEquals(0x03, pkt[7].unsigned())
        assertEquals(0x04, pkt[8].unsigned())
        assertEquals(xorOf(pkt, DRIVE_SIZE - 1), pkt[15].unsigned())
    }

    @Test
    fun rejectsShortPsk() = kotlinx.coroutines.test.runTest {
        var threw = false
        try {
            encodeSigned(ControlCommand(RobotMode.MANUAL, 0f, 0f), 1u, ByteArray(SHORT_PSK_SIZE))
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue(threw, "encodeSigned must require a 16-byte PSK")
    }

    @Test
    fun hexRoundTrip() {
        assertTrue(hexToBytes("00112233445566778899aabbccddeeff").contentEquals(hex("00112233445566778899aabbccddeeff")))
        assertTrue(hexToBytes("3EC8B625C9C1").contentEquals(hex("3ec8b625c9c1")))
        var threw = 0
        try {
            hexToBytes("abc")
        } catch (e: IllegalArgumentException) {
            threw++
        }
        try {
            hexToBytes("zz")
        } catch (e: IllegalArgumentException) {
            threw++
        }
        assertEquals(2, threw)
    }
}
