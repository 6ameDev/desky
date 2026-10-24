// Host unit tests for the core->head expression command (no hardware).
// Run: pio test -e native-test
//
// Covers kTypeExpr=0x08: the shared codec literal contract (1B catalog id),
// frame goldens, and fail-closed pack/unpack (QR 5 excluded). Runner lives
// in test_udp_codec.cpp.
//
// Wire literal below is computed, not symbolic: seq=0x0045 id=HAPPY(1) in an
// EXPR frame is
//   4C 08 00 45 00 01 00 01 01 90 D4  (11B total).

#include <unity.h>

#include "link/link_codec.h"

namespace {
const uint8_t kExprGolden[11] = {0x4C, 0x08, 0x00, 0x45, 0x00, 0x01, 0x00, 0x01, 0x01, 0x90, 0xD4};
}  // namespace

void test_link_expr_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x08, link::kTypeExpr);
  TEST_ASSERT_TRUE(link::isKnownType(0x08));
  TEST_ASSERT_EQUAL_size_t(1, link::kExprLen);
  TEST_ASSERT_TRUE(link::isValidPayloadLen(link::kTypeExpr, 1));
  TEST_ASSERT_FALSE(link::isValidPayloadLen(link::kTypeExpr, 0));
  TEST_ASSERT_FALSE(link::isValidPayloadLen(link::kTypeExpr, 2));
}

void test_link_expr_golden_encode() {
  const uint8_t payload[1] = {0x01};
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypeExpr, 0x0045, payload, sizeof(payload), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(kExprGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kExprGolden, out, sizeof(kExprGolden));
  TEST_ASSERT_EQUAL_UINT8(0x08, out[1]);  // literal EXPR type byte
}

void test_link_expr_golden_decode() {
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(kExprGolden, sizeof(kExprGolden), f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypeExpr, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0045, f.seq);
  TEST_ASSERT_EQUAL_UINT8(0, f.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, f.fragTotal);
  TEST_ASSERT_EQUAL_UINT16(1, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8(0x01, f.payload[0]);
}

void test_link_expr_pack_unpack_round_trip() {
  const uint8_t ids[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
  for (size_t i = 0; i < sizeof(ids); ++i) {
    uint8_t wire = 0xFF;
    TEST_ASSERT_TRUE(link::packExpr(ids[i], wire));
    TEST_ASSERT_EQUAL_UINT8(ids[i], wire);
    uint8_t id = 0xFF;
    TEST_ASSERT_TRUE(link::unpackExpr(ids[i], id));
    TEST_ASSERT_EQUAL_UINT8(ids[i], id);
  }
}

void test_link_expr_pack_unpack_fail_closed() {
  // QR 5 has no catalog row; 0x0C/0x63/0xFF are outside the table.
  const uint8_t bad[] = {0x05, 0x0C, 0x63, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    uint8_t wire = 0xAA;
    TEST_ASSERT_FALSE(link::packExpr(bad[i], wire));
    uint8_t id = 0xAA;
    TEST_ASSERT_FALSE(link::unpackExpr(bad[i], id));
  }
}
