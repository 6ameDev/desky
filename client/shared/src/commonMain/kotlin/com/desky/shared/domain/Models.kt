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
 * since pairing/auth v1 — the bond secret itself lives in the platform vault
 * ([BondSecretStore]) since Task 8. [pskHex] (lowercase hex of the 16-byte
 * PSK) is legacy: always null on rows written by [PairDeviceWithCodeUseCase],
 * non-null only on pre-Task-8 rows until [GetBondPskUseCase] migrates them.
 */
data class PairedDevice(
    val id: String,
    val ip: String,
    val port: Int = DEFAULT_UDP_PORT,
    val authToken: String,
    val pskHex: String? = null,
    val alias: String = ip,
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
