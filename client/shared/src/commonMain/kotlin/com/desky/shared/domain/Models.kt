package com.desky.shared.domain

/**
 * Robot operating mode. Wire values mirror the firmware
 * (`SystemState::RobotMode` / `udpstatus` mode bits 2-3): 0..3.
 */
enum class RobotMode(val bits: Int) {
    MANUAL(0),
    AUTONOMOUS(1),
    LOW_POWER(2),
    EMERGENCY(3);

    fun toBits(): Int = bits

    companion object {
        /** Strict 0..3 mapping; throws [IllegalArgumentException] otherwise. */
        fun fromBits(bits: Int): RobotMode {
            require(bits in 0..3) { "RobotMode bits out of range: $bits" }
            return entries.first { it.bits == bits }
        }
    }
}

/**
 * App -> Robot drive intent. Throttle/steering are unit floats in -1..1
 * (0 = centered stick); [flags] is an opaque wire byte (0..255).
 */
data class ControlCommand(
    val mode: RobotMode,
    val throttle: Float,
    val steering: Float,
    val flags: Int = 0,
) {
    init {
        require(throttle in -1f..1f) { "throttle out of range: $throttle" }
        require(steering in -1f..1f) { "steering out of range: $steering" }
        require(flags in 0..255) { "flags out of range: $flags" }
    }
}

/**
 * Robot -> App sensor snapshot. Pitch/roll are degrees (the decidegree/10
 * fixed-point conversion lives in the codec); [distanceMm] is millimetres.
 */
data class Telemetry(
    val pitchDeg: Float,
    val rollDeg: Float,
    val distanceMm: Int,
    val cliff: Boolean,
    val driving: Boolean,
    val mode: RobotMode,
) {
    init {
        require(distanceMm in 0..65535) { "distanceMm out of range: $distanceMm" }
    }
}

/**
 * A robot the app is paired with. [authToken] is a version marker (`"psk-v1"`)
 * since pairing/auth v1 — the bond secret itself lives only in the platform
 * vault ([BondSecretStore]) since Task 8, never in Room. [lastCounter] is the
 * last drive counter persisted to Room (Task 9: saved every 128 sends,
 * mirroring firmware A1); the drive session resumes from it, so the first
 * wire packet after a fresh pair uses counter 1. [persistEvery] is the
 * counter-persist cadence negotiated once at pair (v1.1 `PAIR_OK` interval
 * byte, sanitized 8..1024 else 128; a v1 robot sends no byte and the app
 * defaults to 128) — Room-persisted (v4 column) so the drive session keeps
 * the negotiated cadence across restarts. [era] is the Tier-1 rekey epoch
 * (u16, fresh pair = 0, Room-persisted v5 column + firmware NVS "bera"):
 * every silent era+PSK rotation increments it and resets [lastCounter] to
 * the bond baseline (0 persisted, first post-rekey drive uses 1). Never
 * shown in UI state — domain/session fields only.
 */
data class PairedDevice(
    val id: String,
    val ip: String,
    val port: Int = DEFAULT_UDP_PORT,
    val authToken: String,
    val lastCounter: UInt = 0u,
    val alias: String = ip,
    val persistEvery: Int = DriveCounter.COUNTER_PERSIST_EVERY,
    val era: UShort = 0u,
)

/** Plain robot settings holder (persistence lives elsewhere). */
data class RobotConfig(
    /** T5: defaults to full speed so `RobotConfig()` is the unset-config default. */
    val speedLimit: Float = 1f,
    val ledProfile: Int = 0,
    val pidP: Float = 0f,
    val pidI: Float = 0f,
    val pidD: Float = 0f,
    val wifiSsid: String = "",
) {
    init {
        require(speedLimit in 0f..1f) { "speedLimit out of range: $speedLimit" }
    }
}
