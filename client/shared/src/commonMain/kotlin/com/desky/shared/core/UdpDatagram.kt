package com.desky.shared.core

/**
 * A raw UDP datagram as seen by the domain layer: payload plus source.
 * [bytes] participates in equality by content ([ByteArray.contentEquals] /
 * [contentHashCode]) so datagrams compare by value in tests and fakes.
 */
data class UdpDatagram(
    val bytes: ByteArray,
    val host: String,
    val port: Int,
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is UdpDatagram) return false
        return bytes.contentEquals(other.bytes) && host == other.host && port == other.port
    }

    override fun hashCode(): Int {
        var result = bytes.contentHashCode()
        result = 31 * result + host.hashCode()
        result = 31 * result + port
        return result
    }
}
