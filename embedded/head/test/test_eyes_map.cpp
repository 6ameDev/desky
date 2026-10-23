// Eyes request map (no hardware): FACE_* ids to vendored eEmotions ordinals.

#include <unity.h>

#include "../src/abilities/display/display_codec.h"

void test_eyes_map_request_ids() {
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(FACE_BOOT));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionHappy, eyesmap::emotionForFace(FACE_HAPPY));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionSad, eyesmap::emotionForFace(FACE_SAD));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(FACE_BLINK));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionSurprised, eyesmap::emotionForFace(FACE_ALERT));
}

void test_eyes_map_ordinals_track_emotions() {
  TEST_ASSERT_EQUAL_UINT8(0, eyesmap::kEmotionNormal);
  TEST_ASSERT_EQUAL_UINT8(3, eyesmap::kEmotionHappy);
  TEST_ASSERT_EQUAL_UINT8(4, eyesmap::kEmotionSad);
  TEST_ASSERT_EQUAL_UINT8(8, eyesmap::kEmotionSurprised);
}

void test_eyes_map_falls_back_to_normal() {
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(FACE_QR));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(FACE_COUNT));
  TEST_ASSERT_EQUAL_UINT8(eyesmap::kEmotionNormal, eyesmap::emotionForFace(99));
}
