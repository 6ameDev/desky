package com.desky.shared.domain

/**
 * Wire constants. Mirror of `udp_codec.h` (`kControlHeader`, `kTelemetryHeader`,
 * `kControlSize`, `kTelemetrySize`), `udp_server.h` (`CFG_UDP_PORT`) and
 * `coordinator.h` (`CFG_COORDINATOR_STALE_MS`).
 */
const val CONTROL_HEADER = 0xAA
const val TELEMETRY_HEADER = 0xBB
const val CONTROL_SIZE = 6
const val TELEMETRY_SIZE = 9
const val DEFAULT_UDP_PORT = 3333

/** Fixed robot endpoint: the app never takes an IP from the user. */
const val ROBOT_HOST = "192.168.4.1"

/** Pairing wire headers/sizes (frozen spec: `shared/protocol/README.md` §v1). */
const val PAIR_REQ_HEADER = 0x50
const val PAIR_OK_HEADER = 0x51
const val PAIR_ERR_HEADER = 0x52
const val PAIR_REQ_SIZE = 12
const val PAIR_OK_SIZE = 38
const val PAIR_ERR_SIZE = 4
const val PAIR_CODE_SIZE = 6
const val PAIR_NONCE_SIZE = 4

/** Failsafe window: the robot stops ~500 ms after the last control datagram. */
const val STALE_MS = 500

/** statusFlags bit map (mirror of `udpstatus` in `udp_server.h`). */
const val CLIFF_BIT = 0
const val DRIVING_BIT = 1
const val MODE_SHIFT = 2
const val MODE_MASK = 0x03

/**
 * Stick byte (center-128) -> unit (-1..1). Exact mirror of
 * `coordinator::byteToUnit`: 128 -> 0.0, 255 -> +1.0, 0 clamps to -1.0.
 *
 * @param raw raw wire byte (Kotlin [Byte] is signed; treated as 0..255).
 */
fun byteToUnit(raw: Byte): Float {
    val unsigned = raw.toInt() and 0xFF
    val f = (unsigned - 128).toFloat() / 127f
    return f.coerceIn(-1f, 1f)
}

/**
 * Unit (-1..1) -> stick byte. Exact mirror of `coordinator::unitToByte`
 * (round-half-away via +-0.5 + truncation, clamped to 0..255).
 */
fun unitToByte(v: Float): Byte {
    val clamped = v.coerceIn(-1f, 1f)
    val b = (clamped * 127f + 128f + if (clamped >= 0f) 0.5f else -0.5f).toInt()
    return b.coerceIn(0, 255).toByte()
}

/**
 * Degrees -> decidegrees (0.1 deg), clamped to int16 for the wire.
 * Mirror of `udpstatus::degToDecideg`.
 */
fun degToDecideg(deg: Float): Short {
    val d = deg * 10f
    if (d >= 32767f) return 32767
    if (d <= -32768f) return -32768
    return (d + if (d >= 0f) 0.5f else -0.5f).toInt().toShort()
}

/** Decidegrees -> degrees. */
fun decidegToDeg(raw: Short): Float = raw.toFloat() / 10f

/** Pack cliff/driving/mode into the telemetry statusFlags byte (0..255). */
fun packStatusFlags(cliff: Boolean, driving: Boolean, mode: RobotMode): Int {
    var flags = 0
    if (cliff) flags = flags or (1 shl CLIFF_BIT)
    if (driving) flags = flags or (1 shl DRIVING_BIT)
    flags = flags or ((mode.toBits() and MODE_MASK) shl MODE_SHIFT)
    return flags
}

fun statusCliff(flags: Int): Boolean = (flags and (1 shl CLIFF_BIT)) != 0

fun statusDriving(flags: Int): Boolean = (flags and (1 shl DRIVING_BIT)) != 0

fun statusMode(flags: Int): RobotMode = RobotMode.fromBits((flags shr MODE_SHIFT) and MODE_MASK)

private fun xorChecksum(bytes: ByteArray, len: Int): Byte {
    var sum = 0
    for (i in 0 until len) {
        sum = sum xor (bytes[i].toInt() and 0xFF)
    }
    return sum.toByte()
}

private fun putI16BE(out: ByteArray, at: Int, v: Short) {
    val i = v.toInt()
    out[at] = ((i shr 8) and 0xFF).toByte()
    out[at + 1] = (i and 0xFF).toByte()
}

private fun getI16BE(bytes: ByteArray, at: Int): Short {
    return (((bytes[at].toInt() and 0xFF) shl 8) or (bytes[at + 1].toInt() and 0xFF)).toShort()
}

/**
 * Control packet codec: App -> Robot, 6 bytes
 * `[0xAA | mode | throttle | steering | flags | XOR]`.
 * Pure byte math mirroring `udp::encodeControl` / `udp::decodeControl`.
 */
object ControlPacketCodec {
    fun encode(cmd: ControlCommand): ByteArray {
        val out = ByteArray(CONTROL_SIZE)
        out[0] = CONTROL_HEADER.toByte()
        out[1] = cmd.mode.toBits().toByte()
        out[2] = unitToByte(cmd.throttle)
        out[3] = unitToByte(cmd.steering)
        out[4] = cmd.flags.toByte()
        out[5] = xorChecksum(out, CONTROL_SIZE - 1)
        return out
    }

    /**
     * Null on wrong header, wrong length, or bad checksum. The mode byte is
     * masked to 2 bits first (firmware `statusMode` semantics), so any mode
     * byte value decodes instead of rejecting.
     */
    fun decode(bytes: ByteArray): ControlCommand? {
        if (bytes.size != CONTROL_SIZE) return null
        if ((bytes[0].toInt() and 0xFF) != CONTROL_HEADER) return null
        if (xorChecksum(bytes, CONTROL_SIZE - 1) != bytes[CONTROL_SIZE - 1]) return null
        return ControlCommand(
            mode = RobotMode.fromBits(bytes[1].toInt() and MODE_MASK),
            throttle = byteToUnit(bytes[2]),
            steering = byteToUnit(bytes[3]),
            flags = bytes[4].toInt() and 0xFF,
        )
    }
}

/**
 * Pairing packet codec (frozen spec: `shared/protocol/README.md`
 * §"Pairing + authenticated drive v1").
 *
 * - `PAIR_REQ` App -> Robot, 12 bytes:
 *   `[0x50 | code 6B ASCII | client_nonce 4B | XOR]`.
 * - `PAIR_OK` Robot -> App, 38 bytes:
 *   `[0x51 | server_nonce 4B | ct 16B | gcm_tag 16B | XOR]`.
 * - `PAIR_ERR` Robot -> App, 4 bytes:
 *   `[0x52 | reason 1B | retry_s 1B | XOR]`
 *   (reason: 0=bad_code, 1=locked, 2=not_in_window, 3=already_bonded).
 *
 * Pure byte math mirroring the firmware `middleware/pairing.h` layout.
 */
object PairingPacketCodec {
    /** Throws [IllegalArgumentException] unless [code] is 6B and [clientNonce] 4B. */
    fun encodePairReq(code: ByteArray, clientNonce: ByteArray): ByteArray {
        require(code.size == PAIR_CODE_SIZE) { "encodePairReq: code must be 6 ASCII digits" }
        require(clientNonce.size == PAIR_NONCE_SIZE) { "encodePairReq: client nonce must be 4 bytes" }
        val out = ByteArray(PAIR_REQ_SIZE)
        out[0] = PAIR_REQ_HEADER.toByte()
        code.copyInto(out, destinationOffset = 1)
        clientNonce.copyInto(out, destinationOffset = 1 + PAIR_CODE_SIZE)
        out[PAIR_REQ_SIZE - 1] = xorChecksum(out, PAIR_REQ_SIZE - 1)
        return out
    }

    /**
     * Null on wrong header, wrong length, or bad checksum. On success the
     * `Triple` is `(server_nonce 4B, ct 16B, gcm_tag 16B)`.
     */
    fun decodePairOk(bytes: ByteArray): Triple<ByteArray, ByteArray, ByteArray>? {
        if (bytes.size != PAIR_OK_SIZE) return null
        if ((bytes[0].toInt() and 0xFF) != PAIR_OK_HEADER) return null
        if (xorChecksum(bytes, PAIR_OK_SIZE - 1) != bytes[PAIR_OK_SIZE - 1]) return null
        val serverNonce = bytes.copyOfRange(1, 1 + PAIR_NONCE_SIZE)
        val ct = bytes.copyOfRange(5, 5 + 16)
        val tag = bytes.copyOfRange(21, 21 + 16)
        return Triple(serverNonce, ct, tag)
    }

    /**
     * Null on wrong header, wrong length, or bad checksum. On success the
     * `Pair` is `(reason, retry_s)` as unsigned ints.
     */
    fun decodePairErr(bytes: ByteArray): Pair<Int, Int>? {
        if (bytes.size != PAIR_ERR_SIZE) return null
        if ((bytes[0].toInt() and 0xFF) != PAIR_ERR_HEADER) return null
        if (xorChecksum(bytes, PAIR_ERR_SIZE - 1) != bytes[PAIR_ERR_SIZE - 1]) return null
        return (bytes[1].toInt() and 0xFF) to (bytes[2].toInt() and 0xFF)
    }
}
/**
 * Telemetry packet codec: Robot -> App, 9 bytes
 * `[0xBB | pitchBE i16 | rollBE i16 | distBE u16 | statusFlags | XOR]`.
 * Pure byte math mirroring `udp::encodeTelemetry` / `udp::decodeTelemetry`.
 */
object TelemetryPacketCodec {
    /** Provided for tests/fakes; the robot is the real producer on the wire. */
    fun encode(t: Telemetry): ByteArray {
        val out = ByteArray(TELEMETRY_SIZE)
        out[0] = TELEMETRY_HEADER.toByte()
        putI16BE(out, 1, degToDecideg(t.pitchDeg))
        putI16BE(out, 3, degToDecideg(t.rollDeg))
        out[5] = ((t.distanceMm shr 8) and 0xFF).toByte()
        out[6] = (t.distanceMm and 0xFF).toByte()
        out[7] = packStatusFlags(t.cliff, t.driving, t.mode).toByte()
        out[8] = xorChecksum(out, TELEMETRY_SIZE - 1)
        return out
    }

    /** Null on wrong header, wrong length, or bad checksum. */
    fun decode(bytes: ByteArray): Telemetry? {
        if (bytes.size != TELEMETRY_SIZE) return null
        if ((bytes[0].toInt() and 0xFF) != TELEMETRY_HEADER) return null
        if (xorChecksum(bytes, TELEMETRY_SIZE - 1) != bytes[TELEMETRY_SIZE - 1]) return null
        val pitch = getI16BE(bytes, 1)
        val roll = getI16BE(bytes, 3)
        val distance = ((bytes[5].toInt() and 0xFF) shl 8) or (bytes[6].toInt() and 0xFF)
        val flags = bytes[7].toInt() and 0xFF
        return Telemetry(
            pitchDeg = decidegToDeg(pitch),
            rollDeg = decidegToDeg(roll),
            distanceMm = distance,
            cliff = statusCliff(flags),
            driving = statusDriving(flags),
            mode = statusMode(flags),
        )
    }
}
