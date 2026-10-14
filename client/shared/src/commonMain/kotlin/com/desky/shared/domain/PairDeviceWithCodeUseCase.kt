package com.desky.shared.domain

/**
 * Pair with a robot given its address and a short numeric PIN shown to the
 * user. The token is derived client-locally as
 * `sha256Hex("$ip|$pin|desky-v1")`.
 *
 * TODO(firmware-auth): the token is client-local until the robot gains a
 * challenge/response handshake (firmware today is open UDP, no auth).
 */
class PairDeviceWithCodeUseCase(
    private val deviceRepo: DeviceRepository,
) {
    suspend operator fun invoke(ip: String, pin: String): Outcome<PairedDevice> {
        val host = ip.trim()
        val code = pin.trim()
        val hostFailure = validateHost(host)
        if (hostFailure != null) return hostFailure
        val pinFailure = validatePin(code)
        if (pinFailure != null) return pinFailure
        val device = PairedDevice(
            id = "desky-$host",
            ip = host,
            port = DEFAULT_UDP_PORT,
            authToken = sha256Hex("$host|$code|desky-v1".encodeToByteArray()),
        )
        return try {
            deviceRepo.save(device)
            Outcome.Success(device)
        } catch (e: Exception) {
            Outcome.Failure("Failed to persist paired device: ${e.message}", e)
        }
    }

    private fun validateHost(host: String): Outcome.Failure? {
        if (host.isEmpty()) return Outcome.Failure("IP/hostname must not be blank")
        if (host.length > 253) return Outcome.Failure("IP/hostname too long (max 253 chars)")
        // All-digit-and-dot input must be a valid IPv4 address, never a hostname.
        if (host.all { it.isDigit() || it == '.' }) {
            if (!isValidIpv4(host)) {
                return Outcome.Failure("Invalid IPv4 address: \"$host\" (expected four octets 0..255)")
            }
            return null
        }
        if (!isValidHostname(host)) {
            return Outcome.Failure(
                "Invalid hostname: \"$host\" " +
                    "(1..253 chars; letters, digits, dots, hyphens; no empty labels)",
            )
        }
        return null
    }

    private fun validatePin(code: String): Outcome.Failure? {
        if (!code.all { it in '0'..'9' }) {
            return Outcome.Failure("PIN must contain ASCII digits only")
        }
        if (code.length !in 4..8) {
            return Outcome.Failure("PIN must be 4..8 digits (got ${code.length})")
        }
        return null
    }

    private fun isValidIpv4(host: String): Boolean {
        val parts = host.split('.')
        if (parts.size != 4) return false
        for (p in parts) {
            if (p.isEmpty() || p.length > 3 || !p.all { it.isDigit() }) return false
            val v = p.toIntOrNull() ?: return false
            if (v !in 0..255) return false
        }
        return true
    }

    private fun isValidHostname(host: String): Boolean {
        if (host.isEmpty() || host.length > 253) return false
        if (!host.all { it.isLetterOrDigit() || it == '.' || it == '-' }) return false
        // Non-ASCII letters are rejected: hostnames are LDH (letters/digits/hyphen).
        if (!host.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '.' || it == '-' }) {
            return false
        }
        if (host.startsWith('.') || host.startsWith('-') || host.endsWith('.') || host.endsWith('-')) {
            return false
        }
        val labels = host.split('.')
        for (label in labels) {
            if (label.isEmpty() || label.length > 63) return false
            if (label.startsWith('-') || label.endsWith('-')) return false
        }
        return true
    }
}
