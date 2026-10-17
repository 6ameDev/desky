package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * M9 three-way mirror gate: literal pin of every frozen header/size.
 * Mirrors `udp_codec.h` freeze table + `shared/protocol/README.md` §v1.
 * Any renumber/resize must break this test, not silently pass.
 */
class WireConstantsTest {

    @Test
    fun control() {
        assertEquals(0xAA, CONTROL_HEADER) // udp::kControlHeader
        assertEquals(6, CONTROL_SIZE) // udp::kControlSize
    }

    @Test
    fun telemetry() {
        assertEquals(0xBB, TELEMETRY_HEADER) // udp::kTelemetryHeader
        assertEquals(9, TELEMETRY_SIZE) // udp::kTelemetrySize
    }

    @Test
    fun pairReq() {
        assertEquals(0x50, PAIR_REQ_HEADER) // udpstatus::kPairReqHeader
        assertEquals(12, PAIR_REQ_SIZE) // udpstatus::kPairReqSize
    }

    @Test
    fun pairOk() {
        assertEquals(0x51, PAIR_OK_HEADER) // pairing::kPairOkHeader (fw-only)
        assertEquals(38, PAIR_OK_SIZE) // pairing::kPairOkLegacySize (v1)
        assertEquals(39, PAIR_OK_SIZE_V11) // pairing::kPairOkSize (v1.1)
    }

    @Test
    fun pairErr() {
        assertEquals(0x52, PAIR_ERR_HEADER) // fw sendPairErr literal
        assertEquals(4, PAIR_ERR_SIZE)
    }

    @Test
    fun driveV2() {
        assertEquals(0xAC, DRIVE_V2_HEADER) // udpstatus::kDriveV2Header
        assertEquals(16, DRIVE_V2_SIZE) // udpstatus::kDriveV2Size
    }

    @Test
    fun unbond() {
        assertEquals(0xAD, UNBOND_HEADER) // udpstatus::kUnbondHeader
        assertEquals(12, UNBOND_SIZE) // udpstatus::kUnbondSize
    }

    @Test
    fun rekey() {
        assertEquals(0x53, REKEY_REQ_HEADER) // udpstatus::kRekeyReqHeader
        assertEquals(14, REKEY_REQ_SIZE) // udpstatus::kRekeyReqSize
        assertEquals(0x54, REKEY_ACK_HEADER) // udpstatus::kRekeyAckHeader
        assertEquals(10, REKEY_ACK_SIZE) // udpstatus::kRekeyAckSize
    }

    @Test
    fun aliveDisconnect() {
        assertEquals(0x55, ALIVE_HEADER) // udpstatus::kAliveHeader
        assertEquals(14, ALIVE_SIZE) // udpstatus::kAliveSize
        assertEquals(0x56, DISCONNECT_HEADER) // udpstatus::kDisconnectHeader
        assertEquals(14, DISCONNECT_SIZE) // udpstatus::kDisconnectSize
    }

    @Test
    fun claimedHeadersDistinct() {
        val headers = listOf(CONTROL_HEADER, TELEMETRY_HEADER, PAIR_REQ_HEADER, PAIR_OK_HEADER, PAIR_ERR_HEADER, DRIVE_V2_HEADER, UNBOND_HEADER, REKEY_REQ_HEADER, REKEY_ACK_HEADER, ALIVE_HEADER, DISCONNECT_HEADER)
        assertEquals(headers.size, headers.toSet().size) // never reuse/renumber
        assertTrue(headers.none { it == 0x00 }) // 0x00 stays unclaimed
    }
}
