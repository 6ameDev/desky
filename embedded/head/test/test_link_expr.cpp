// Core->head expression command (no hardware): EXPR 0x08 1B frames through
// the map seam into the single gated request entry, unknown-id neutral
// fallback, pairing/OTA arbitration, bench trigger path intact.

#include <unity.h>

#include "../src/abilities/display/display_manager.h"
#include "../src/services/power_manager.h"  // headapply:: (Arduino-free top half)
#include "../src/transport/link_codec.h"    // linkcode:: alias + wifilink::

namespace {

// EXPR golden: seq 0x0045, id HAPPY(1), CRC 0x90D4.
const uint8_t kExprGolden[11] = {0x4C, 0x08, 0x00, 0x45, 0x00, 0x01, 0x00, 0x01, 0x01, 0x90, 0xD4};
const uint8_t kPairPin[pairdisplay::kPinLen] = {'1', '2', '3', '4', '5', '6'};

}  // namespace

void test_link_expr_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x08, linkcode::kTypeExpr);
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x08));
  TEST_ASSERT_EQUAL_size_t(1, linkcode::kExprLen);
  TEST_ASSERT_EQUAL_STRING("EXPR", wifilink::typeName(0x08));
  TEST_ASSERT_TRUE(linkcode::isValidPayloadLen(linkcode::kTypeExpr, 1));
  TEST_ASSERT_FALSE(linkcode::isValidPayloadLen(linkcode::kTypeExpr, 0));
  TEST_ASSERT_FALSE(linkcode::isValidPayloadLen(linkcode::kTypeExpr, 2));
}

void test_link_expr_golden_round_trip() {
  uint8_t out[linkcode::kMaxFrame] = {};
  const uint8_t payload[1] = {FACE_HAPPY};
  const size_t n = linkcode::encodeFrame(linkcode::kTypeExpr, 0x0045, payload, 1, 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(kExprGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kExprGolden, out, sizeof(kExprGolden));
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kExprGolden, sizeof(kExprGolden), f));
  TEST_ASSERT_EQUAL_UINT8(linkcode::kTypeExpr, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0045, f.seq);
  TEST_ASSERT_EQUAL_UINT16(1, f.payloadLen);
  uint8_t id = 0xFF;
  TEST_ASSERT_TRUE(linkcode::unpackExpr(f.payload[0], id));
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, id);
}

void test_link_expr_unpack_fail_closed() {
  const uint8_t good[] = {FACE_BOOT,        FACE_HAPPY,        FACE_SAD,          FACE_BLINK,
                          FACE_ALERT,       FACE_CURIOUS,      FACE_SURPRISED,    exprcat::kAngry,
                          exprcat::kScared, exprcat::kExcited, exprcat::kConfused};
  for (size_t i = 0; i < sizeof(good); ++i) {
    uint8_t id = 0xFF;
    TEST_ASSERT_TRUE(linkcode::unpackExpr(good[i], id));
    TEST_ASSERT_EQUAL_UINT8(good[i], id);
    uint8_t wire = 0xFF;
    TEST_ASSERT_TRUE(linkcode::packExpr(good[i], wire));
    TEST_ASSERT_EQUAL_UINT8(good[i], wire);
  }
  // QR has no catalog row; the rest sit outside the table.
  const uint8_t bad[] = {FACE_QR, 12, 99, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    uint8_t id = 0xAA;
    TEST_ASSERT_FALSE(linkcode::unpackExpr(bad[i], id));
    uint8_t wire = 0xAA;
    TEST_ASSERT_FALSE(linkcode::packExpr(bad[i], wire));
  }
}

void test_link_expr_maps_to_face_gate() {
  PowerCommand cmd;
  const uint8_t happy = FACE_HAPPY;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &happy, 1, cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(FACE_HAPPY, cmd.arg);
  const uint8_t surprised = FACE_SURPRISED;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &surprised, 1, cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_SURPRISED, cmd.arg);
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeExpr, &happy, 2, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypeExpr, nullptr, 1, cmd));
}

void test_link_expr_unknown_maps_neutral() {
  PowerCommand cmd;
  const uint8_t bad[] = {FACE_QR, 12, 99, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &bad[i], 1, cmd));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
    TEST_ASSERT_EQUAL_INT32(FACE_BOOT, cmd.arg);
    TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(headapply::resolveFace(cmd.arg)));
  }
}

void test_link_expr_v2_ids_route_to_paint() {
  const uint8_t v2[] = {exprcat::kAngry, exprcat::kScared, exprcat::kExcited, exprcat::kConfused};
  for (size_t i = 0; i < sizeof(v2); ++i) {
    PowerCommand cmd;
    TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &v2[i], 1, cmd));
    TEST_ASSERT_EQUAL_INT32(v2[i], cmd.arg);
    const uint8_t resolved = headapply::resolveFace(cmd.arg);
    TEST_ASSERT_EQUAL_UINT8(v2[i], resolved);
    DisplayManager mgr;
    DisplayTransition t;
    TEST_ASSERT_TRUE(mgr.paintFace(resolved, t));
    TEST_ASSERT_EQUAL_UINT8(v2[i], mgr.eyesId());
    TEST_ASSERT_EQUAL_UINT8(eyesmap::emotionForFace(v2[i]), eyesmap::emotionForFace(mgr.eyesId()));
  }
}

void test_link_expr_gated_entry_routes_eyes() {
  DisplayManager mgr;
  DisplayTransition t;
  PowerCommand cmd;
  const uint8_t raw = FACE_SAD;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &raw, 1, cmd));
  const uint8_t resolved = headapply::resolveFace(cmd.arg);
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, resolved);
  TEST_ASSERT_TRUE(mgr.paintFace(resolved, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, mgr.eyesId());
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionSad, eyesmap::emotionForFace(mgr.eyesId()));
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_ALERT, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, mgr.eyesId());
}

void test_link_expr_pairing_screen_survives_command() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showPairing(kPairPin, 60, 0, t));
  TEST_ASSERT_TRUE(mgr.pairActive());
  TEST_ASSERT_FALSE(mgr.paintFace(FACE_ALERT, t));
  TEST_ASSERT_TRUE(mgr.pairActive());
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  // Eyes otherwise: once the overlay clears, the same entry applies.
  TEST_ASSERT_TRUE(mgr.clearPairing(t));
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_ALERT, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
}

void test_link_expr_ota_screen_survives_command() {
  DisplayManager mgr;
  DisplayTransition t;
  mgr.showOta(30, 0, t);
  TEST_ASSERT_TRUE(mgr.otaActive());
  TEST_ASSERT_FALSE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_TRUE(mgr.otaActive());
  TEST_ASSERT_EQUAL_UINT8(30, mgr.otaPct());
}

void test_link_expr_bench_path_intact() {
  // Bench path: EXPR frame bytes decode -> map -> resolve -> paint.
  uint8_t produced[linkcode::kMaxFrame] = {};
  const uint8_t curiousPayload[1] = {FACE_CURIOUS};
  const size_t n =
      linkcode::encodeFrame(linkcode::kTypeExpr, 0x0046, curiousPayload, 1, 0, 1, produced, sizeof(produced));
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + 1, n);
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(produced, n, f));
  PowerCommand wire;
  TEST_ASSERT_TRUE(wifilink::mapCmd(f.type, f.payload, f.payloadLen, wire));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(wire.target));
  DisplayManager mgr;
  DisplayTransition t;
  const uint8_t resolved = headapply::resolveFace(wire.arg);
  TEST_ASSERT_EQUAL_UINT8(FACE_CURIOUS, resolved);
  TEST_ASSERT_TRUE(mgr.paintFace(resolved, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_CURIOUS, mgr.eyesId());
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionAwe, eyesmap::emotionForFace(mgr.eyesId()));
  DisplayManager link;
  PowerCommand happy;
  const uint8_t raw = FACE_HAPPY;
  TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &raw, 1, happy));
  TEST_ASSERT_TRUE(link.paintFace(headapply::resolveFace(happy.arg), t));
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, link.eyesId());
}
