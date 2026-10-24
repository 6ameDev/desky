// Expression paint gate (no hardware): held overlays reject with zero state change; a live QR show clears.

#include <unity.h>

#include "../src/abilities/display/display_manager.h"
#include "../src/services/power_manager.h"  // headapply:: (Arduino-free top half)
#include "../src/transport/link_codec.h"    // linkcode:: alias + wifilink::

void test_expr_priority_order() {
  TEST_ASSERT_GREATER_THAN_UINT8(exprcat::priorityFor(FACE_BOOT), exprcat::priorityFor(FACE_HAPPY));
  TEST_ASSERT_GREATER_THAN_UINT8(exprcat::priorityFor(FACE_HAPPY), exprcat::priorityFor(FACE_CURIOUS));
  TEST_ASSERT_GREATER_THAN_UINT8(exprcat::priorityFor(FACE_CURIOUS), exprcat::priorityFor(FACE_SAD));
  TEST_ASSERT_GREATER_THAN_UINT8(exprcat::priorityFor(FACE_SAD), exprcat::priorityFor(FACE_ALERT));
  TEST_ASSERT_GREATER_THAN_UINT8(exprcat::priorityFor(FACE_ALERT), exprcat::priorityFor(FACE_SURPRISED));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(FACE_BOOT));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(FACE_BLINK));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(FACE_QR));
}

void test_expr_play_once_shape() {
  TEST_ASSERT_TRUE(exprcat::isPlayOnce(FACE_SURPRISED));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(FACE_ALERT));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(FACE_SAD));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(FACE_BOOT));
  TEST_ASSERT_EQUAL_UINT32(1500, exprcat::holdMsFor(FACE_SURPRISED));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(FACE_ALERT));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(FACE_HAPPY));
}

void test_expr_paint_stores_every_id() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_SURPRISED, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_BOOT, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_BLINK, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BLINK, mgr.eyesId());
  const uint8_t v2[] = {FACE_ANGRY, FACE_SCARED, FACE_EXCITED, FACE_CONFUSED};
  for (size_t i = 0; i < sizeof(v2); ++i) {
    TEST_ASSERT_TRUE(mgr.paintFace(v2[i], t));
    TEST_ASSERT_EQUAL_UINT8(v2[i], mgr.eyesId());
    TEST_ASSERT_EQUAL_UINT8(exprcat::lookup(v2[i])->eye, eyesmap::emotionForFace(mgr.eyesId()));
  }
}

void test_expr_paint_unknown_clamps_boot() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_QR, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(static_cast<uint8_t>(FACE_COUNT), t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(99, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.paintFace(0xFF, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
}

void test_expr_paint_held_pairing_rejects_zero_change() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_ALERT, t));
  const uint8_t pin[pairdisplay::kPinLen] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_TRUE(mgr.showPairing(pin, 60, 0, t));
  TEST_ASSERT_TRUE(mgr.pairActive());
  TEST_ASSERT_FALSE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_TRUE(mgr.pairActive());
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
  TEST_ASSERT_EQUAL_UINT32(60, mgr.remainingSecs(0));
}

void test_expr_paint_held_ota_rejects_zero_change() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_SAD, t));
  mgr.showOta(30, 0, t);
  TEST_ASSERT_TRUE(mgr.otaActive());
  TEST_ASSERT_FALSE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_TRUE(mgr.otaActive());
  TEST_ASSERT_EQUAL_UINT8(30, mgr.otaPct());
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, mgr.eyesId());
}

void test_expr_paint_clears_qr_only() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_CURIOUS, t));
  uint8_t qr[qrdisplay::kQrLen] = {};
  TEST_ASSERT_TRUE(mgr.showQr(qr, 0, t));
  TEST_ASSERT_TRUE(mgr.qrActive());
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_HAPPY, t));
  TEST_ASSERT_TRUE(t.clearedQr);
  TEST_ASSERT_FALSE(mgr.qrActive());
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, mgr.eyesId());
}

void test_expr_paint_applies_after_overlay_clears() {
  DisplayManager mgr;
  DisplayTransition t;
  const uint8_t pin[pairdisplay::kPinLen] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_TRUE(mgr.showPairing(pin, 60, 0, t));
  TEST_ASSERT_FALSE(mgr.paintFace(FACE_ALERT, t));
  TEST_ASSERT_TRUE(mgr.clearPairing(t));
  TEST_ASSERT_TRUE(mgr.paintFace(FACE_ALERT, t));
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
}

void test_expr_trigger_paths_cover_all_five() {
  struct Row {
    uint8_t id;
    uint8_t emotion;
  };
  const Row rows[] = {{FACE_BOOT, eyesmap::kEmotionNormal},
                      {FACE_HAPPY, eyesmap::kEmotionHappy},
                      {FACE_SAD, eyesmap::kEmotionSad},
                      {FACE_CURIOUS, eyesmap::kEmotionAwe},
                      {FACE_SURPRISED, eyesmap::kEmotionSurprised}};
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
    // WiFi frame door: EXPR id rides the wire, clamped downstream.
    PowerCommand wire;
    const uint8_t raw = rows[i].id;
    TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeExpr, &raw, 1, wire));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(wire.target));
    TEST_ASSERT_EQUAL_INT32(rows[i].id, wire.arg);
    // Single paint entry stores the resolved id verbatim.
    DisplayManager mgr;
    DisplayTransition t;
    const uint8_t resolved = headapply::resolveFace(wire.arg);
    TEST_ASSERT_EQUAL_UINT8(rows[i].id, resolved);
    TEST_ASSERT_TRUE(mgr.paintFace(resolved, t));
    TEST_ASSERT_EQUAL_UINT8(rows[i].id, mgr.eyesId());
    TEST_ASSERT_EQUAL_UINT8(rows[i].emotion, eyesmap::emotionForFace(mgr.eyesId()));
  }
}
