// Head face-id mapping (no hardware).

#include <unity.h>

#include "../src/abilities/display/display_codec.h"
#include "../src/abilities/display/display_task.h"

void test_face_names_match_ids() {
  TEST_ASSERT_EQUAL_STRING("boot", faceName(FACE_BOOT));
  TEST_ASSERT_EQUAL_STRING("happy", faceName(FACE_HAPPY));
  TEST_ASSERT_EQUAL_STRING("sad", faceName(FACE_SAD));
  TEST_ASSERT_EQUAL_STRING("blink", faceName(FACE_BLINK));
  TEST_ASSERT_EQUAL_STRING("alert", faceName(FACE_ALERT));
  TEST_ASSERT_EQUAL_STRING("curious", faceName(FACE_CURIOUS));
  TEST_ASSERT_EQUAL_STRING("surprised", faceName(FACE_SURPRISED));
}

void test_face_name_unknown_id() { TEST_ASSERT_EQUAL_STRING("unknown", faceName(9)); }

void test_face_id_from_name_all() {
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, faceIdFromName("boot"));
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, faceIdFromName("happy"));
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, faceIdFromName("sad"));
  TEST_ASSERT_EQUAL_UINT8(FACE_BLINK, faceIdFromName("blink"));
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, faceIdFromName("alert"));
  TEST_ASSERT_EQUAL_UINT8(FACE_CURIOUS, faceIdFromName("curious"));
  TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, faceIdFromName("surprised"));
}

void test_face_id_from_name_rejects() {
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName("Happy"));  // lowercase dialect only
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName("scared"));
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName(""));
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName(nullptr));
}

void test_face_clamp_valid_passthrough() {
  const uint8_t valid[] = {FACE_BOOT, FACE_HAPPY, FACE_SAD, FACE_BLINK, FACE_ALERT, FACE_CURIOUS, FACE_SURPRISED};
  for (size_t i = 0; i < sizeof(valid); ++i) {
    TEST_ASSERT_EQUAL_UINT8(valid[i], clampFace(valid[i]));
  }
}

void test_face_clamp_falls_back_to_boot() {
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(-1));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(static_cast<int>(FACE_COUNT)));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(static_cast<int>(FACE_QR)));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(99));
}

void test_head_event_names() {
  TEST_ASSERT_EQUAL_STRING("FACE_CHANGED", headEventName(HEAD_EVENT_FACE_CHANGED));
  TEST_ASSERT_EQUAL_STRING("CAMERA_STATE", headEventName(HEAD_EVENT_CAMERA_STATE));
  TEST_ASSERT_EQUAL_STRING("LINK_CMD", headEventName(HEAD_EVENT_LINK_CMD));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", headEventName(HEAD_EVENT_COUNT));
}
