// Host unit tests for the expression picker wire (no hardware).
// Run: pio test -e native-test
//
// Covers the EXPR_LIST_REQ / EXPR_TRIGGER tag-message layouts both sides
// must agree on, the EXPR_LIST_RESP composer served from the shared catalog
// (ver/count/entries, blank-padded names, play-once flags, XOR, fail-closed
// composes), and the MANUAL-only trigger gate. HMAC tags are ARDUINO-only
// (mbedTLS) and pinned as cross-side vectors in shared/protocol/README.md —
// these host tests pin the message layouts. Runner lives in
// test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/abilities/expression/expression_codec.h"
#include "../src/abilities/pairing/pairing_codec.h"

void test_expr_wire_consts() {
  TEST_ASSERT_EQUAL_UINT8(0x59, pairing::kExprListReqHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(pairing::kExprListReqSize));
  TEST_ASSERT_EQUAL_UINT8(0x5A, expression::kExprListRespHeader);
  TEST_ASSERT_EQUAL_UINT8(1, expression::kExprListRespVer);
  TEST_ASSERT_EQUAL(12, static_cast<int>(expression::kExprListRespMaxCount));
  TEST_ASSERT_EQUAL(10, static_cast<int>(expression::kExprNameLen));
  TEST_ASSERT_EQUAL(12, static_cast<int>(expression::kExprEntryLen));
  TEST_ASSERT_EQUAL_UINT8(0x5B, pairing::kExprTriggerHeader);
  TEST_ASSERT_EQUAL(15, static_cast<int>(pairing::kExprTriggerSize));
  TEST_ASSERT_EQUAL(6, static_cast<int>(pairing::kExprTagLen));
  TEST_ASSERT_EQUAL(14, static_cast<int>(pairing::kExprListMsgLen));
  TEST_ASSERT_EQUAL(15, static_cast<int>(pairing::kExprTriggerMsgLen));
  // The wire cap covers the catalog with headroom (never a second copy).
  TEST_ASSERT_TRUE(exprcat::count() <= expression::kExprListRespMaxCount);
}

void test_expr_list_tag_msg_golden() {
  uint8_t msg[pairing::kExprListMsgLen];
  pairing::exprListTagMsg(1u, 100u, msg);
  // "EXPRLIST" || eraBE2(0001) || counterBE4(00000064).
  const uint8_t expected[14] = {0x45, 0x58, 0x50, 0x52, 0x4C, 0x49, 0x53, 0x54, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  // Era/counter lay out big-endian.
  pairing::exprListTagMsg(0x0102u, 0x01020304u, msg);
  const uint8_t be[14] = {0x45, 0x58, 0x50, 0x52, 0x4C, 0x49, 0x53, 0x54, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(be, msg, sizeof(be));
  // Null-safe (no crash on null out).
  pairing::exprListTagMsg(1u, 1u, nullptr);
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

void test_expr_list_resp_layout_matches_catalog() {
  uint8_t resp[4 + expression::kExprEntryLen * expression::kExprListRespMaxCount] = {};
  const size_t len = expression::composeExprListResp(resp, sizeof(resp));
  const size_t count = exprcat::count();
  TEST_ASSERT_EQUAL(4 + expression::kExprEntryLen * count, len);
  TEST_ASSERT_EQUAL_UINT8(expression::kExprListRespHeader, resp[0]);
  TEST_ASSERT_EQUAL_UINT8(expression::kExprListRespVer, resp[1]);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(count), resp[2]);
  // Every entry mirrors the shared row: id, bit0 = play-once, blank-padded
  // name (sound/motion stay off the wire).
  for (size_t i = 0; i < count; ++i) {
    const exprcat::Entry& e = exprcat::kTable[i];
    const size_t o = 3 + expression::kExprEntryLen * i;
    TEST_ASSERT_EQUAL_UINT8(e.id, resp[o]);
    TEST_ASSERT_EQUAL_UINT8(e.playOnce ? 0x01 : 0x00, resp[o + 1]);
    TEST_ASSERT_EQUAL_UINT8(0, resp[o + 1] & 0xFE);
    size_t n = 0;
    while (e.name[n] != '\0') {
      TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(e.name[n]), resp[o + 2 + n]);
      ++n;
    }
    TEST_ASSERT_TRUE(n <= expression::kExprNameLen);
    for (size_t p = n; p < expression::kExprNameLen; ++p) {
      TEST_ASSERT_EQUAL_UINT8(' ', resp[o + 2 + p]);
    }
  }
  // XOR over all preceding bytes closes the frame.
  uint8_t xorSum = 0;
  for (size_t i = 0; i + 1 < len; ++i) {
    xorSum ^= resp[i];
  }
  TEST_ASSERT_EQUAL_UINT8(xorSum, resp[len - 1]);
}

void test_expr_list_resp_golden() {
  uint8_t resp[4 + expression::kExprEntryLen * expression::kExprListRespMaxCount] = {};
  const size_t len = expression::composeExprListResp(resp, sizeof(resp));
  TEST_ASSERT_EQUAL(88, len);
  const uint8_t expected[88] = {0x5A, 0x01, 0x07, 0x00, 0x00, 'b', 'o',  'o',  't', ' ',  ' ',  ' ', ' ',  ' ',  ' ',
                                0x01, 0x00, 'h',  'a',  'p',  'p', 'y',  ' ',  ' ', ' ',  ' ',  ' ', 0x02, 0x00, 's',
                                'a',  'd',  ' ',  ' ',  ' ',  ' ', ' ',  ' ',  ' ', 0x03, 0x00, 'b', 'l',  'i',  'n',
                                'k',  ' ',  ' ',  ' ',  ' ',  ' ', 0x04, 0x00, 'a', 'l',  'e',  'r', 't',  ' ',  ' ',
                                ' ',  ' ',  ' ',  0x06, 0x00, 'c', 'u',  'r',  'i', 'o',  'u',  's', ' ',  ' ',  ' ',
                                0x07, 0x01, 's',  'u',  'r',  'p', 'r',  'i',  's', 'e',  'd',  ' ', 0x4D};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, resp, sizeof(expected));
}

void test_expr_list_resp_fail_closed() {
  uint8_t resp[4 + expression::kExprEntryLen * expression::kExprListRespMaxCount] = {};
  TEST_ASSERT_EQUAL(0, expression::composeExprListResp(nullptr, sizeof(resp)));
  TEST_ASSERT_EQUAL(0, expression::composeExprListResp(resp, 0));
  TEST_ASSERT_EQUAL(0, expression::composeExprListResp(resp, 3));
  // One byte short of the catalog frame still refuses.
  const size_t full = 4 + expression::kExprEntryLen * exprcat::count();
  TEST_ASSERT_EQUAL(0, expression::composeExprListResp(resp, full - 1));
  // Exact fit succeeds.
  TEST_ASSERT_EQUAL(full, expression::composeExprListResp(resp, full));
}

void test_expr_trigger_gate_manual_only() {
  TEST_ASSERT_TRUE(expression::triggerAllowed(sysmode::Mode::kManual));
  TEST_ASSERT_FALSE(expression::triggerAllowed(sysmode::Mode::kNormal));
  TEST_ASSERT_FALSE(expression::triggerAllowed(sysmode::Mode::kPairing));
  TEST_ASSERT_FALSE(expression::triggerAllowed(sysmode::Mode::kOta));
}
