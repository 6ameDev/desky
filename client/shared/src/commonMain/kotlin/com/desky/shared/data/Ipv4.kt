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
 * is why the bug hides until unicast to a literal fails).
 *
 * Only dotted-quad literals are parsed here — hostname resolution (mDNS)
 * lives in the socket send paths (`getaddrinfo` on iOS,
 * `InetAddress.getByName` on Android/JVM), which call this only for the
 * literal fast path.
 */

/** Dotted quad (e.g. `192.168.4.1`) to network-order UInt32. */
internal fun parseIpv4ToNetworkOrder(host: String): UInt {
    val parts = host.split('.')
    require(parts.size == OCTET_COUNT) { "Invalid IPv4 address: $host" }
    var addr = 0u
    for ((i, part) in parts.withIndex()) {
        val octet = part.toUIntOrNull()
        require(octet != null && octet <= MAX_OCTET) { "Invalid IPv4 address: $host" }
        addr = addr or (octet shl (i * BITS_PER_OCTET))
    }
    return addr
}

/** Network-order UInt32 to dotted quad. */
internal fun formatIpv4FromNetworkOrder(addr: UInt): String =
    "${addr and OCTET_MASK_U}.${(addr shr SECOND_OCTET_SHIFT) and OCTET_MASK_U}.${(addr shr THIRD_OCTET_SHIFT) and OCTET_MASK_U}.${(addr shr FOURTH_OCTET_SHIFT) and OCTET_MASK_U}"

/** Host-order port (0..65535) to network-order 16-bit. Manual `htons`. */
internal fun hostPortToNetworkOrder(port: Int): UShort {
    require(port in MIN_PORT..MAX_PORT) { "port out of range: $port" }
    return (((port and BYTE_MASK) shl BITS_PER_OCTET) or ((port ushr BITS_PER_OCTET) and BYTE_MASK)).toUShort()
}

/** Network-order 16-bit to host-order port. Manual `ntohs`. */
internal fun networkOrderToHostPort(raw: UShort): Int {
    val v = raw.toInt()
    return ((v and BYTE_MASK) shl BITS_PER_OCTET) or ((v ushr BITS_PER_OCTET) and BYTE_MASK)
}

private const val OCTET_COUNT = 4
private const val BITS_PER_OCTET = 8
private const val SECOND_OCTET_SHIFT = 8
private const val THIRD_OCTET_SHIFT = 16
private const val FOURTH_OCTET_SHIFT = 24
private const val BYTE_MASK = 0xFF
private const val MIN_PORT = 0
private const val MAX_PORT = 65535
private val OCTET_MASK_U = 0xFFu
private val MAX_OCTET = 255u
