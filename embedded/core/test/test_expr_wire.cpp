// Host unit tests for the expression trigger wire (no hardware).
// Run: pio test -e native-test
//
// Covers the EXPR_TRIGGER tag-message layout both sides must agree on and
// the MANUAL-only trigger gate. The HMAC tag is ARDUINO-only (mbedTLS) and
// pinned as a cross-side vector in shared/protocol/README.md — these host
// tests pin the message layout. Runner lives in test_udp_codec.cpp (single
// main).

#include <unity.h>

#include "../src/abilities/expression/expression_codec.h"
#include "../src/abilities/pairing/pairing_codec.h"
#include "../src/behavior/mode_manager.h"

void test_expr_wire_consts() {
  TEST_ASSERT_EQUAL_UINT8(0x5B, pairing::kExprTriggerHeader);
  TEST_ASSERT_EQUAL(15, static_cast<int>(pairing::kExprTriggerSize));
  TEST_ASSERT_EQUAL(6, static_cast<int>(pairing::kExprTagLen));
  TEST_ASSERT_EQUAL(15, static_cast<int>(pairing::kExprTriggerMsgLen));
  // 0x59/0x5A stay claimed forever (retired-reserved, never reused).
  TEST_ASSERT_EQUAL_UINT8(0x59, udp::kExprListReqHeader);
  TEST_ASSERT_EQUAL_UINT8(0x5A, udp::kExprListRespHeader);
}

void test_expr_trigger_tag_msg_golden() {
  uint8_t msg[pairing::kExprTriggerMsgLen];
  pairing::exprTriggerTagMsg(1u, 1u, 100u, msg);
  // "EXPRTRIG" || id(01) || eraBE2(0001) || counterBE4(00000064).
  const uint8_t expected[15] = {0x45, 0x58, 0x50, 0x52, 0x54, 0x52, 0x49, 0x47,
                                0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  // Id rides between the prefix and the era; era/counter lay out big-endian.
  pairing::exprTriggerTagMsg(0x07u, 0x0102u, 0x01020304u, msg);
  const uint8_t be[15] = {0x45, 0x58, 0x50, 0x52, 0x54, 0x52, 0x49, 0x47, 0x07, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(be, msg, sizeof(be));
  // Null-safe (no crash on null out).
  pairing::exprTriggerTagMsg(1u, 1u, 1u, nullptr);
}

void test_expr_trigger_gate_manual_only() {
  TEST_ASSERT_TRUE(sysmode::exprTriggerAllowed(sysmode::Mode::kManual));
  TEST_ASSERT_FALSE(sysmode::exprTriggerAllowed(sysmode::Mode::kNormal));
  TEST_ASSERT_FALSE(sysmode::exprTriggerAllowed(sysmode::Mode::kPairing));
  TEST_ASSERT_FALSE(sysmode::exprTriggerAllowed(sysmode::Mode::kOta));
}
