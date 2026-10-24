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
    fun drive() {
        assertEquals(0xAC, DRIVE_HEADER) // udpstatus::kDriveHeader
        assertEquals(16, DRIVE_SIZE) // udpstatus::kDriveSize
    }

    @Test
    fun unpair() {
        assertEquals(0xAD, UNPAIR_HEADER) // udpstatus::kUnpairHeader
        assertEquals(12, UNPAIR_SIZE) // udpstatus::kUnpairSize
    }

    @Test
    fun pairRefresh() {
        assertEquals(0x53, PAIR_REFRESH_REQ_HEADER) // udpstatus::kPairRefreshReqHeader
        assertEquals(14, PAIR_REFRESH_REQ_SIZE) // udpstatus::kPairRefreshReqSize
        assertEquals(0x54, PAIR_REFRESH_ACK_HEADER) // udpstatus::kPairRefreshAckHeader
        assertEquals(10, PAIR_REFRESH_ACK_SIZE) // udpstatus::kPairRefreshAckSize
    }

    @Test
    fun aliveDisconnect() {
        assertEquals(0x55, ALIVE_HEADER) // udpstatus::kAliveHeader
        assertEquals(14, ALIVE_SIZE) // udpstatus::kAliveSize
        assertEquals(0x56, DISCONNECT_HEADER) // udpstatus::kDisconnectHeader
        assertEquals(14, DISCONNECT_SIZE) // udpstatus::kDisconnectSize
    }

    @Test
    fun manualTakeRelease() {
        assertEquals(0x57, MANUAL_TAKE_HEADER)
        assertEquals(14, MANUAL_TAKE_SIZE)
        assertEquals(0x58, MANUAL_RELEASE_HEADER)
        assertEquals(14, MANUAL_RELEASE_SIZE)
    }

    @Test
    fun expressions() {
        // 0x59/0x5A stay claimed retired-reserved (never reuse); 0x5B is live.
        assertEquals(0x59, EXPR_LIST_REQ_HEADER)
        assertEquals(0x5A, EXPR_LIST_RESP_HEADER)
        assertEquals(0x5B, EXPR_TRIGGER_HEADER)
        assertEquals(15, EXPR_TRIGGER_SIZE)
    }

    @Test
    fun claimedHeadersDistinct() {
        val headers = listOf(CONTROL_HEADER, TELEMETRY_HEADER, PAIR_REQ_HEADER, PAIR_OK_HEADER, PAIR_ERR_HEADER, DRIVE_HEADER, UNPAIR_HEADER, PAIR_REFRESH_REQ_HEADER, PAIR_REFRESH_ACK_HEADER, ALIVE_HEADER, DISCONNECT_HEADER, MANUAL_TAKE_HEADER, MANUAL_RELEASE_HEADER, EXPR_LIST_REQ_HEADER, EXPR_LIST_RESP_HEADER, EXPR_TRIGGER_HEADER)
        assertEquals(headers.size, headers.toSet().size) // never reuse/renumber
        assertTrue(headers.none { it == 0x00 }) // 0x00 stays unclaimed
    }
}
