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

    private companion object {
        const val EXPECTED_CONTROL_HEADER = 0xAA
        const val EXPECTED_CONTROL_SIZE = 6
        const val EXPECTED_TELEMETRY_HEADER = 0xBB
        const val EXPECTED_TELEMETRY_SIZE = 9
        const val EXPECTED_PAIR_REQ_HEADER = 0x50
        const val EXPECTED_PAIR_REQ_SIZE = 12
        const val EXPECTED_PAIR_OK_HEADER = 0x51
        const val EXPECTED_PAIR_OK_SIZE = 38
        const val EXPECTED_PAIR_OK_SIZE_V11 = 39
        const val EXPECTED_PAIR_ERR_HEADER = 0x52
        const val EXPECTED_PAIR_ERR_SIZE = 4
        const val EXPECTED_DRIVE_HEADER = 0xAC
        const val EXPECTED_DRIVE_SIZE = 16
        const val EXPECTED_UNPAIR_HEADER = 0xAD
        const val EXPECTED_UNPAIR_SIZE = 12
        const val EXPECTED_PAIR_REFRESH_REQ_HEADER = 0x53
        const val EXPECTED_PAIR_REFRESH_REQ_SIZE = 14
        const val EXPECTED_PAIR_REFRESH_ACK_HEADER = 0x54
        const val EXPECTED_PAIR_REFRESH_ACK_SIZE = 10
        const val EXPECTED_ALIVE_HEADER = 0x55
        const val EXPECTED_ALIVE_SIZE = 14
        const val EXPECTED_DISCONNECT_HEADER = 0x56
        const val EXPECTED_DISCONNECT_SIZE = 14
        const val EXPECTED_MANUAL_TAKE_HEADER = 0x57
        const val EXPECTED_MANUAL_TAKE_SIZE = 14
        const val EXPECTED_MANUAL_RELEASE_HEADER = 0x58
        const val EXPECTED_MANUAL_RELEASE_SIZE = 14
        const val EXPECTED_EXPR_LIST_REQ_HEADER = 0x59
        const val EXPECTED_EXPR_LIST_RESP_HEADER = 0x5A
        const val EXPECTED_EXPR_TRIGGER_HEADER = 0x5B
        const val EXPECTED_EXPR_TRIGGER_SIZE = 15
    }

    @Test
    fun control() {
        assertEquals(EXPECTED_CONTROL_HEADER, CONTROL_HEADER) // udp::kControlHeader
        assertEquals(EXPECTED_CONTROL_SIZE, CONTROL_SIZE) // udp::kControlSize
    }

    @Test
    fun telemetry() {
        assertEquals(EXPECTED_TELEMETRY_HEADER, TELEMETRY_HEADER) // udp::kTelemetryHeader
        assertEquals(EXPECTED_TELEMETRY_SIZE, TELEMETRY_SIZE) // udp::kTelemetrySize
    }

    @Test
    fun pairReq() {
        assertEquals(EXPECTED_PAIR_REQ_HEADER, PAIR_REQ_HEADER) // udpstatus::kPairReqHeader
        assertEquals(EXPECTED_PAIR_REQ_SIZE, PAIR_REQ_SIZE) // udpstatus::kPairReqSize
    }

    @Test
    fun pairOk() {
        assertEquals(EXPECTED_PAIR_OK_HEADER, PAIR_OK_HEADER) // pairing::kPairOkHeader (fw-only)
        assertEquals(EXPECTED_PAIR_OK_SIZE, PAIR_OK_SIZE) // pairing::kPairOkLegacySize (v1)
        assertEquals(EXPECTED_PAIR_OK_SIZE_V11, PAIR_OK_SIZE_V11) // pairing::kPairOkSize (v1.1)
    }

    @Test
    fun pairErr() {
        assertEquals(EXPECTED_PAIR_ERR_HEADER, PAIR_ERR_HEADER) // fw sendPairErr literal
        assertEquals(EXPECTED_PAIR_ERR_SIZE, PAIR_ERR_SIZE)
    }

    @Test
    fun drive() {
        assertEquals(EXPECTED_DRIVE_HEADER, DRIVE_HEADER) // udpstatus::kDriveHeader
        assertEquals(EXPECTED_DRIVE_SIZE, DRIVE_SIZE) // udpstatus::kDriveSize
    }

    @Test
    fun unpair() {
        assertEquals(EXPECTED_UNPAIR_HEADER, UNPAIR_HEADER) // udpstatus::kUnpairHeader
        assertEquals(EXPECTED_UNPAIR_SIZE, UNPAIR_SIZE) // udpstatus::kUnpairSize
    }

    @Test
    fun pairRefresh() {
        assertEquals(EXPECTED_PAIR_REFRESH_REQ_HEADER, PAIR_REFRESH_REQ_HEADER) // udpstatus::kPairRefreshReqHeader
        assertEquals(EXPECTED_PAIR_REFRESH_REQ_SIZE, PAIR_REFRESH_REQ_SIZE) // udpstatus::kPairRefreshReqSize
        assertEquals(EXPECTED_PAIR_REFRESH_ACK_HEADER, PAIR_REFRESH_ACK_HEADER) // udpstatus::kPairRefreshAckHeader
        assertEquals(EXPECTED_PAIR_REFRESH_ACK_SIZE, PAIR_REFRESH_ACK_SIZE) // udpstatus::kPairRefreshAckSize
    }

    @Test
    fun aliveDisconnect() {
        assertEquals(EXPECTED_ALIVE_HEADER, ALIVE_HEADER) // udpstatus::kAliveHeader
        assertEquals(EXPECTED_ALIVE_SIZE, ALIVE_SIZE) // udpstatus::kAliveSize
        assertEquals(EXPECTED_DISCONNECT_HEADER, DISCONNECT_HEADER) // udpstatus::kDisconnectHeader
        assertEquals(EXPECTED_DISCONNECT_SIZE, DISCONNECT_SIZE) // udpstatus::kDisconnectSize
    }

    @Test
    fun manualTakeRelease() {
        assertEquals(EXPECTED_MANUAL_TAKE_HEADER, MANUAL_TAKE_HEADER)
        assertEquals(EXPECTED_MANUAL_TAKE_SIZE, MANUAL_TAKE_SIZE)
        assertEquals(EXPECTED_MANUAL_RELEASE_HEADER, MANUAL_RELEASE_HEADER)
        assertEquals(EXPECTED_MANUAL_RELEASE_SIZE, MANUAL_RELEASE_SIZE)
    }

    @Test
    fun expressions() {
        // 0x59/0x5A stay claimed retired-reserved (never reuse); 0x5B is live.
        assertEquals(EXPECTED_EXPR_LIST_REQ_HEADER, EXPR_LIST_REQ_HEADER)
        assertEquals(EXPECTED_EXPR_LIST_RESP_HEADER, EXPR_LIST_RESP_HEADER)
        assertEquals(EXPECTED_EXPR_TRIGGER_HEADER, EXPR_TRIGGER_HEADER)
        assertEquals(EXPECTED_EXPR_TRIGGER_SIZE, EXPR_TRIGGER_SIZE)
    }

    @Test
    fun claimedHeadersDistinct() {
        val headers = listOf(CONTROL_HEADER, TELEMETRY_HEADER, PAIR_REQ_HEADER, PAIR_OK_HEADER, PAIR_ERR_HEADER, DRIVE_HEADER, UNPAIR_HEADER, PAIR_REFRESH_REQ_HEADER, PAIR_REFRESH_ACK_HEADER, ALIVE_HEADER, DISCONNECT_HEADER, MANUAL_TAKE_HEADER, MANUAL_RELEASE_HEADER, EXPR_LIST_REQ_HEADER, EXPR_LIST_RESP_HEADER, EXPR_TRIGGER_HEADER)
        assertEquals(headers.size, headers.toSet().size) // never reuse/renumber
        assertTrue(headers.none { it == 0x00 }) // 0x00 stays unclaimed
    }
}
