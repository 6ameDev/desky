// Expression priority + trigger paths (no hardware): one active V1 id, higher
// preempts lower, neutral releases, play-once self-clears with fake millis.

#include <stdio.h>
#include <unity.h>

#include "../src/abilities/display/display_manager.h"
#include "../src/services/cli_manager.h"
#include "../src/services/power_manager.h"  // headapply:: (Arduino-free top half)
#include "../src/transport/link_codec.h"    // linkcode:: alias + wifilink::
#include "../src/transport/link_stub.h"

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

void test_expr_higher_preempts_lower() {
  DisplayManager mgr;
  DisplayTransition t;
  const uint8_t chain[] = {FACE_HAPPY, FACE_CURIOUS, FACE_SAD, FACE_ALERT, FACE_SURPRISED};
  for (size_t i = 0; i < sizeof(chain); ++i) {
    TEST_ASSERT_TRUE(mgr.showEyes(chain[i], t, 0));
    TEST_ASSERT_EQUAL_UINT8(chain[i], mgr.eyesId());
  }
}

void test_expr_lower_never_displaces_higher() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, 0));
  const uint8_t lower[] = {FACE_HAPPY, FACE_CURIOUS, FACE_SAD, FACE_ALERT};
  for (size_t i = 0; i < sizeof(lower); ++i) {
    TEST_ASSERT_FALSE(mgr.showEyes(lower[i], t, 100));
    TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, mgr.eyesId());
  }
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_BOOT, t, 150));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_ALERT, t, 200));
  TEST_ASSERT_FALSE(mgr.showEyes(FACE_HAPPY, t, 300));
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
}

void test_expr_neutral_releases_from_any() {
  DisplayManager mgr;
  DisplayTransition t;
  const uint8_t moods[] = {FACE_HAPPY, FACE_CURIOUS, FACE_SAD, FACE_ALERT, FACE_SURPRISED};
  for (size_t i = 0; i < sizeof(moods); ++i) {
    TEST_ASSERT_TRUE(mgr.showEyes(moods[i], t, 0));
    TEST_ASSERT_TRUE(mgr.showEyes(FACE_BOOT, t, 10));
    TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  }
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SAD, t, 20));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_BLINK, t, 30));
  TEST_ASSERT_EQUAL_UINT8(FACE_BLINK, mgr.eyesId());
}

void test_expr_yields_neutral_not_previous_mood() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_HAPPY, t, 0));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, 10));
  TEST_ASSERT_TRUE(mgr.exprTick(10 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_HAPPY, t, 2000));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SAD, t, 2010));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_BOOT, t, 2020));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
}

void test_expr_play_once_auto_release() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, 1000));
  TEST_ASSERT_FALSE(mgr.exprTick(1000 + exprcat::kSurprisedHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.exprTick(1000 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_FALSE(mgr.exprTick(1000 + exprcat::kSurprisedHoldMs + 1));
}

void test_expr_play_once_retrigger_extends_hold() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, 0));
  TEST_ASSERT_FALSE(mgr.exprTick(exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_FALSE(mgr.exprTick(2 * exprcat::kSurprisedHoldMs - 101));
  TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.exprTick(2 * exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
}

void test_expr_hold_wraps_and_persists() {
  DisplayManager mgr;
  DisplayTransition t;
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SURPRISED, t, 0xFFFFFF00u));
  TEST_ASSERT_FALSE(mgr.exprTick(0xFFFFFF00u + 100));
  TEST_ASSERT_TRUE(mgr.exprTick(0xFFFFFF00u + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, mgr.eyesId());
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_SAD, t, 5000));
  TEST_ASSERT_FALSE(mgr.exprTick(5000 + 100000));
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, mgr.eyesId());
}

void test_expr_gated_request_still_clears_overlays() {
  DisplayManager mgr;
  DisplayTransition t;
  const uint8_t pin[pairdisplay::kPinLen] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_TRUE(mgr.showEyes(FACE_ALERT, t, 0));
  TEST_ASSERT_TRUE(mgr.showPairing(pin, 60, 0, t));
  TEST_ASSERT_TRUE(mgr.pairActive());
  TEST_ASSERT_FALSE(mgr.showEyes(FACE_HAPPY, t, 10));
  TEST_ASSERT_TRUE(t.clearedPair);
  TEST_ASSERT_FALSE(mgr.pairActive());
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, mgr.eyesId());
}

void test_expr_trigger_paths_cover_all_five() {
  struct Row {
    uint8_t id;
    const char* name;
    uint8_t emotion;
  };
  const Row rows[] = {{FACE_BOOT, "boot", eyesmap::kEmotionNormal},
                      {FACE_HAPPY, "happy", eyesmap::kEmotionHappy},
                      {FACE_SAD, "sad", eyesmap::kEmotionSad},
                      {FACE_CURIOUS, "curious", eyesmap::kEmotionAwe},
                      {FACE_SURPRISED, "surprised", eyesmap::kEmotionSurprised}};
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
    // CLI door: name + numeric.
    PowerCommand cli;
    char line[24];
    snprintf(line, sizeof(line), "face %s", rows[i].name);
    TEST_ASSERT_TRUE(headcmd::parseLine(line, cli));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cli.target));
    TEST_ASSERT_EQUAL_INT32(rows[i].id, cli.arg);
    // Link stub door: numeric.
    PowerCommand stub;
    char uline[16];
    snprintf(uline, sizeof(uline), "FACE %u", static_cast<unsigned>(rows[i].id));
    TEST_ASSERT_TRUE(linkstub::mapLine(uline, stub));
    TEST_ASSERT_EQUAL_INT32(rows[i].id, stub.arg);
    // WiFi frame door: passthrough, clamped downstream.
    PowerCommand wire;
    const uint8_t raw = rows[i].id;
    TEST_ASSERT_TRUE(wifilink::mapCmd(linkcode::kTypeFace, &raw, 1, wire));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(wire.target));
    // End to end from neutral: resolve -> gate -> render row.
    DisplayManager mgr;
    DisplayTransition t;
    const uint8_t resolved = headapply::resolveFace(wire.arg);
    TEST_ASSERT_EQUAL_UINT8(rows[i].id, resolved);
    TEST_ASSERT_TRUE(mgr.showEyes(resolved, t, 0));
    TEST_ASSERT_EQUAL_UINT8(rows[i].id, mgr.eyesId());
    TEST_ASSERT_EQUAL_UINT8(rows[i].emotion, eyesmap::emotionForFace(mgr.eyesId()));
  }
}
