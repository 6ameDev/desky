package com.desky.shared.data

/**
 * Pure-Kotlin IPv4 literal <-> network-order conversions shared by the
 * Kotlin/Native POSIX socket actual (and any future raw-socket code).
 *
 * All Apple platforms (and Android, and the JVM hosts we build on) are
 * little-endian, so "network order" here means: the UInt value whose
 * little-endian memory image is the on-the-wire byte sequence
 * `o1 o2 o3 o4`. Concretely `parseIpv4ToNetworkOrder("192.168.4.1")` is
 * `0x0104A8C0u`, whose LE memory image is `C0 A8 04 01` — exactly what
 * `sin_addr.s_addr` must contain. Do NOT "fix" this by shifting the first
 * octet to the top: that yields host order and byte-reverses every
 * non-palindromic address on the wire (255.255.255.255 still works, which
 * is why the bug hides until unicast to 192.168.4.1 fails).
 *
 * Only dotted-quad literals are supported — no DNS in v1 scope.
 */

/** Dotted quad (e.g. `192.168.4.1`) to network-order UInt32. */
internal fun parseIpv4ToNetworkOrder(host: String): UInt {
    val parts = host.split('.')
    require(parts.size == 4) { "Invalid IPv4 address: $host" }
    var addr = 0u
    for ((i, part) in parts.withIndex()) {
        val octet = part.toUIntOrNull()
        require(octet != null && octet <= 255u) { "Invalid IPv4 address: $host" }
        addr = addr or (octet shl (i * 8))
    }
    return addr
}

/** Network-order UInt32 to dotted quad. */
internal fun formatIpv4FromNetworkOrder(addr: UInt): String =
    "${addr and 0xFFu}.${(addr shr 8) and 0xFFu}.${(addr shr 16) and 0xFFu}.${(addr shr 24) and 0xFFu}"

/** Host-order port (0..65535) to network-order 16-bit. Manual `htons`. */
internal fun hostPortToNetworkOrder(port: Int): UShort {
    require(port in 0..65535) { "port out of range: $port" }
    return (((port and 0xFF) shl 8) or ((port ushr 8) and 0xFF)).toUShort()
}

/** Network-order 16-bit to host-order port. Manual `ntohs`. */
internal fun networkOrderToHostPort(raw: UShort): Int {
    val v = raw.toInt()
    return ((v and 0xFF) shl 8) or ((v ushr 8) and 0xFF)
}
