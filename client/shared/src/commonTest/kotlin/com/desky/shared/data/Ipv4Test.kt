package com.desky.shared.data

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class Ipv4Test {

    @Test
    fun parseKnownAddressesToNetworkOrder() {
        // 192.168.4.1 on the wire is C0 A8 04 01; as a LE-stored UInt that is 0x0104A8C0.
        assertEquals(0x0104A8C0u, parseIpv4ToNetworkOrder("192.168.4.1"))
        assertEquals(0x04030201u, parseIpv4ToNetworkOrder("1.2.3.4"))
        assertEquals(0xFFFFFFFFu, parseIpv4ToNetworkOrder("255.255.255.255"))
        assertEquals(0u, parseIpv4ToNetworkOrder("0.0.0.0"))
    }

    @Test
    fun formatKnownNetworkOrderValues() {
        assertEquals("192.168.4.1", formatIpv4FromNetworkOrder(0x0104A8C0u))
        assertEquals("1.2.3.4", formatIpv4FromNetworkOrder(0x04030201u))
        assertEquals("255.255.255.255", formatIpv4FromNetworkOrder(0xFFFFFFFFu))
        assertEquals("0.0.0.0", formatIpv4FromNetworkOrder(0u))
    }

    @Test
    fun parseFormatRoundTrip() {
        listOf("192.168.4.1", "10.0.0.2", "8.8.8.8", "127.0.0.1", "192.168.4.255").forEach { ip ->
            assertEquals(ip, formatIpv4FromNetworkOrder(parseIpv4ToNetworkOrder(ip)))
        }
    }

    @Test
    fun parseRejectsInvalid() {
        assertFailsWith<IllegalArgumentException> { parseIpv4ToNetworkOrder("999.1.1.1") }
        assertFailsWith<IllegalArgumentException> { parseIpv4ToNetworkOrder("1.2.3") }
        assertFailsWith<IllegalArgumentException> { parseIpv4ToNetworkOrder("desky.local") }
        assertFailsWith<IllegalArgumentException> { parseIpv4ToNetworkOrder("") }
    }

    @Test
    fun portByteSwapRoundTrip() {
        listOf(0, 1, 3333, 8080, 65535).forEach { port ->
            val wire = hostPortToNetworkOrder(port)
            assertEquals(port, networkOrderToHostPort(wire))
        }
        // 3333 = 0x0D05 -> wire 0x050D.
        assertEquals(0x050Du.toUShort(), hostPortToNetworkOrder(3333))
        assertFailsWith<IllegalArgumentException> { hostPortToNetworkOrder(65536) }
        assertFailsWith<IllegalArgumentException> { hostPortToNetworkOrder(-1) }
    }
}
