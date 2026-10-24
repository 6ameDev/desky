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
/**
 * v1.1 `PAIR_OK` size: 39 bytes
 * `[0x51 | server_nonce 4B | ct 17B | gcm_tag 16B | XOR]`, where the ct
 * covers `psk16 || persist_u8` (see `shared/protocol/README.md` §v1.1
 * addendum). [PAIR_OK_SIZE] stays the v1 (38B, ct 16B) size: the app
 * accepts BOTH — a v1 robot has no interval byte and the app defaults to
 * [ToRobotCounter.COUNTER_PERSIST_EVERY].
 */
const val PAIR_OK_SIZE_V11 = 39
const val PAIR_ERR_SIZE = 4
const val PAIR_CODE_SIZE = 6
const val PAIR_NONCE_SIZE = 4

/**
 * Authenticated drive header/size (frozen spec: `shared/protocol/README.md`
 * §"Pairing + authenticated drive v1": `DRIVE` 16 bytes
 * `[0xAC | mode | thr | str | flags | counterBE4 | tag6 | XOR]`).
 */
const val DRIVE_HEADER = 0xAC
const val DRIVE_SIZE = 16

/**
 * Signed unpair header/size: `UNPAIR`, 12 bytes
 * `[0xAD | counterBE4 | tag6 | XOR]`, where
 * `tag = first6(HMAC-SHA256(psk16, "UNPAIR ASCII" || counterBE4))` — the
 * 10-byte message is the 6 ASCII bytes of `"UNPAIR"` followed by the 4-byte
 * big-endian counter. Must match the firmware `UNPAIR` frame exactly. The
 * robot never replies; the app always proceeds to the local forget.
 */
const val UNPAIR_HEADER = 0xAD
const val UNPAIR_SIZE = 12

/**
 * Tier-1 silent auto-pair-refresh headers/sizes (see `shared/protocol/README.md`
 * §"Tier-1 silent auto-pair-refresh"): `PAIR_REFRESH_REQ` 14 bytes
 * `[0x53 | eraBE2 | counterBE4 | tag6 | XOR]` (App -> Robot,
 * `tag = first6(HMAC-SHA256(oldPsk, "PAIRREFRESH" || eraBE2 || counterBE4))`)
 * and `PAIR_REFRESH_ACK` 10 bytes `[0x54 | eraBE2 | tag6 | XOR]` (Robot -> App,
 * `tag = first6(HMAC-SHA256(newPsk, "PAIRREFRESHACK" || eraBE2))`).
 * Headers `0x53`/`0x54` are claimed here — never `0xAA`/`0xAC`/`0xBB`/
 * `0x50`/`0x51`/`0x52`/`0xAD` (Phase-2 `SET_RATE` must pick other values).
 */
const val PAIR_REFRESH_REQ_HEADER = 0x53
const val PAIR_REFRESH_REQ_SIZE = 14
const val PAIR_REFRESH_ACK_HEADER = 0x54
const val PAIR_REFRESH_ACK_SIZE = 10

/**
 * Robot alive rule headers/sizes (see `shared/protocol/README.md`
 * §"ALIVE + DISCONNECT + robot alive rule"): `ALIVE` 14 bytes
 * `[0x55 | eraBE2 | counterBE4 | tag6 | XOR]` (App -> Robot keepalive,
 * `tag = first6(HMAC-SHA256(psk, "ALIVE" || eraBE2 || counterBE4))`,
 * era = CURRENT paired era) and `DISCONNECT` 14 bytes
 * `[0x56 | eraBE2 | counterBE4 | tag6 | XOR]` (graceful goodbye,
 * `tag = first6(HMAC-SHA256(psk, "DISCONNECT" || eraBE2 || counterBE4))`,
 * pair KEPT). Both ride the shared strictly-increasing counter (same
 * holder as `DRIVE` + `PAIR_REFRESH_REQ`).
 * Headers `0x55`/`0x56` are claimed here — never `0xAA`/`0xAC`/`0xBB`/
 * `0x50`/`0x51`/`0x52`/`0x53`/`0x54`/`0xAD` (Phase-2 `SET_RATE` must pick
 * other values).
 */
const val ALIVE_HEADER = 0x55
const val ALIVE_SIZE = 14
const val DISCONNECT_HEADER = 0x56
const val DISCONNECT_SIZE = 14

/**
 * Manual take/release headers/sizes (see `shared/protocol/README.md`
 * §"Pairing + authenticated drive v1", MANUAL take/release bullets): `MANUAL_TAKE` 14 bytes
 * `[0x57 | eraBE2 | counterBE4 | tag6 | XOR]` (App -> Robot mode entry,
 * `tag = first6(HMAC-SHA256(psk, "MANUALTAKE" || eraBE2 || counterBE4))`)
 * and `MANUAL_RELEASE` 14 bytes
 * `[0x58 | eraBE2 | counterBE4 | tag6 | XOR]` (App -> Robot mode exit,
 * `tag = first6(HMAC-SHA256(psk, "MANUALRELEASE" || eraBE2 || counterBE4))`,
 * era = CURRENT paired era). Both ride the shared strictly-increasing
 * counter (same holder as `DRIVE` + `ALIVE` + `DISCONNECT`).
 * Headers `0x57`/`0x58` are claimed here — never `0xAA`/`0xAC`/`0xBB`/
 * `0x50`/`0x51`/`0x52`/`0x53`/`0x54`/`0x55`/`0x56`/`0xAD` (Phase-2
 * `SET_RATE` must pick other values).
 */
const val MANUAL_TAKE_HEADER = 0x57
const val MANUAL_TAKE_SIZE = 14
const val MANUAL_RELEASE_HEADER = 0x58
const val MANUAL_RELEASE_SIZE = 14

/**
 * Expression headers: `EXPR_TRIGGER` 0x5B is live (15 bytes
 * `[0x5B | id | eraBE2 | counterBE4 | tag6 | XOR]`, App -> Robot play
 * request, `tag = first6(HMAC-SHA256(psk, "EXPRTRIG" || id || eraBE2 ||
 * counterBE4))`, one send per tap, robot-gated on MANUAL, no reply).
 * `EXPR_LIST_REQ` 0x59 and `EXPR_LIST_RESP` 0x5A are RETIRED-reserved: the
 * list round trip is gone (the picker is hardcoded app-side mirroring the
 * shared catalog), but the headers stay claimed here — never reuse 0x59 /
 * 0x5A for anything else. The trigger frame rides the shared
 * strictly-increasing counter (same holder as `DRIVE` + `ALIVE` +
 * `MANUAL_TAKE`).
 * Headers `0x59`/`0x5A`/`0x5B` are claimed here — never `0xAA`/`0xAC`/
 * `0xBB`/`0x50`/`0x51`/`0x52`/`0x53`/`0x54`/`0x55`/`0x56`/`0x57`/`0x58`/
 * `0xAD`.
 */
const val EXPR_LIST_REQ_HEADER = 0x59
const val EXPR_LIST_RESP_HEADER = 0x5A
const val EXPR_TRIGGER_HEADER = 0x5B
const val EXPR_TRIGGER_SIZE = 15

/** Failsafe window: the robot stops ~500 ms after the last control datagram. */
const val STALE_MS = 500

/** statusFlags bit map (mirror of `udpstatus` in `udp_server.h`). */
const val CLIFF_BIT = 0
const val DRIVING_BIT = 1
const val MODE_SHIFT = 2
const val MODE_MASK = 0x03
/**
 * statusFlags bit 4: robot reports MANUAL mode active (see
 * `shared/protocol/README.md` §"Pairing + authenticated drive v1" (MANUAL take/release). Bits 5-7 stay
 * reserved. The mode bits (2-3) keep their existing meaning; this bit is
 * the authoritative drivability report the drive loop gates on.
 */
const val MANUAL_BIT = 4

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

/** Pack cliff/driving/mode/manual into the telemetry statusFlags byte (0..255). */
fun packStatusFlags(cliff: Boolean, driving: Boolean, mode: RobotMode, manual: Boolean = false): Int {
    var flags = 0
    if (cliff) flags = flags or (1 shl CLIFF_BIT)
    if (driving) flags = flags or (1 shl DRIVING_BIT)
    flags = flags or ((mode.toBits() and MODE_MASK) shl MODE_SHIFT)
    if (manual) flags = flags or (1 shl MANUAL_BIT)
    return flags
}

fun statusCliff(flags: Int): Boolean = (flags and (1 shl CLIFF_BIT)) != 0

fun statusDriving(flags: Int): Boolean = (flags and (1 shl DRIVING_BIT)) != 0

fun statusMode(flags: Int): RobotMode = RobotMode.fromBits((flags shr MODE_SHIFT) and MODE_MASK)

/** True while the robot reports MANUAL mode active (statusFlags bit 4). */
fun statusManual(flags: Int): Boolean = (flags and (1 shl MANUAL_BIT)) != 0

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
 *
 * Legacy post-pair: the firmware drops `0xAA` drive datagrams once paired,
 * so NOTHING in the app sends through [encode] anymore — bench/test frame
 * builder, not for drive path (telemetry-garbage fixtures, hello shape).
 * All drive traffic goes through [encodeSigned].
 */
object ControlPacketCodec {
    internal fun encode(cmd: ControlCommand): ByteArray {
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
 * Authenticated drive frame: `DRIVE`, 16 bytes
 * `[0xAC | mode | thr | str | flags | counter 4B BE | tag 6B | XOR]`
 * (frozen spec: `shared/protocol/README.md` §"Pairing + authenticated
 * drive v1").
 *
 * Sticks/mode/flags bytes sit at the same positions as the legacy
 * layout; `counter` (bytes 5..8, big-endian) is 1 for the first wire
 * packet and strictly increasing after (0 is never valid on the wire);
 * `tag = driveTag(psk, counter, mode, thr, str, flags)`; the trailing
 * byte is the XOR over bytes 0..14.
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (house rule — `object` suspend members break
 * the iOS framework build).
 *
 * Suspend: the tag needs the platform HMAC backend ([driveTag]).
 *
 * @param psk 16-byte pair PSK.
 */
suspend fun encodeSigned(cmd: ControlCommand, counter: UInt, psk: ByteArray): ByteArray {
    require(psk.size == 16) { "encodeSigned: psk must be 16 bytes" }
    val out = ByteArray(DRIVE_SIZE)
    out[0] = DRIVE_HEADER.toByte()
    val mode = cmd.mode.toBits().toByte()
    val thr = unitToByte(cmd.throttle)
    val str = unitToByte(cmd.steering)
    val flags = cmd.flags.toByte()
    out[1] = mode
    out[2] = thr
    out[3] = str
    out[4] = flags
    out[5] = (counter shr 24).toByte()
    out[6] = (counter shr 16).toByte()
    out[7] = (counter shr 8).toByte()
    out[8] = counter.toByte()
    driveTag(psk, counter, mode, thr, str, flags).copyInto(out, destinationOffset = 9)
    out[DRIVE_SIZE - 1] = xorChecksum(out, DRIVE_SIZE - 1)
    return out
}

/**
 * Pairing packet codec (frozen spec: `shared/protocol/README.md`
 * §"Pairing + authenticated drive v1").
 *
 * - `PAIR_REQ` App -> Robot, 12 bytes:
 *   `[0x50 | code 6B ASCII | client_nonce 4B | XOR]`.
 * - `PAIR_OK` Robot -> App, 38 bytes (v1) or 39 (v1.1):
 *   `[0x51 | server_nonce 4B | ct 16/17B | gcm_tag 16B | XOR]`
 *   (v1.1 ct covers `psk16 || persist_u8`).
 * - `PAIR_ERR` Robot -> App, 4 bytes:
 *   `[0x52 | reason 1B | retry_s 1B | XOR]`
 *   (reason: 0=bad_code, 1=locked, 2=not_in_window, 3=already_paired).
 *   M11 additive reason 4=internal_retry_fresh ("robot error, use fresh
 *   code"): reasons 0-3 frozen, never renumber; 2 keeps not_in_window.
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
     * Null on wrong header, wrong length, or bad checksum. Accepts both the
     * v1 (38B, ct 16B) and v1.1 (39B, ct 17B) frames; on success the
     * `Triple` is `(server_nonce 4B, ct 16/17B, gcm_tag 16B)` — split the
     * decrypted plaintext with `unwrapPairPlaintext` (v1 defaults the
     * interval to [ToRobotCounter.COUNTER_PERSIST_EVERY]).
     */
    fun decodePairOk(bytes: ByteArray): Triple<ByteArray, ByteArray, ByteArray>? {
        if (bytes.size != PAIR_OK_SIZE && bytes.size != PAIR_OK_SIZE_V11) return null
        if ((bytes[0].toInt() and 0xFF) != PAIR_OK_HEADER) return null
        if (xorChecksum(bytes, bytes.size - 1) != bytes[bytes.size - 1]) return null
        val ctLen = bytes.size - /* header + nonce + tag + xor */ (1 + PAIR_NONCE_SIZE + 16 + 1)
        val serverNonce = bytes.copyOfRange(1, 1 + PAIR_NONCE_SIZE)
        val ct = bytes.copyOfRange(5, 5 + ctLen)
        val tag = bytes.copyOfRange(5 + ctLen, 5 + ctLen + 16)
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
 * Signed unpair frame: App -> Robot, 12 bytes
 * `[0xAD | counter 4B BE | tag 6B | XOR]`.
 *
 * Tells a paired robot to drop the pair. Fire-and-forget: the firmware
 * never replies, so the app sends it (up to 3x, see [ForgetDeviceUseCase])
 * and always proceeds to the local forget regardless of send outcome.
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned] above).
 *
 * Suspend: the tag needs the platform HMAC backend ([unpairTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param counter next drive counter (`lastCounter + 1`; strictly increasing
 *   like every other signed frame).
 */
suspend fun encodeUnpair(psk: ByteArray, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeUnpair: psk must be 16 bytes" }
    val out = ByteArray(UNPAIR_SIZE)
    out[0] = UNPAIR_HEADER.toByte()
    out[1] = (counter shr 24).toByte()
    out[2] = (counter shr 16).toByte()
    out[3] = (counter shr 8).toByte()
    out[4] = counter.toByte()
    unpairTag(psk, counter).copyInto(out, destinationOffset = 5)
    out[UNPAIR_SIZE - 1] = xorChecksum(out, UNPAIR_SIZE - 1)
    return out
}

/**
 * Tier-1 silent auto-pair-refresh frames (see `shared/protocol/README.md`
 * §"Tier-1 silent auto-pair-refresh").
 *
 * - `PAIR_REFRESH_REQ` App -> Robot, 14 bytes:
 *   `[0x53 | eraBE2 | counterBE4 | tag6 | XOR]`
 *   (`tag = pairRefreshTag(oldPsk, nextEra, counter)`).
 * - `PAIR_REFRESH_ACK` Robot -> App, 10 bytes:
 *   `[0x54 | eraBE2 | tag6 | XOR]`
 *   (`tag = pairRefreshAckTag(newPsk, nextEra)`).
 *
 * Pure byte math mirroring firmware `pairing::pairRefreshTagMsg` /
 * `pairRefreshAckTagMsg` + `UdpServer::handlePairRefresh`/`sendPairRefreshAck`.
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 */
suspend fun encodePairRefreshReq(oldPsk: ByteArray, nextEra: UShort, counter: UInt): ByteArray {
    require(oldPsk.size == 16) { "encodePairRefreshReq: oldPsk must be 16 bytes" }
    val out = ByteArray(PAIR_REFRESH_REQ_SIZE)
    out[0] = PAIR_REFRESH_REQ_HEADER.toByte()
    out[1] = ((nextEra.toInt() shr 8) and 0xFF).toByte()
    out[2] = (nextEra.toInt() and 0xFF).toByte()
    out[3] = (counter shr 24).toByte()
    out[4] = (counter shr 16).toByte()
    out[5] = (counter shr 8).toByte()
    out[6] = counter.toByte()
    pairRefreshTag(oldPsk, nextEra, counter).copyInto(out, destinationOffset = 7)
    out[PAIR_REFRESH_REQ_SIZE - 1] = xorChecksum(out, PAIR_REFRESH_REQ_SIZE - 1)
    return out
}

/**
 * Decode a `PAIR_REFRESH_ACK` frame: null on wrong header, wrong length, or bad
 * checksum. On success the `Pair` is `(era, tag6)` — verify the tag with
 * [pairRefreshAckTag] under the derived [pairRefreshPsk] before trusting the era.
 */
fun decodePairRefreshAck(bytes: ByteArray): Pair<UShort, ByteArray>? {
    if (bytes.size != PAIR_REFRESH_ACK_SIZE) return null
    if ((bytes[0].toInt() and 0xFF) != PAIR_REFRESH_ACK_HEADER) return null
    if (xorChecksum(bytes, PAIR_REFRESH_ACK_SIZE - 1) != bytes[PAIR_REFRESH_ACK_SIZE - 1]) return null
    val era = (((bytes[1].toInt() and 0xFF) shl 8) or (bytes[2].toInt() and 0xFF)).toUShort()
    val tag = bytes.copyOfRange(3, 3 + 6)
    return era to tag
}

/**
 * `ALIVE` keepalive frame: App -> Robot, 14 bytes
 * `[0x55 | eraBE2 | counterBE4 | tag6 | XOR]`
 * (`tag = aliveTag(psk, era, counter)`).
 *
 * Fire-and-forget: the robot never replies — steady telemetry is the ACK
 * (see `shared/protocol/README.md` §"Robot alive rule": every valid paired
 * packet refreshes the 5 s alive window, so a 2 Hz `ALIVE` keeps telemetry
 * flowing exactly like 50 Hz drive, without touching the drive effect).
 * No decode side exists by design.
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 *
 * Suspend: the tag needs the platform HMAC backend ([aliveTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param era CURRENT paired era (stored era; a pair-refresh moves it).
 * @param counter next drive counter from the shared [ToRobotCounter]
 *   (strictly increasing like every other signed frame).
 */
suspend fun encodeAlive(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeAlive: psk must be 16 bytes" }
    val out = ByteArray(ALIVE_SIZE)
    out[0] = ALIVE_HEADER.toByte()
    out[1] = ((era.toInt() shr 8) and 0xFF).toByte()
    out[2] = (era.toInt() and 0xFF).toByte()
    out[3] = (counter shr 24).toByte()
    out[4] = (counter shr 16).toByte()
    out[5] = (counter shr 8).toByte()
    out[6] = counter.toByte()
    aliveTag(psk, era, counter).copyInto(out, destinationOffset = 7)
    out[ALIVE_SIZE - 1] = xorChecksum(out, ALIVE_SIZE - 1)
    return out
}

/**
 * `DISCONNECT` goodbye frame: App -> Robot, 14 bytes
 * `[0x56 | eraBE2 | counterBE4 | tag6 | XOR]`
 * (`tag = disconnectTag(psk, era, counter)`).
 *
 * Fire-and-forget like [encodeAlive] (no reply ever comes back): the robot
 * runs refresh-then-clear (net peer dropped, pair KEPT — the next launch
 * rejoins instantly). The app sends it 2x on backgrounding (same
 * byte-identical repeat shape as `UNPAIR`'s 3x in [ForgetDeviceUseCase]).
 * Decode side exists only for the reverse direction: the robot's physical
 * pair button sends one `DISCONNECT` (same layout) as a pair eviction —
 * see [decodeDisconnect].
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 *
 * Suspend: the tag needs the platform HMAC backend ([disconnectTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param era CURRENT paired era.
 * @param counter next drive counter from the shared [ToRobotCounter].
 */
suspend fun encodeDisconnect(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeDisconnect: psk must be 16 bytes" }
    val out = ByteArray(DISCONNECT_SIZE)
    out[0] = DISCONNECT_HEADER.toByte()
    out[1] = ((era.toInt() shr 8) and 0xFF).toByte()
    out[2] = (era.toInt() and 0xFF).toByte()
    out[3] = (counter shr 24).toByte()
    out[4] = (counter shr 16).toByte()
    out[5] = (counter shr 8).toByte()
    out[6] = counter.toByte()
    disconnectTag(psk, era, counter).copyInto(out, destinationOffset = 7)
    out[DISCONNECT_SIZE - 1] = xorChecksum(out, DISCONNECT_SIZE - 1)
    return out
}

/**
 * `MANUAL_TAKE` mode-entry frame: App -> Robot, 14 bytes
 * `[0x57 | eraBE2 | counterBE4 | tag6 | XOR]`
 * (`tag = manualTakeTag(psk, era, counter)`).
 *
 * Fire-and-forget: the robot never replies — the telemetry `statusFlags`
 * MANUAL bit (bit 4) is the ACK. The app sends it once per user action
 * (entry is deliberate; repeats add nothing). No decode side exists.
 * See `shared/protocol/README.md` §"Pairing + authenticated drive v1" (MANUAL take/release).
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 *
 * Suspend: the tag needs the platform HMAC backend ([manualTakeTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param era CURRENT paired era.
 * @param counter next drive counter from the shared [ToRobotCounter].
 */
suspend fun encodeManualTake(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeManualTake: psk must be 16 bytes" }
    val out = ByteArray(MANUAL_TAKE_SIZE)
    out[0] = MANUAL_TAKE_HEADER.toByte()
    out[1] = ((era.toInt() shr 8) and 0xFF).toByte()
    out[2] = (era.toInt() and 0xFF).toByte()
    out[3] = (counter shr 24).toByte()
    out[4] = (counter shr 16).toByte()
    out[5] = (counter shr 8).toByte()
    out[6] = counter.toByte()
    manualTakeTag(psk, era, counter).copyInto(out, destinationOffset = 7)
    out[MANUAL_TAKE_SIZE - 1] = xorChecksum(out, MANUAL_TAKE_SIZE - 1)
    return out
}

/**
 * `MANUAL_RELEASE` mode-exit frame: App -> Robot, 14 bytes
 * `[0x58 | eraBE2 | counterBE4 | tag6 | XOR]`
 * (`tag = manualReleaseTag(psk, era, counter)`).
 *
 * Fire-and-forget like [encodeManualTake] (no reply ever comes back): the
 * app sends it 2x byte-identical (same counter — one allocation, like
 * `DISCONNECT`'s 2x in `RobotControlViewModel`) on user release.
 * See `shared/protocol/README.md` §"Pairing + authenticated drive v1" (MANUAL take/release).
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 *
 * Suspend: the tag needs the platform HMAC backend ([manualReleaseTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param era CURRENT paired era.
 * @param counter next drive counter from the shared [ToRobotCounter].
 */
suspend fun encodeManualRelease(psk: ByteArray, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeManualRelease: psk must be 16 bytes" }
    val out = ByteArray(MANUAL_RELEASE_SIZE)
    out[0] = MANUAL_RELEASE_HEADER.toByte()
    out[1] = ((era.toInt() shr 8) and 0xFF).toByte()
    out[2] = (era.toInt() and 0xFF).toByte()
    out[3] = (counter shr 24).toByte()
    out[4] = (counter shr 16).toByte()
    out[5] = (counter shr 8).toByte()
    out[6] = counter.toByte()
    manualReleaseTag(psk, era, counter).copyInto(out, destinationOffset = 7)
    out[MANUAL_RELEASE_SIZE - 1] = xorChecksum(out, MANUAL_RELEASE_SIZE - 1)
    return out
}

/**
 * `EXPR_TRIGGER` play-request frame: App -> Robot, 15 bytes
 * `[0x5B | id | eraBE2 | counterBE4 | tag6 | XOR]`
 * (`tag = exprTriggerTag(psk, id, era, counter)`).
 *
 * Fire-and-forget, one send per tap: the robot gates on MANUAL and
 * replies nothing (a robot-side drop is silent by protocol). Callers
 * reject unknown ids client-side against the static catalog before
 * sending (see `TriggerExpressionUseCase`).
 *
 * Top-level (not an `object` member) per the SKIE constraint documented in
 * `client/ARCHITECTURE.md` (same reason as [encodeSigned]/[encodeUnpair]).
 *
 * Suspend: the tag needs the platform HMAC backend ([exprTriggerTag]).
 *
 * @param psk 16-byte pair PSK.
 * @param id catalog expression id (0..255).
 * @param era CURRENT paired era.
 * @param counter next drive counter from the shared [ToRobotCounter].
 */
suspend fun encodeExprTrigger(psk: ByteArray, id: Int, era: UShort, counter: UInt): ByteArray {
    require(psk.size == 16) { "encodeExprTrigger: psk must be 16 bytes" }
    require(id in 0..255) { "encodeExprTrigger: id out of range: $id" }
    val out = ByteArray(EXPR_TRIGGER_SIZE)
    out[0] = EXPR_TRIGGER_HEADER.toByte()
    out[1] = id.toByte()
    out[2] = ((era.toInt() shr 8) and 0xFF).toByte()
    out[3] = (era.toInt() and 0xFF).toByte()
    out[4] = (counter shr 24).toByte()
    out[5] = (counter shr 16).toByte()
    out[6] = (counter shr 8).toByte()
    out[7] = counter.toByte()
    exprTriggerTag(psk, id, era, counter).copyInto(out, destinationOffset = 8)
    out[EXPR_TRIGGER_SIZE - 1] = xorChecksum(out, EXPR_TRIGGER_SIZE - 1)
    return out
}

/**
 * Decode a core-originated `DISCONNECT` eviction frame: null on wrong
 * header, wrong length, or bad checksum. On success the `Triple` is
 * `(era, counter, tag6)` — the caller must still verify the era against
 * the stored era, the counter for wrap-aware freshness, and the tag with
 * [disconnectTag] under the vault PSK (constant-time) before trusting it.
 * See `shared/protocol/README.md` §"ALIVE + DISCONNECT + robot alive rule".
 */
fun decodeDisconnect(bytes: ByteArray): Triple<UShort, UInt, ByteArray>? {    if (bytes.size != DISCONNECT_SIZE) return null
    if ((bytes[0].toInt() and 0xFF) != DISCONNECT_HEADER) return null
    if (xorChecksum(bytes, DISCONNECT_SIZE - 1) != bytes[DISCONNECT_SIZE - 1]) return null
    val era = (((bytes[1].toInt() and 0xFF) shl 8) or (bytes[2].toInt() and 0xFF)).toUShort()
    val counter = (((bytes[3].toUInt() and 0xFFu) shl 24) or
        ((bytes[4].toUInt() and 0xFFu) shl 16) or
        ((bytes[5].toUInt() and 0xFFu) shl 8) or
        (bytes[6].toUInt() and 0xFFu))
    val tag = bytes.copyOfRange(7, 7 + 6)
    return Triple(era, counter, tag)
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
        out[7] = packStatusFlags(t.cliff, t.driving, t.mode, t.manual).toByte()
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
            manual = statusManual(flags),
        )
    }
}
