// Host unit tests for the UDP server helpers (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace udpstatus (statusFlags bit map +
// decidegree math in src/transport/udp_server.h), the coordinator
// packCommand->unpackCommand round-trip at the center-128 edges, and a
// telemetry encode->decode round-trip with flags set. The UdpServer task
// (WiFi/socket/WDT) is firmware-only (#ifdef ARDUINO) and exercised on
// hardware. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include <type_traits>
#include <utility>

#include "../include/pairing_receipt.h"  // PairingReceipt vocabulary home (move-only pins below)
#include "../src/abilities/pairing/pairing_codec.h"
#include "../src/transport/udp_server.h"
// Platform split 1.1: udp_server.h is included before telemetry_manager.h (it
// defines UDPSTATUS_TOP_HALF_INCLUDED + pulls the connection core);
// pairing_codec.h is the wire-codec owner, telemetry_manager.h the
// cadence/encode owner.
#include "../src/abilities/telemetry/telemetry_manager.h"
#include "../src/transport/udp_codec.h"

// Static assert #3 (platform split 1.1): frozen wire sizes + LastPair cache
// shape + distinct claimed headers, pinned at COMPILE time on the host.
// (The LastPair::ok39 == kPairOkSize pin lives in udp_server.h
// firmware-side — both contexts covered. All goldens below stay green.)
static_assert(udp::kControlHeader == 0xAA, "control header is 0xAA");
static_assert(udp::kControlSize == 6, "control is 6B");
static_assert(udp::kTelemetryHeader == 0xBB, "telemetry header is 0xBB");
static_assert(sizeof(udp::ControlPacket) == 4, "ControlPacket is 4 payload bytes (wire 6B = hdr + 4 + XOR)");
static_assert(sizeof(udp::TelemetryPacket) == 8, "TelemetryPacket is 8B padded (wire 9B = hdr + 7 fields + XOR)");
static_assert(udpstatus::kPairReqHeader == 0x50, "PAIR_REQ header is 0x50");
static_assert(udpstatus::kPairReqSize == 12, "PAIR_REQ is 12B");
static_assert(udpstatus::kDriveHeader == 0xAC, "DRIVE header is 0xAC");
static_assert(udpstatus::kUnpairHeader == 0xAD, "UNPAIR header is 0xAD");
static_assert(udpstatus::kUnpairSize == 12, "UNPAIR is 12B");
static_assert(udpstatus::kPairRefreshReqHeader == 0x53, "PAIR_REFRESH_REQ header is 0x53");
static_assert(udpstatus::kPairRefreshReqSize == 14, "PAIR_REFRESH_REQ is 14B");
static_assert(udpstatus::kPairRefreshAckHeader == 0x54, "PAIR_REFRESH_ACK header is 0x54");
static_assert(pairing::kPairOkSize == 39, "PAIR_OK is 39B (v1.1)");
static_assert(pairing::kPairOkLegacySize == 38, "v1 PAIR_OK was 38B");
static_assert(udpstatus::kPairRefreshAckSize == 10, "PAIR_REFRESH_ACK is 10B");
// NOTE: PAIR_OK header (0x51) lives in pairing_codec.h and PAIR_ERR (0x52/4) is a firmware-only
// literal in sendPairErr — neither has a host-visible constexpr, so both are pinned
// as literals in test_udp_mirror_freeze / test_udp_pair_*_frame_golden below
// (literals reviewed against the udp_codec.h freeze table + Codec.kt + README).
static_assert(pairing::kPairRefreshAckSize == 10, "PAIR_REFRESH_ACK is 10B (pairing owner)");
static_assert(udpstatus::kPairRefreshAckSize == pairing::kPairRefreshAckSize, "pair-refresh ACK size single truth");
static_assert(udpstatus::kAliveSize == 14, "ALIVE is 14B");
static_assert(udpstatus::kDisconnectSize == 14, "DISCONNECT is 14B");
static_assert(udpstatus::kManualTakeHeader == 0x57, "MANUAL_TAKE header is 0x57");
static_assert(udpstatus::kManualTakeSize == 14, "MANUAL_TAKE is 14B");
static_assert(udpstatus::kManualReleaseHeader == 0x58, "MANUAL_RELEASE header is 0x58");
static_assert(udpstatus::kManualReleaseSize == 14, "MANUAL_RELEASE is 14B");
static_assert(udp::kExprListReqHeader == 0x59, "0x59 retired-reserved, never reused");
static_assert(udp::kExprListRespHeader == 0x5A, "0x5A retired-reserved, never reused");
static_assert(udpstatus::kExprTriggerHeader == 0x5B, "EXPR_TRIGGER header is 0x5B");
static_assert(udpstatus::kExprTriggerSize == 15, "EXPR_TRIGGER is 15B");
static_assert(pairing::kAliveSize == 14, "ALIVE is 14B (pairing owner)");
static_assert(pairing::kDisconnectSize == 14, "DISCONNECT is 14B (pairing owner)");
static_assert(udpstatus::kDriveSize == 16, "DRIVE is 16B");
static_assert(udp::kTelemetrySize == 9, "telemetry is 9B");
static_assert(udpstatus::kDriveHeader != udpstatus::kPairReqHeader, "claimed headers distinct");
static_assert(udpstatus::kDriveHeader != udpstatus::kUnpairHeader, "claimed headers distinct");
static_assert(udpstatus::kDriveHeader != udpstatus::kPairRefreshReqHeader, "claimed headers distinct");
static_assert(udpstatus::kDriveHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kDriveHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kPairReqHeader != udpstatus::kUnpairHeader, "claimed headers distinct");
static_assert(udpstatus::kPairReqHeader != udpstatus::kPairRefreshReqHeader, "claimed headers distinct");
static_assert(udpstatus::kPairReqHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kPairReqHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kUnpairHeader != udpstatus::kPairRefreshReqHeader, "claimed headers distinct");
static_assert(udpstatus::kUnpairHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kUnpairHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kPairRefreshReqHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kPairRefreshReqHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kAliveHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kManualTakeHeader != udpstatus::kManualReleaseHeader, "claimed headers distinct");
static_assert(udpstatus::kManualTakeHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kManualTakeHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kManualReleaseHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kManualReleaseHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kManualTakeHeader != udpstatus::kUnpairHeader, "claimed headers distinct");
static_assert(udpstatus::kManualReleaseHeader != udpstatus::kUnpairHeader, "claimed headers distinct");
static_assert(udpstatus::kExprTriggerHeader != udpstatus::kManualTakeHeader, "claimed headers distinct");
static_assert(udpstatus::kExprTriggerHeader != udpstatus::kAliveHeader, "claimed headers distinct");
static_assert(udpstatus::kExprTriggerHeader != udpstatus::kDisconnectHeader, "claimed headers distinct");
static_assert(udpstatus::kExprTriggerHeader != udpstatus::kUnpairHeader, "claimed headers distinct");
// Token discipline (platform split 1.1, token #1): move-only, never copied.
static_assert(!std::is_copy_constructible<connection::Verified>::value, "Verified is move-only");
static_assert(!std::is_copy_assignable<connection::Verified>::value, "Verified is move-only");
static_assert(std::is_move_constructible<connection::Verified>::value, "Verified moves");
static_assert(!std::is_copy_constructible<connection::Pinned>::value, "Pinned is move-only");
static_assert(!std::is_copy_assignable<connection::Pinned>::value, "Pinned is move-only");
static_assert(std::is_move_constructible<connection::Pinned>::value, "Pinned moves");
static_assert(!std::is_copy_constructible<connection::PairingReceipt>::value, "PairingReceipt is move-only");
static_assert(!std::is_copy_assignable<connection::PairingReceipt>::value, "PairingReceipt is move-only");
static_assert(std::is_move_constructible<connection::PairingReceipt>::value, "PairingReceipt moves");

void test_status_flags_pack_bits() {
  TEST_ASSERT_EQUAL_UINT8(0x01, udpstatus::packStatus(true, false, 0));
  TEST_ASSERT_EQUAL_UINT8(0x02, udpstatus::packStatus(false, true, 0));
  TEST_ASSERT_EQUAL_UINT8(0x08, udpstatus::packStatus(false, false, 2));
  TEST_ASSERT_EQUAL_UINT8(0x0F, udpstatus::packStatus(true, true, 3));
  TEST_ASSERT_EQUAL_UINT8(0x00, udpstatus::packStatus(false, false, 0));
  // Reserved bits 5-7 stay 0 for every mode (bit 4 is the MANUAL session bit).
  for (uint8_t mode = 0; mode < 4; ++mode) {
    TEST_ASSERT_EQUAL_UINT8(0, udpstatus::packStatus(true, true, mode) & 0xE0);
    TEST_ASSERT_EQUAL_UINT8(0, udpstatus::packStatus(true, true, mode, true) & 0xE0);
  }
}

void test_status_flags_manual_bit() {
  // Bit 4 mirrors the MANUAL drive session: 0 without, 0x10 with.
  TEST_ASSERT_EQUAL_UINT8(0x00, udpstatus::packStatus(false, false, 0, false));
  TEST_ASSERT_EQUAL_UINT8(0x10, udpstatus::packStatus(false, false, 0, true));
  TEST_ASSERT_EQUAL_UINT8(0x1F, udpstatus::packStatus(true, true, 3, true));
  TEST_ASSERT_EQUAL_UINT8(0x0F, udpstatus::packStatus(true, true, 3, false));
  TEST_ASSERT_FALSE(udpstatus::statusManual(udpstatus::packStatus(false, false, 0, false)));
  TEST_ASSERT_TRUE(udpstatus::statusManual(udpstatus::packStatus(false, false, 0, true)));
  // The session bit is orthogonal to cliff/driving/mode.
  for (uint8_t mode = 0; mode < 4; ++mode) {
    const uint8_t flags = udpstatus::packStatus(true, true, mode, true);
    TEST_ASSERT_TRUE(udpstatus::statusCliff(flags));
    TEST_ASSERT_TRUE(udpstatus::statusDriving(flags));
    TEST_ASSERT_EQUAL_UINT8(mode, udpstatus::statusMode(flags));
    TEST_ASSERT_TRUE(udpstatus::statusManual(flags));
  }
}

void test_status_flags_unpack_round_trip() {
  for (uint8_t mode = 0; mode < 4; ++mode) {
    const uint8_t flags = udpstatus::packStatus(true, false, mode);
    TEST_ASSERT_TRUE(udpstatus::statusCliff(flags));
    TEST_ASSERT_FALSE(udpstatus::statusDriving(flags));
    TEST_ASSERT_EQUAL_UINT8(mode, udpstatus::statusMode(flags));
  }
  const uint8_t flags = udpstatus::packStatus(false, true, 1);
  TEST_ASSERT_FALSE(udpstatus::statusCliff(flags));
  TEST_ASSERT_TRUE(udpstatus::statusDriving(flags));
  TEST_ASSERT_EQUAL_UINT8(1, udpstatus::statusMode(flags));
}

void test_pack_command_center_edges() {
  // Stick edges: 0 -> -1, 128 -> 0, 255 -> +1 through the event payload.
  uint8_t mode = 0;
  uint8_t flags = 0;
  float v = 0.0f;
  float omega = 0.0f;
  udp::unpackCommand(udp::packCommand(0x02, 0, 255, 0x05), mode, v, omega, flags);
  TEST_ASSERT_EQUAL_UINT8(0x02, mode);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, -1.0f, v);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.0f, omega);
  TEST_ASSERT_EQUAL_UINT8(0x05, flags);
  udp::unpackCommand(udp::packCommand(0x00, 128, 128, 0x00), mode, v, omega, flags);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, omega);
}

void test_telemetry_round_trip_with_flags_set() {
  udp::TelemetryPacket src;
  src.pitch = udpstatus::degToDecideg(12.3f);
  src.roll = udpstatus::degToDecideg(-4.5f);
  src.distanceMm = 1234;
  src.statusFlags = udpstatus::packStatus(true, true, 3);  // cliff + driving + EMERGENCY
  TEST_ASSERT_EQUAL_UINT8(0x0F, src.statusFlags);
  uint8_t buf[udp::kTelemetrySize];
  TEST_ASSERT_TRUE(udp::encodeTelemetry(src, buf));
  udp::TelemetryPacket dst;
  TEST_ASSERT_TRUE(udp::decodeTelemetry(buf, sizeof(buf), dst));
  TEST_ASSERT_EQUAL_INT16(123, dst.pitch);
  TEST_ASSERT_EQUAL_INT16(-45, dst.roll);
  TEST_ASSERT_EQUAL_UINT16(1234, dst.distanceMm);
  TEST_ASSERT_EQUAL_UINT8(0x0F, dst.statusFlags);
  TEST_ASSERT_TRUE(udpstatus::statusCliff(dst.statusFlags));
  TEST_ASSERT_TRUE(udpstatus::statusDriving(dst.statusFlags));
  TEST_ASSERT_EQUAL_UINT8(3, udpstatus::statusMode(dst.statusFlags));
}

void test_deg_to_decideg_edges_and_clamp() {
  TEST_ASSERT_EQUAL_INT16(0, udpstatus::degToDecideg(0.0f));
  TEST_ASSERT_EQUAL_INT16(123, udpstatus::degToDecideg(12.3f));
  TEST_ASSERT_EQUAL_INT16(-45, udpstatus::degToDecideg(-4.5f));
  TEST_ASSERT_EQUAL_INT16(32767, udpstatus::degToDecideg(4000.0f));
  TEST_ASSERT_EQUAL_INT16(-32768, udpstatus::degToDecideg(-4000.0f));
}

// Pairing/auth v1: frozen wire constants, wrap-aware counter
// freshness, and the RX demux table. The firmware verify path (HMAC, NVS,
// sockets) is #ifdef ARDUINO and exercised on hardware.

void test_udp_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0xAC, udpstatus::kDriveHeader);
  TEST_ASSERT_TRUE(udpstatus::kDriveSize == 16);
  TEST_ASSERT_EQUAL_UINT8(0x50, udpstatus::kPairReqHeader);
  TEST_ASSERT_TRUE(udpstatus::kPairReqSize == 12);
  // Cross-check: the RX demux hard-codes the legacy 6B/0xAA shape instead
  // of including udp_codec.h. Pin the copy to the canonical
  // udp::kControlHeader/kControlSize so drift fails here.
  TEST_ASSERT_EQUAL_UINT8(udp::kControlHeader, 0xAA);
  TEST_ASSERT_TRUE(udp::kControlSize == 6);
  TEST_ASSERT_EQUAL(static_cast<int>(udpstatus::RxKind::kLegacy),
                    static_cast<int>(udpstatus::classifyRx(udp::kControlSize, udp::kControlHeader)));
}

void test_udp_fresh_counter_greater_ok() {
  // Initial lastSeen=0, so the first counter (1) is fresh.
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(0, 1));
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(1, 2));
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(41, 42));
}

void test_udp_fresh_counter_equal_older_reject() {
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(7, 7));
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(0, 0));
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(10, 9));
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(1, 0));
}

void test_udp_fresh_counter_wrap_accept() {
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(0xFFFFFFFFu, 0u));
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(0xFFFFFFFEu, 0xFFFFFFFFu));
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(0xFFFFFFFFu, 1u));
  // Equal across the wrap is still a replay.
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(0xFFFFFFFFu, 0xFFFFFFFFu));
}

void test_udp_fresh_counter_half_range_reject() {
  // Exactly half the range ahead is direction-ambiguous: reject.
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(0u, 0x80000000u));
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(0x80000000u, 0u));
  // Just inside half-range still accepts; just outside rejects.
  TEST_ASSERT_TRUE(udpstatus::isFreshCounter(0u, 0x7FFFFFFFu));
  TEST_ASSERT_FALSE(udpstatus::isFreshCounter(0u, 0x80000001u));
}

namespace {
int rxKindInt(udpstatus::RxKind k) { return static_cast<int>(k); }
}  // namespace

void test_udp_classify_table() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kLegacy), rxKindInt(udpstatus::classifyRx(6, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrive), rxKindInt(udpstatus::classifyRx(16, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kPairReq), rxKindInt(udpstatus::classifyRx(12, 0x50)));
}

void test_udp_classify_wrong_header_or_size_drops() {
  // Right size, wrong header.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(6, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(6, 0x50)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(16, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(16, 0x50)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xAC)));
  // Right header, wrong size.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(5, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(7, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(17, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(11, 0x50)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x50)));
  // Telemetry header never arrives on the RX path.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(9, 0xBB)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xBB)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(0, 0xAA)));
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the udpstatus tests are declared extern there.

// PAIR_OK redelivery predicate: same code+nonce inside the window
// redelivers; anything else does not. Window is CFG_PAIR_REDELIVERY_MS (60s).

void test_udp_redeliver_same_true() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_TRUE(udpstatus::shouldRedeliver(true, code, nonce, code, nonce, 1000u, 2000u));
}

void test_udp_redeliver_different_nonce_false() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  const uint8_t other[4] = {0x01, 0x02, 0x03, 0x05};
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, code, other, 1000u, 2000u));
}

void test_udp_redeliver_different_code_false() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t wrong[6] = {'1', '2', '3', '4', '5', '7'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, wrong, nonce, 1000u, 2000u));
}

void test_udp_redeliver_expired_false() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, code, nonce, 0u, 60000u));
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, code, nonce, 0u, 60001u));
}

void test_udp_redeliver_invalid_or_null_false() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(false, code, nonce, code, nonce, 1000u, 2000u));
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, nullptr, nonce, code, nonce, 1000u, 2000u));
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nullptr, code, nonce, 1000u, 2000u));
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, nullptr, nonce, 1000u, 2000u));
  TEST_ASSERT_FALSE(udpstatus::shouldRedeliver(true, code, nonce, code, nullptr, 1000u, 2000u));
}

void test_udp_redeliver_millis_wrap_true() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonce[4] = {0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_TRUE(udpstatus::shouldRedeliver(true, code, nonce, code, nonce, 0xFFFFFF00u, 100u));
}

// Signed UNPAIR: wire constants, demux, and the tag-message golden.

void test_udp_unpair_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0xAD, udpstatus::kUnpairHeader);
  TEST_ASSERT_TRUE(udpstatus::kUnpairSize == 12);
}

void test_udp_classify_unpair() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kUnpair), rxKindInt(udpstatus::classifyRx(12, 0xAD)));
  // Neighbouring sizes/headers still drop.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(11, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xAC)));
}

void test_udp_unpair_tag_msg_golden() {
  uint8_t msg[udpstatus::kUnpairMsgLen];
  pairing::unpairTagMsg(1u, msg);
  const uint8_t expected[10] = {0x55, 0x4E, 0x50, 0x41, 0x49, 0x52, 0x00, 0x00, 0x00, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
}

// Tier-1 silent auto-pair-refresh (Task: era+PSK rotation): wire constants pin
// claimed headers/sizes (0x53/0x54 — never 0xAA/0xAC/0xBB/0x50/0x51/0x52/
// 0xAD), and the demux accepts exactly the 14B PAIR_REFRESH_REQ shape.

void test_udp_pair_refresh_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0x53, udpstatus::kPairRefreshReqHeader);
  TEST_ASSERT_TRUE(udpstatus::kPairRefreshReqSize == 14);
  TEST_ASSERT_EQUAL_UINT8(0x54, udpstatus::kPairRefreshAckHeader);
  TEST_ASSERT_TRUE(udpstatus::kPairRefreshAckSize == 10);
}

void test_udp_classify_pair_refresh() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kPairRefreshReq), rxKindInt(udpstatus::classifyRx(14, 0x53)));
  // Neighbouring sizes/headers still drop (incl. the ACK header, which the
  // robot only ever transmits, and every previously claimed header).
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x53)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x53)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x54)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(10, 0x54)));
}

// ALIVE + DISCONNECT (robot alive rule): wire constants pin the claimed
// headers/sizes (0x55/0x56 — never 0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/
// 0xAD), and the demux accepts exactly the two 14B shapes. The HMAC tag
// goldens live in shared/protocol/README.md (firmware verifies on-device);
// these host tests pin consts + demux + the 5s freshness window.
void test_udp_alive_disconnect_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0x55, udpstatus::kAliveHeader);
  TEST_ASSERT_TRUE(udpstatus::kAliveSize == 14);
  TEST_ASSERT_EQUAL_UINT8(0x56, udpstatus::kDisconnectHeader);
  TEST_ASSERT_TRUE(udpstatus::kDisconnectSize == 14);
  TEST_ASSERT_EQUAL_UINT32(5000, udpstatus::kAliveMs);
}

void test_udp_classify_alive_disconnect() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kAlive), rxKindInt(udpstatus::classifyRx(14, 0x55)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDisconnect), rxKindInt(udpstatus::classifyRx(14, 0x56)));
  // Neighbouring sizes/headers still drop (incl. the ACK header, which the
  // robot only ever transmits at 10B, and every other claimed header at 14B
  // — except 0x53, which IS the valid 14B PAIR_REFRESH_REQ shape).
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x55)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x55)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x56)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x56)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x54)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x50)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAA)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0x55)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0x56)));
}

void test_udp_claimed_headers_distinct() {
  // Never-reuse rule: every claimed RX header is distinct from the legacy
  // control header, the telemetry header, and each other.
  const uint8_t headers[] = {0xAA,
                             udpstatus::kDriveHeader,
                             udpstatus::kPairReqHeader,
                             udpstatus::kUnpairHeader,
                             udpstatus::kPairRefreshReqHeader,
                             udpstatus::kAliveHeader,
                             udpstatus::kDisconnectHeader,
                             udpstatus::kManualTakeHeader,
                             udpstatus::kManualReleaseHeader,
                             udp::kExprListReqHeader,
                             udpstatus::kExprTriggerHeader};
  for (size_t i = 0; i < sizeof(headers); ++i) {
    for (size_t j = i + 1; j < sizeof(headers); ++j) {
      TEST_ASSERT_TRUE(headers[i] != headers[j]);
    }
    TEST_ASSERT_TRUE(udp::kTelemetryHeader != headers[i]);
  }
}

// MANUAL session demux: 0x57/0x58 claim fresh 14B shapes. The HMAC tags are
// ARDUINO-only (mbedTLS); the cross-side goldens live in
// shared/protocol/README.md (take_tag=1ee989586f8e,
// release_tag=e2ce426715b5 for era=1, counter=100 under the shared psk).
void test_udp_manual_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0x57, udpstatus::kManualTakeHeader);
  TEST_ASSERT_TRUE(udpstatus::kManualTakeSize == 14);
  TEST_ASSERT_EQUAL_UINT8(0x58, udpstatus::kManualReleaseHeader);
  TEST_ASSERT_TRUE(udpstatus::kManualReleaseSize == 14);
}

void test_udp_classify_manual() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kManualTake), rxKindInt(udpstatus::classifyRx(14, 0x57)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kManualRelease), rxKindInt(udpstatus::classifyRx(14, 0x58)));
  // Neighbouring sizes/headers still drop. (0x53/0x55/0x56 at 14B are the
  // valid PAIR_REFRESH_REQ/ALIVE/DISCONNECT shapes, so they are absent here.)
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x57)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x57)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x58)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x58)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xFF)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0x57)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0x58)));
}

// Legacy pre-pair frames pin the telemetry peer only and never reach the
// drive publisher: the demux must keep the 6B/0xAA shape on its own kind,
// distinct from every verified-drive kind.
void test_udp_legacy_never_drives() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kLegacy), rxKindInt(udpstatus::classifyRx(6, 0xAA)));
  TEST_ASSERT_TRUE(rxKindInt(udpstatus::RxKind::kLegacy) != rxKindInt(udpstatus::RxKind::kDrive));
  TEST_ASSERT_TRUE(rxKindInt(udpstatus::RxKind::kLegacy) != rxKindInt(udpstatus::RxKind::kManualTake));
  TEST_ASSERT_TRUE(rxKindInt(udpstatus::RxKind::kLegacy) != rxKindInt(udpstatus::RxKind::kManualRelease));
}

void test_udp_alive_fresh_inside_window() {
  TEST_ASSERT_TRUE(udpstatus::isAliveFresh(1000u, 1000u));
  TEST_ASSERT_TRUE(udpstatus::isAliveFresh(1000u, 5999u));
  TEST_ASSERT_TRUE(udpstatus::isAliveFresh(1000u, 6000u));  // Boundary counts as fresh.
}

void test_udp_alive_stale_past_window() {
  TEST_ASSERT_FALSE(udpstatus::isAliveFresh(1000u, 6001u));
  TEST_ASSERT_FALSE(udpstatus::isAliveFresh(0u, 5001u));
  TEST_ASSERT_FALSE(udpstatus::isAliveFresh(0u, 60000u));
}

void test_udp_alive_fresh_millis_wrap() {
  // Wrap-safe: 356ms after a near-rollover mark is fresh, 6256ms is stale.
  TEST_ASSERT_TRUE(udpstatus::isAliveFresh(0xFFFFFF00u, 100u));
  TEST_ASSERT_FALSE(udpstatus::isAliveFresh(0xFFFFFF00u, 6000u));
}

// Telemetry TX gate (broadcast fallback deleted): unicast only when paired
// && peer pinned && alive-fresh; else silent. Pre-pair stays silent.
void test_udp_telemetry_gate_unicast_when_paired_alive() {
  TEST_ASSERT_TRUE(udpstatus::shouldSendTelemetry(true, true, 1000u, 1000u));
  TEST_ASSERT_TRUE(udpstatus::shouldSendTelemetry(true, true, 1000u, 6000u));  // Boundary fresh.
}

void test_udp_telemetry_gate_silent_when_stale() {
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(true, true, 1000u, 6001u));
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(true, true, 0u, 60000u));
}

void test_udp_telemetry_gate_silent_when_no_peer() {
  // Post-pair reboot before the first packet: no peer yet -> silent (the
  // deleted broadcast fallback used to send here).
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(true, false, 1000u, 1000u));
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(true, false, 0u, 0u));
}

void test_udp_telemetry_gate_silent_pre_pair() {
  // Pre-pair stays silent even with a pinned peer + fresh stamp.
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(false, true, 1000u, 1000u));
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(false, false, 1000u, 1000u));
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(false, true, 1000u, 6001u));
}

void test_udp_telemetry_gate_wrap() {
  TEST_ASSERT_TRUE(udpstatus::shouldSendTelemetry(true, true, 0xFFFFFF00u, 100u));
  TEST_ASSERT_FALSE(udpstatus::shouldSendTelemetry(true, true, 0xFFFFFF00u, 6000u));
}

// Runtime mirror of the frozen-size static asserts above (keeps the sizes
// visible in the test log too).
void test_udp_frozen_sizes() {
  TEST_ASSERT_EQUAL(39, static_cast<int>(pairing::kPairOkSize));
  TEST_ASSERT_EQUAL(38, static_cast<int>(pairing::kPairOkLegacySize));
  TEST_ASSERT_EQUAL(10, static_cast<int>(udpstatus::kPairRefreshAckSize));
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kAliveSize));
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kDisconnectSize));
  TEST_ASSERT_EQUAL(16, static_cast<int>(udpstatus::kDriveSize));
  TEST_ASSERT_EQUAL(9, static_cast<int>(udp::kTelemetrySize));
}

// Platform split 1.1, step 1: ConnectionManager (u32 peer, no IPAddress in
// core). Window is the udpstatus::kAliveMs passthrough — pinned, not
// redefined: the manager takes it as a ctor parameter.
void test_conn_ip_u32_roundtrip() {
  TEST_ASSERT_EQUAL_UINT32(0xC0A80401u, connection::ipToU32(192, 168, 4, 1));
  uint8_t out[4] = {};
  connection::u32ToIp(0xC0A80401u, out);
  const uint8_t expected[4] = {192, 168, 4, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
  connection::u32ToIp(0, nullptr);  // Null-safe no-op.
}

void test_conn_initial_cleared() {
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  TEST_ASSERT_EQUAL_UINT32(5000, udpstatus::kAliveMs);  // Passthrough value pinned.
  TEST_ASSERT_FALSE(mgr.havePeer());
  TEST_ASSERT_FALSE(mgr.isAlive(1000));
  TEST_ASSERT_EQUAL_UINT32(0, mgr.peer());
}

void test_conn_markalive_pins_and_alive() {
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  const connection::Checks okc{true, true, true, true};
  connection::Verified v = connection::AuthVerifier::verify(okc, 42, 3);
  TEST_ASSERT_TRUE(v.valid());
  TEST_ASSERT_EQUAL_UINT32(42, v.ctr());
  TEST_ASSERT_EQUAL_UINT16(3, v.era());
  connection::Pinned p = mgr.markAlive(std::move(v), 0xC0A80401u, 1000);
  TEST_ASSERT_TRUE(p.valid());
  TEST_ASSERT_EQUAL_UINT32(0xC0A80401u, p.peer());
  TEST_ASSERT_TRUE(mgr.havePeer());
  TEST_ASSERT_EQUAL_UINT32(0xC0A80401u, mgr.peer());
  TEST_ASSERT_TRUE(mgr.isAlive(1000));
  TEST_ASSERT_TRUE(mgr.isAlive(6000));   // Boundary counts as fresh.
  TEST_ASSERT_FALSE(mgr.isAlive(6001));  // Past the window: stale.
}

void test_conn_markalive_rejects_unverified() {
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  // Each denied cause mints an invalid token; the funnel changes nothing.
  const connection::Checks causes[4] = {
      {false, true, true, true}, {true, false, true, true}, {true, true, false, true}, {true, true, true, false}};
  for (size_t i = 0; i < 4; ++i) {
    connection::Verified v = connection::AuthVerifier::verify(causes[i], 7, 0);
    TEST_ASSERT_FALSE(v.valid());
    connection::Pinned p = mgr.markAlive(std::move(v), 0xC0A80401u, 1000);
    TEST_ASSERT_FALSE(p.valid());
  }
  TEST_ASSERT_FALSE(mgr.havePeer());
  TEST_ASSERT_FALSE(mgr.isAlive(1000));
}

void test_conn_refresh_then_clear() {
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  const connection::Checks okc{true, true, true, true};
  connection::Verified v = connection::AuthVerifier::verify(okc, 1, 0);
  connection::Pinned p = mgr.markAlive(std::move(v), 0xC0A80401u, 1000);
  TEST_ASSERT_TRUE(p.valid());
  connection::Verified g = connection::AuthVerifier::verify(okc, 2, 0);
  mgr.refreshThenClear(std::move(g), 2000);  // Signed goodbye: stamp kept, net peer gone.
  TEST_ASSERT_FALSE(mgr.havePeer());
  TEST_ASSERT_FALSE(mgr.isAlive(2000));
  TEST_ASSERT_EQUAL_UINT32(2000, mgr.lastAliveMs());
  mgr.clear();
  TEST_ASSERT_EQUAL_UINT32(0, mgr.lastAliveMs());
}

void test_conn_pin_peer_no_stamp() {
  // Pre-pair quirk: pinning records the peer WITHOUT touching the alive
  // stamp.
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  mgr.pinPeer(0xC0A80401u);
  TEST_ASSERT_TRUE(mgr.havePeer());
  TEST_ASSERT_EQUAL_UINT32(0, mgr.lastAliveMs());
  const connection::Checks okc{true, true, true, true};
  connection::Verified v = connection::AuthVerifier::verify(okc, 1, 0);
  connection::Pinned p = mgr.markAlive(std::move(v), 0xC0A80401u, 1000);
  TEST_ASSERT_TRUE(p.valid());
  mgr.pinPeer(0xC0A80402u);  // Re-pin (e.g. DHCP change) without a valid frame.
  TEST_ASSERT_EQUAL_UINT32(0xC0A80402u, mgr.peer());
  TEST_ASSERT_EQUAL_UINT32(1000, mgr.lastAliveMs());  // Stamp untouched by the pin.
}

void test_conn_isalive_wrap_safe() {
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  const connection::Checks okc{true, true, true, true};
  connection::Verified v = connection::AuthVerifier::verify(okc, 1, 0);
  connection::Pinned p = mgr.markAlive(std::move(v), 1, 0xFFFFFF00u);
  TEST_ASSERT_TRUE(p.valid());
  TEST_ASSERT_TRUE(mgr.isAlive(100u));    // 356ms after a near-rollover mark: fresh.
  TEST_ASSERT_FALSE(mgr.isAlive(6000u));  // 6256ms: stale.
}

// Platform split 1.1, step 3: token move semantics — validity transfers to
// the new owner, the moved-from token is dead (no proof duplication).
void test_tokens_move_transfers_validity() {
  const connection::Checks okc{true, true, true, true};
  connection::Verified v = connection::AuthVerifier::verify(okc, 9, 1);
  connection::Verified v2(std::move(v));
  TEST_ASSERT_TRUE(v2.valid());
  connection::ConnectionManager mgr(udpstatus::kAliveMs);
  connection::Pinned p = mgr.markAlive(std::move(v2), 5, 100);
  connection::Pinned p2(std::move(p));
  TEST_ASSERT_TRUE(p2.valid());
  TEST_ASSERT_EQUAL_UINT32(5, p2.peer());
  connection::PairingReceipt r;  // Default-constructs INVALID (fail-closed).
  TEST_ASSERT_FALSE(r.valid());
  connection::PairingReceipt r2(std::move(r));
  TEST_ASSERT_FALSE(r2.valid());
}

// TelemetryManager — encode, cadence + edge decide,
// bytes leave via the caller-provided send fn (no sockets here).
void test_telemetry_encode_snapshot_golden() {
  telemetry::Snapshot s;
  s.mode = 3;
  s.cliff = true;
  s.driving = true;
  s.pitchDeg = 12.3f;
  s.rollDeg = -4.5f;
  s.distMm = 1234;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  TEST_ASSERT_EQUAL_INT16(123, pkt.pitch);
  TEST_ASSERT_EQUAL_INT16(-45, pkt.roll);
  TEST_ASSERT_EQUAL_UINT16(1234, pkt.distanceMm);
  TEST_ASSERT_EQUAL_UINT8(0x0F, pkt.statusFlags);  // cliff + driving + EMERGENCY.
  // Single-owner cross-check: identical to the udpstatus lines moved here.
  TEST_ASSERT_EQUAL_INT16(udpstatus::degToDecideg(12.3f), pkt.pitch);
  TEST_ASSERT_EQUAL_INT16(udpstatus::degToDecideg(-4.5f), pkt.roll);
  TEST_ASSERT_EQUAL_UINT8(udpstatus::packStatus(true, true, 3), pkt.statusFlags);
}

void test_telemetry_publish_sends_when_due() {
  telemetry::TelemetryManager tm(66);
  TEST_ASSERT_EQUAL_UINT32(66, tm.rateMs());
  telemetry::Snapshot s;
  s.distMm = 300;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  uint8_t got[udp::kTelemetrySize] = {};
  size_t gotLen = 0;
  uint32_t calls = 0;
  const bool sent = tm.publish(pkt, true, 1000, [&](const uint8_t* b, size_t n) {
    gotLen = n;
    for (size_t i = 0; i < n && i < sizeof(got); ++i) {
      got[i] = b[i];
    }
    ++calls;
  });
  TEST_ASSERT_TRUE(sent);
  TEST_ASSERT_EQUAL_UINT32(1, calls);
  uint8_t expected[udp::kTelemetrySize] = {};
  udp::encodeTelemetry(pkt, expected);
  TEST_ASSERT_EQUAL(sizeof(expected), gotLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, got, sizeof(expected));
  TEST_ASSERT_TRUE(tm.hasSent());
  TEST_ASSERT_EQUAL_UINT32(1000, tm.lastTxMs());
}

void test_telemetry_publish_silent_without_gate() {
  telemetry::TelemetryManager tm(66);
  telemetry::Snapshot s;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  uint32_t calls = 0;
  TEST_ASSERT_FALSE(tm.publish(pkt, false, 1000, [&](const uint8_t* b, size_t n) {
    (void)b;
    (void)n;
    ++calls;
  }));
  TEST_ASSERT_EQUAL_UINT32(0, calls);
  TEST_ASSERT_FALSE(tm.hasSent());
}

void test_telemetry_publish_holds_cadence() {
  telemetry::TelemetryManager tm(66);
  telemetry::Snapshot s;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  uint32_t calls = 0;
  auto send = [&](const uint8_t* b, size_t n) {
    (void)b;
    (void)n;
    ++calls;
  };
  TEST_ASSERT_TRUE(tm.publish(pkt, true, 1000, send));
  TEST_ASSERT_FALSE(tm.publish(pkt, true, 1001, send));  // Same bytes, before cadence: silent.
  TEST_ASSERT_TRUE(tm.publish(pkt, true, 1066, send));   // Cadence due: sends.
  TEST_ASSERT_EQUAL_UINT32(2, calls);
}

void test_telemetry_publish_edge_forces_early() {
  telemetry::TelemetryManager tm(60000);  // Huge cadence: only an edge can send early.
  telemetry::Snapshot s;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  uint32_t calls = 0;
  auto send = [&](const uint8_t* b, size_t n) {
    (void)b;
    (void)n;
    ++calls;
  };
  TEST_ASSERT_TRUE(tm.publish(pkt, true, 1000, send));   // First send is an edge.
  TEST_ASSERT_FALSE(tm.publish(pkt, true, 1001, send));  // Same bytes: silent.
  s.cliff = true;                                        // Status edge: cliff bit flips.
  telemetry::encodeSnapshot(s, pkt);
  TEST_ASSERT_TRUE(tm.publish(pkt, true, 1001, send));  // Edge fires before cadence.
  TEST_ASSERT_EQUAL_UINT32(2, calls);
}

// ── Wire mirror gate ─────────────────────────────────────────────
// Literal freeze of every header/size in the udp_codec.h table (firmware
// udp_server.h top half + pairing_codec.h vs client domain/Codec.kt vs
// shared/protocol/README.md). Host-visible constexprs are pinned against
// literals; PAIR_OK header (0x51, ARDUINO-only kPairOkHeader) and PAIR_ERR
// (0x52/4, firmware-only sendPairErr literal) are pinned as literals by the
// golden frames below — the byte arrays literally contain 0x51/0x52, so any
// renumber drifts the review, not silently passes.
void test_udp_mirror_freeze() {
  TEST_ASSERT_EQUAL_UINT8(0xAA, udp::kControlHeader);
  TEST_ASSERT_EQUAL(6, static_cast<int>(udp::kControlSize));
  TEST_ASSERT_EQUAL_UINT8(0xBB, udp::kTelemetryHeader);
  TEST_ASSERT_EQUAL(9, static_cast<int>(udp::kTelemetrySize));
  TEST_ASSERT_EQUAL_UINT8(0x50, udpstatus::kPairReqHeader);
  TEST_ASSERT_EQUAL(12, static_cast<int>(udpstatus::kPairReqSize));
  TEST_ASSERT_EQUAL(39, static_cast<int>(pairing::kPairOkSize));
  TEST_ASSERT_EQUAL(38, static_cast<int>(pairing::kPairOkLegacySize));
  TEST_ASSERT_EQUAL_UINT8(0xAC, udpstatus::kDriveHeader);
  TEST_ASSERT_EQUAL(16, static_cast<int>(udpstatus::kDriveSize));
  TEST_ASSERT_EQUAL_UINT8(0xAD, udpstatus::kUnpairHeader);
  TEST_ASSERT_EQUAL(12, static_cast<int>(udpstatus::kUnpairSize));
  TEST_ASSERT_EQUAL_UINT8(0x53, udpstatus::kPairRefreshReqHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kPairRefreshReqSize));
  TEST_ASSERT_EQUAL_UINT8(0x54, udpstatus::kPairRefreshAckHeader);
  TEST_ASSERT_EQUAL(10, static_cast<int>(udpstatus::kPairRefreshAckSize));
  TEST_ASSERT_EQUAL_UINT8(0x55, udpstatus::kAliveHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kAliveSize));
  TEST_ASSERT_EQUAL_UINT8(0x56, udpstatus::kDisconnectHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kDisconnectSize));
  TEST_ASSERT_EQUAL_UINT8(0x57, udpstatus::kManualTakeHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kManualTakeSize));
  TEST_ASSERT_EQUAL_UINT8(0x58, udpstatus::kManualReleaseHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udpstatus::kManualReleaseSize));
  TEST_ASSERT_EQUAL_UINT8(0x59, udp::kExprListReqHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(udp::kExprListReqSize));
  TEST_ASSERT_EQUAL_UINT8(0x5A, udp::kExprListRespHeader);
  TEST_ASSERT_EQUAL_UINT8(0x5B, udpstatus::kExprTriggerHeader);
  TEST_ASSERT_EQUAL(15, static_cast<int>(udpstatus::kExprTriggerSize));
  // TX-only sizes never collide with an RX shape (PAIR_OK 38/39, PAIR_ERR 4,
  // PAIR_REFRESH_ACK 10, EXPR_LIST_RESP 0x5A at any length): the RX demux
  // must drop them on any length.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(38, 0x51)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(39, 0x51)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(4, 0x52)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(10, 0x54)));
}

namespace {
// XOR over bytes 0..len-2 must equal the last byte (the exact check the
// firmware RX loop performs after classifyRx).
bool mirrorXorOk(const uint8_t* b, size_t n) {
  uint8_t x = 0;
  for (size_t i = 0; i + 1 < n; ++i) {
    x ^= b[i];
  }
  return x == b[n - 1];
}
}  // namespace

// PAIR_REQ 12B [0x50|code6|cliNonce4|XOR]: code "123456" + nonce 01020304
// (same fixed inputs as the Kotlin PairingPacketsTest golden).
void test_udp_pair_req_frame_golden() {
  const uint8_t frame[12] = {0x50, '1', '2', '3', '4', '5', '6', 0x01, 0x02, 0x03, 0x04, 0x53};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kPairReq), rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  // Decode: code + nonce bytes sit at the documented offsets.
  TEST_ASSERT_EQUAL_UINT8('1', frame[1]);
  TEST_ASSERT_EQUAL_UINT8('6', frame[6]);
  TEST_ASSERT_EQUAL_UINT8(0x01, frame[7]);
  TEST_ASSERT_EQUAL_UINT8(0x04, frame[10]);
  // Reject: truncated (11B), bad CRC (payload flip breaks the XOR), unknown ID.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(11, 0x50)));
  uint8_t bad[12];
  for (size_t i = 0; i < sizeof(bad); ++i) {
    bad[i] = frame[i];
  }
  bad[2] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0x51)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xFF)));
}

// PAIR_ERR 4B [0x52|reason|retry_s|XOR]: reason locked(1), retry 30s
// (same golden as the Kotlin decodePairErrGolden). TX-only: never RX.
void test_udp_pair_err_frame_golden() {
  const uint8_t frame[4] = {0x52, 0x01, 0x1E, 0x4D};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT8(0x52, frame[0]);  // Pins the sendPairErr literal.
  TEST_ASSERT_EQUAL_UINT8(0x01, frame[1]);
  TEST_ASSERT_EQUAL_UINT8(0x1E, frame[2]);
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  // Reject: truncated, bad CRC, unknown ID at this length.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(3, 0x52)));
  uint8_t bad[4] = {0x52, 0x01, 0x1E, 0x00};
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(4, 0xFF)));
}

// Additive reason 4 (internal_retry_fresh) PAIR_ERR golden:
// 4B [0x52|0x04|0x00|0x56] (0x52^0x04^0x00 = 0x56). This is the exact wire
// byte string UdpServer::handlePairReq sends on burn-failure after tryCode
// kOk (savePair/deriveKek/wrapPsk fail — code already burned single-use),
// via pairing::toWireReason(pairing::Result::kInternal). Genuine
// not_in_window still sends reason 2 via the tryCode path. TX-only: never RX.
void test_udp_pair_err_reason4_frame_golden() {
  TEST_ASSERT_EQUAL_UINT8(4, pairing::toWireReason(pairing::Result::kInternal));
  const uint8_t frame[4] = {0x52, 0x04, 0x00, 0x56};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT8(0x52, frame[0]);  // Pins the sendPairErr literal.
  TEST_ASSERT_EQUAL_UINT8(0x04, frame[1]);
  TEST_ASSERT_EQUAL_UINT8(0x00, frame[2]);
  TEST_ASSERT_EQUAL_UINT8(0x56, frame[3]);
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  // Reject: truncated, bad CRC, unknown ID at this length.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(3, 0x52)));
  uint8_t bad[4] = {0x52, 0x04, 0x00, 0x00};
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(4, 0xFF)));
}

// DRIVE 16B: mode 0x00, thr/str 0x80, flags 0x00, counter 1, frozen
// drive_tag 3ec8b625c9c1 (README shared vector, psk 0011..eeff).
void test_udp_drive_frame_golden() {
  const uint8_t frame[16] = {0xAC, 0x00, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00,
                             0x01, 0x3E, 0xC8, 0xB6, 0x25, 0xC9, 0xC1, 0xC0};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrive), rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  // Decode: sticks/mode/flags at legacy positions, counter BE bytes 5..8.
  TEST_ASSERT_EQUAL_UINT8(0x00, frame[1]);
  TEST_ASSERT_EQUAL_UINT8(0x80, frame[2]);
  TEST_ASSERT_EQUAL_UINT8(0x80, frame[3]);
  TEST_ASSERT_EQUAL_UINT8(0x00, frame[4]);
  const uint32_t ctr = (static_cast<uint32_t>(frame[5]) << 24) | (static_cast<uint32_t>(frame[6]) << 16) |
                       (static_cast<uint32_t>(frame[7]) << 8) | static_cast<uint32_t>(frame[8]);
  TEST_ASSERT_EQUAL_UINT32(1, ctr);
  // Reject: truncated, bad CRC, unknown ID at this length.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0xAC)));
  uint8_t bad[16];
  for (size_t i = 0; i < sizeof(bad); ++i) {
    bad[i] = frame[i];
  }
  bad[9] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(16, 0xFF)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(16, 0x50)));
}

// UNPAIR 12B [0xAD|ctrBE4|tag6|XOR]: counter 1 under psk 0011..eeff,
// tag first6(HMAC-SHA256(psk, "UNPAIR"||ctrBE4)) = 0850b6e7d000.
void test_udp_unpair_frame_golden() {
  const uint8_t frame[12] = {0xAD, 0x00, 0x00, 0x00, 0x01, 0x08, 0x50, 0xB6, 0xE7, 0xD0, 0x00, 0x75};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kUnpair), rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  const uint32_t ctr = (static_cast<uint32_t>(frame[1]) << 24) | (static_cast<uint32_t>(frame[2]) << 16) |
                       (static_cast<uint32_t>(frame[3]) << 8) | static_cast<uint32_t>(frame[4]);
  TEST_ASSERT_EQUAL_UINT32(1, ctr);
  // Reject: truncated, bad CRC, unknown ID at this length.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(11, 0xAD)));
  uint8_t bad[12];
  for (size_t i = 0; i < sizeof(bad); ++i) {
    bad[i] = frame[i];
  }
  bad[5] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xFF)));
}

// PAIR_REFRESH_REQ 14B + PAIR_REFRESH_ACK 10B (README §pair-refresh vectors: era 1, counter 100,
// psk 0011..eeff). The ACK is TX-only: the RX demux must drop it.
void test_udp_pair_refresh_frame_goldens() {
  const uint8_t req[14] = {0x53, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0xEC, 0xAC, 0x8E, 0xB7, 0xC9, 0x3C, 0xBA};
  TEST_ASSERT_TRUE(mirrorXorOk(req, sizeof(req)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kPairRefreshReq),
                    rxKindInt(udpstatus::classifyRx(sizeof(req), req[0])));
  const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(req[1]) << 8) | static_cast<uint16_t>(req[2]));
  TEST_ASSERT_EQUAL_UINT16(1, era);
  const uint8_t ack[10] = {0x54, 0x00, 0x01, 0x62, 0xC2, 0x9E, 0xA4, 0xD1, 0x81, 0x9F};
  TEST_ASSERT_TRUE(mirrorXorOk(ack, sizeof(ack)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(sizeof(ack), ack[0])));
  // Reject: truncated REQ, bad CRC on either frame, unknown ID at 14B/10B.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x53)));
  uint8_t badReq[14];
  for (size_t i = 0; i < sizeof(badReq); ++i) {
    badReq[i] = req[i];
  }
  badReq[7] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badReq, sizeof(badReq)));
  uint8_t badAck[10];
  for (size_t i = 0; i < sizeof(badAck); ++i) {
    badAck[i] = ack[i];
  }
  badAck[9] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badAck, sizeof(badAck)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xFF)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(10, 0xFF)));
}

// ALIVE 14B + DISCONNECT 14B (README §alive vectors: era 1, counter 100,
// psk 0011..eeff). Era/counter extraction mirrors handleAlive/handleDisconnect.
void test_udp_alive_disconnect_frame_goldens() {
  const uint8_t alive[14] = {0x55, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0xDD, 0x15, 0x2B, 0xAA, 0x54, 0xF7, 0xDA};
  TEST_ASSERT_TRUE(mirrorXorOk(alive, sizeof(alive)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kAlive), rxKindInt(udpstatus::classifyRx(sizeof(alive), alive[0])));
  const uint8_t disc[14] = {0x56, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0xCA, 0x90, 0xCA, 0x03, 0x90, 0x40, 0x70};
  TEST_ASSERT_TRUE(mirrorXorOk(disc, sizeof(disc)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDisconnect), rxKindInt(udpstatus::classifyRx(sizeof(disc), disc[0])));
  const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(alive[1]) << 8) | static_cast<uint16_t>(alive[2]));
  TEST_ASSERT_EQUAL_UINT16(1, era);
  const uint32_t ctr = (static_cast<uint32_t>(alive[3]) << 24) | (static_cast<uint32_t>(alive[4]) << 16) |
                       (static_cast<uint32_t>(alive[5]) << 8) | static_cast<uint32_t>(alive[6]);
  TEST_ASSERT_EQUAL_UINT32(100, ctr);
  // Reject: truncated, bad CRC on either frame, unknown ID at 14B.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x55)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x56)));
  uint8_t badAlive[14];
  for (size_t i = 0; i < sizeof(badAlive); ++i) {
    badAlive[i] = alive[i];
  }
  badAlive[7] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badAlive, sizeof(badAlive)));
  uint8_t badDisc[14];
  for (size_t i = 0; i < sizeof(badDisc); ++i) {
    badDisc[i] = disc[i];
  }
  badDisc[13] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badDisc, sizeof(badDisc)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xFF)));
}

// MANUAL_TAKE 14B + MANUAL_RELEASE 14B (README manual vectors: era 1,
// counter 100, psk 0011..eeff). Era/counter extraction mirrors
// handleManualTake/handleManualRelease.
void test_udp_manual_frame_goldens() {
  const uint8_t take[14] = {0x57, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x1E, 0xE9, 0x89, 0x58, 0x6F, 0x8E, 0xF5};
  TEST_ASSERT_TRUE(mirrorXorOk(take, sizeof(take)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kManualTake), rxKindInt(udpstatus::classifyRx(sizeof(take), take[0])));
  const uint8_t rel[14] = {0x58, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0xE2, 0xCE, 0x42, 0x67, 0x15, 0xB5, 0x94};
  TEST_ASSERT_TRUE(mirrorXorOk(rel, sizeof(rel)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kManualRelease),
                    rxKindInt(udpstatus::classifyRx(sizeof(rel), rel[0])));
  const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(take[1]) << 8) | static_cast<uint16_t>(take[2]));
  TEST_ASSERT_EQUAL_UINT16(1, era);
  const uint32_t ctr = (static_cast<uint32_t>(take[3]) << 24) | (static_cast<uint32_t>(take[4]) << 16) |
                       (static_cast<uint32_t>(take[5]) << 8) | static_cast<uint32_t>(take[6]);
  TEST_ASSERT_EQUAL_UINT32(100, ctr);
  // Reject: truncated, bad CRC on either frame, unknown ID at 14B.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x57)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0x58)));
  uint8_t badTake[14];
  for (size_t i = 0; i < sizeof(badTake); ++i) {
    badTake[i] = take[i];
  }
  badTake[7] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badTake, sizeof(badTake)));
  uint8_t badRel[14];
  for (size_t i = 0; i < sizeof(badRel); ++i) {
    badRel[i] = rel[i];
  }
  badRel[13] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(badRel, sizeof(badRel)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xFF)));
}

// Telemetry MANUAL session bit: a snapshot with the session held encodes
// bit 4, and the bit rides the wire through encode/decode untouched.
void test_telemetry_encode_snapshot_manual_bit() {
  telemetry::Snapshot s;
  s.mode = 0;
  udp::TelemetryPacket pkt;
  telemetry::encodeSnapshot(s, pkt);
  TEST_ASSERT_EQUAL_UINT8(0x00, pkt.statusFlags);
  s.manual = true;
  telemetry::encodeSnapshot(s, pkt);
  TEST_ASSERT_EQUAL_UINT8(0x10, pkt.statusFlags);
  TEST_ASSERT_TRUE(udpstatus::statusManual(pkt.statusFlags));
  uint8_t buf[udp::kTelemetrySize] = {};
  TEST_ASSERT_TRUE(udp::encodeTelemetry(pkt, buf));
  udp::TelemetryPacket dst;
  TEST_ASSERT_TRUE(udp::decodeTelemetry(buf, sizeof(buf), dst));
  TEST_ASSERT_EQUAL_UINT8(0x10, dst.statusFlags);
  TEST_ASSERT_TRUE(udpstatus::statusManual(dst.statusFlags));
  TEST_ASSERT_EQUAL_UINT8(0, udpstatus::statusMode(dst.statusFlags));
}

// PAIR_OK framing (TX-only, no RX decode): v1 38B vs v1.1 39B share header
// 0x51; the GCM ct/tag are ARDUINO-only (mbedTLS) so no crypto golden lives
// here — the sizes + RX-drop + Kotlin/bench cross-vectors are the gate.
void test_udp_pair_ok_framing() {
  TEST_ASSERT_EQUAL(39, static_cast<int>(pairing::kPairOkSize));
  TEST_ASSERT_EQUAL(38, static_cast<int>(pairing::kPairOkLegacySize));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(38, 0x51)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(39, 0x51)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(38, 0xFF)));
}

// Expression trigger demux: 0x5B claims the 15B trigger shape. 0x59/0x5A are
// retired-reserved: the RX demux drops them at every length. The HMAC tag is
// ARDUINO-only (mbedTLS); the cross-side golden lives in
// shared/protocol/README.md (trig_tag=95ade6a7dea4 for era=1, counter=100,
// id=1 under the shared psk).
void test_udp_classify_expr() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kExprTrigger), rxKindInt(udpstatus::classifyRx(15, 0x5B)));
  // Neighbouring sizes/headers still drop.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x5B)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(16, 0x5B)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0xAC)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0xFF)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0xFF)));
  // Retired-reserved: 0x59 drops at every length (incl. its old 14B shape),
  // and the 0x5A list answer never arrives on the RX path.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x59)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0x59)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(4, 0x5A)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(88, 0x5A)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(148, 0x5A)));
}

// EXPR_TRIGGER 15B [0x5B|id|eraBE2|ctrBE4|tag6|XOR]: id 1, era 1, counter 100
// under psk 0011..eeff, tag 95ade6a7dea4.
void test_udp_expr_trigger_frame_golden() {
  const uint8_t frame[15] = {0x5B, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x95, 0xAD, 0xE6, 0xA7, 0xDE, 0xA4, 0x3C};
  TEST_ASSERT_TRUE(mirrorXorOk(frame, sizeof(frame)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kExprTrigger),
                    rxKindInt(udpstatus::classifyRx(sizeof(frame), frame[0])));
  TEST_ASSERT_EQUAL_UINT8(0x01, frame[1]);
  const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(frame[2]) << 8) | static_cast<uint16_t>(frame[3]));
  TEST_ASSERT_EQUAL_UINT16(1, era);
  const uint32_t ctr = (static_cast<uint32_t>(frame[4]) << 24) | (static_cast<uint32_t>(frame[5]) << 16) |
                       (static_cast<uint32_t>(frame[6]) << 8) | static_cast<uint32_t>(frame[7]);
  TEST_ASSERT_EQUAL_UINT32(100, ctr);
  // Reject: truncated, bad CRC, unknown ID at 15B.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(14, 0x5B)));
  uint8_t bad[15];
  for (size_t i = 0; i < sizeof(bad); ++i) {
    bad[i] = frame[i];
  }
  bad[8] ^= 0xFF;
  TEST_ASSERT_FALSE(mirrorXorOk(bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(15, 0xFF)));
}
