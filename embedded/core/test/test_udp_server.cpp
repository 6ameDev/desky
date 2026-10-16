// Host unit tests for the UDP server helpers (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace udpstatus (statusFlags bit map +
// decidegree math in src/middleware/udp_server.h), the coordinator
// packCommand->unpackCommand round-trip at the center-128 edges, and a
// telemetry encode->decode round-trip with flags set. The UdpServer task
// (WiFi/socket/WDT) is firmware-only (#ifdef ARDUINO) and exercised on
// hardware. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/behavior/coordinator.h"
#include "../src/middleware/udp_codec.h"
#include "../src/middleware/udp_server.h"

void test_status_flags_pack_bits() {
  TEST_ASSERT_EQUAL_UINT8(0x01, udpstatus::packStatus(true, false, 0));
  TEST_ASSERT_EQUAL_UINT8(0x02, udpstatus::packStatus(false, true, 0));
  TEST_ASSERT_EQUAL_UINT8(0x08, udpstatus::packStatus(false, false, 2));
  TEST_ASSERT_EQUAL_UINT8(0x0F, udpstatus::packStatus(true, true, 3));
  TEST_ASSERT_EQUAL_UINT8(0x00, udpstatus::packStatus(false, false, 0));
  // Reserved bits 4-7 stay 0 for every mode.
  for (uint8_t mode = 0; mode < 4; ++mode) {
    TEST_ASSERT_EQUAL_UINT8(0, udpstatus::packStatus(true, true, mode) & 0xF0);
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
  coordinator::unpackCommand(coordinator::packCommand(0x02, 0, 255, 0x05), mode, v, omega, flags);
  TEST_ASSERT_EQUAL_UINT8(0x02, mode);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, -1.0f, v);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.0f, omega);
  TEST_ASSERT_EQUAL_UINT8(0x05, flags);
  coordinator::unpackCommand(coordinator::packCommand(0x00, 128, 128, 0x00), mode, v, omega, flags);
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

// Pairing/auth v1 (Task 5): frozen wire constants, wrap-aware counter
// freshness, and the RX demux table. The firmware verify path (HMAC, NVS,
// sockets) is #ifdef ARDUINO and exercised on hardware.

void test_udp_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0xAC, udpstatus::kDriveV2Header);
  TEST_ASSERT_TRUE(udpstatus::kDriveV2Size == 16);
  TEST_ASSERT_EQUAL_UINT8(0x50, udpstatus::kPairReqHeader);
  TEST_ASSERT_TRUE(udpstatus::kPairReqSize == 12);
  // M9 cross-check: the RX demux hard-codes the legacy 6B/0xAA shape instead
  // of including udp_codec.h (see the comment above kDriveV2Header). Pin the
  // copy to the canonical udp::kControlHeader/kControlSize so drift fails
  // here. Single source of truth: shared/protocol (normative:
  // embedded/src/middleware/udp_codec.h). (static_cast: rxKindInt is
  // declared further below, after the classify tests.)
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
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDriveV2), rxKindInt(udpstatus::classifyRx(16, 0xAC)));
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

// PAIR_OK redelivery predicate (Task 1): same code+nonce inside the window
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

// Signed UNBOND (Task 2): wire constants, demux, and the tag-message golden.

void test_udp_unbond_wire_constants() {
  TEST_ASSERT_EQUAL_UINT8(0xAD, udpstatus::kUnbondHeader);
  TEST_ASSERT_TRUE(udpstatus::kUnbondSize == 12);
}

void test_udp_classify_unbond() {
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kUnbond), rxKindInt(udpstatus::classifyRx(12, 0xAD)));
  // Neighbouring sizes/headers still drop.
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(11, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(13, 0xAD)));
  TEST_ASSERT_EQUAL(rxKindInt(udpstatus::RxKind::kDrop), rxKindInt(udpstatus::classifyRx(12, 0xAC)));
}

void test_udp_unbond_tag_msg_golden() {
  uint8_t msg[udpstatus::kUnbondMsgLen];
  udpstatus::unbondTagMsg(1u, msg);
  const uint8_t expected[10] = {0x55, 0x4E, 0x42, 0x4F, 0x4E, 0x44, 0x00, 0x00, 0x00, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
}
