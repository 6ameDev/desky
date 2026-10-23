// WiFi link client pure top half (no hardware).

#include <unity.h>

#include "../src/services/cli_manager.h"
#include "../src/transport/link_codec.h"

void test_wifilink_seq_first_fresh_then_duplicate_then_stale() {
  using V = wifilink::RxVerdict;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(false, 0, 0)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(false, 41, 7)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(true, 10, 11)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kDuplicate),
                          static_cast<uint8_t>(wifilink::classifyRx(true, 10, 10)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kStale), static_cast<uint8_t>(wifilink::classifyRx(true, 10, 9)));
}

void test_wifilink_seq_wraps() {
  using V = wifilink::RxVerdict;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(true, 0xFFFF, 0)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kStale), static_cast<uint8_t>(wifilink::classifyRx(true, 0, 0xFFFF)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kStale), static_cast<uint8_t>(wifilink::classifyRx(true, 0, 0x8000)));
}

void test_wifilink_type_names() {
  TEST_ASSERT_EQUAL_STRING("FACE", wifilink::typeName(linkcode::kTypeFace));
  TEST_ASSERT_EQUAL_STRING("OLED", wifilink::typeName(linkcode::kTypeOled));
  TEST_ASSERT_EQUAL_STRING("CAMCTL", wifilink::typeName(linkcode::kTypeCamCtl));
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", wifilink::typeName(linkcode::kTypePairShow));
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", wifilink::typeName(linkcode::kTypePairDone));
  TEST_ASSERT_EQUAL_STRING("JPEG", wifilink::typeName(linkcode::kTypeJpeg));
  TEST_ASSERT_EQUAL_STRING("HB", wifilink::typeName(linkcode::kTypeHb));
  TEST_ASSERT_EQUAL_STRING("HB_ACK", wifilink::typeName(linkcode::kTypeHbAck));
  TEST_ASSERT_EQUAL_STRING("ACK", wifilink::typeName(linkcode::kTypeAck));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0xFF));
}

void test_wifilink_map_face() {
  PowerCommand cmd;
  const uint8_t id = 3;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeFace, &id, 1, cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(3, cmd.arg);
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeFace, &id, 2, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeFace, nullptr, 1, cmd));
}

void test_wifilink_map_oled_rows() {
  PowerCommand cmd;
  const uint8_t on = linkcode::kOledOn;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeOled, &on, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_ON, cmd.arg);
  const uint8_t dim = linkcode::kOledDim;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeOled, &dim, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_DIM, cmd.arg);
  const uint8_t off = linkcode::kOledOff;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeOled, &off, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_OFF, cmd.arg);
  const uint8_t bad = 3;
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeOled, &bad, 1, cmd));
}

void test_wifilink_map_cam() {
  PowerCommand cmd;
  const uint8_t on = linkcode::kCamOn;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeCamCtl, &on, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  const uint8_t off = linkcode::kCamOff;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeCamCtl, &off, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  const uint8_t bad = 2;
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeCamCtl, &bad, 1, cmd));
}

void test_wifilink_map_rejects_non_commands() {
  PowerCommand cmd;
  const uint8_t one = 0;
  TEST_ASSERT_FALSE(wifilink::mapCmd(0x04, &one, 1, cmd));  // retired QRBITMAP: stays unmapped
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairShow, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairDone, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeJpeg, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeHb, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeHbAck, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeAck, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(0xFF, &one, 1, cmd));
}

void test_wifilink_hb_and_retry_gates() {
  TEST_ASSERT_TRUE(wifilink::shouldSendHb(1000, 0, 1000));
  TEST_ASSERT_FALSE(wifilink::shouldSendHb(999, 0, 1000));
  TEST_ASSERT_TRUE(wifilink::shouldSendHb(5, 0xFFFFFFF0u, 20));  // wrap-safe
  TEST_ASSERT_TRUE(wifilink::retryDue(5000, 0, 5000));
  TEST_ASSERT_FALSE(wifilink::retryDue(4999, 0, 5000));
}

// Assoc-attempt driver: begin() is async; the verdict arrives on a later tick.
void test_wifilink_assoc_step_begins_once_then_waits() {
  using A = wifilink::AssocAction;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kBegin),
                          static_cast<uint8_t>(wifilink::assocStep(false, false, 6000, 0, 10000)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kWait),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 6000, 1000, 10000)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kWait),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 10999, 1000, 10000)));
}

void test_wifilink_assoc_step_connects_on_later_tick() {
  using A = wifilink::AssocAction;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kConnected),
                          static_cast<uint8_t>(wifilink::assocStep(true, true, 6000, 1000, 10000)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kConnected),
                          static_cast<uint8_t>(wifilink::assocStep(false, true, 6000, 0, 10000)));
}

void test_wifilink_assoc_step_timeout_restarts() {
  using A = wifilink::AssocAction;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kRestart),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 11000, 1000, 10000)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kRestart),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 12000, 1000, 10000)));
}

void test_wifilink_assoc_step_wrap_safe() {
  using A = wifilink::AssocAction;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kWait),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 10, 0xFFFFFFF0u, 10000)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(A::kRestart),
                          static_cast<uint8_t>(wifilink::assocStep(true, false, 11000, 0xFFFFFFF0u, 10000)));
}

void test_wifilink_link_down_rule() {
  TEST_ASSERT_FALSE(wifilink::linkDownAfter(0));
  TEST_ASSERT_FALSE(wifilink::linkDownAfter(2));
  TEST_ASSERT_TRUE(wifilink::linkDownAfter(3));
  TEST_ASSERT_TRUE(wifilink::linkDownAfter(200));
}

void test_wifilink_frag_plan() {
  TEST_ASSERT_EQUAL_size_t(1024, wifilink::perFramePayload());
  TEST_ASSERT_EQUAL_size_t(1, wifilink::fragTotalFor(0));
  TEST_ASSERT_EQUAL_size_t(1, wifilink::fragTotalFor(1024));
  TEST_ASSERT_EQUAL_size_t(2, wifilink::fragTotalFor(1025));
  size_t off = 0;
  size_t len = 0;
  TEST_ASSERT_TRUE(wifilink::fragSlice(2500, 1024, 0, 3, off, len));
  TEST_ASSERT_EQUAL_size_t(0, off);
  TEST_ASSERT_EQUAL_size_t(1024, len);
  TEST_ASSERT_TRUE(wifilink::fragSlice(2500, 1024, 2, 3, off, len));
  TEST_ASSERT_EQUAL_size_t(2048, off);
  TEST_ASSERT_EQUAL_size_t(452, len);
  TEST_ASSERT_FALSE(wifilink::fragSlice(2500, 1024, 3, 3, off, len));
  TEST_ASSERT_FALSE(wifilink::fragSlice(2500, 0, 0, 3, off, len));
  TEST_ASSERT_TRUE(wifilink::fragSlice(0, 1024, 0, 1, off, len));
  TEST_ASSERT_EQUAL_size_t(0, len);
  TEST_ASSERT_FALSE(wifilink::fragSlice(0, 1024, 1, 2, off, len));
}

void test_cli_link_and_snap_parse() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("link", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::LINK), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(headcmd::parseLine("snap", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::SNAP), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(headcmd::parseLine("  snap  ", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("LINK", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("SNAP", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("link status", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("snapshot", cmd));
}
