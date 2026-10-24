// Host unit tests for the shared expression catalog (link/expression_catalog.h).
// Pins every V1+V2 row (ids, names, priorities, play-once flags, eye commands,
// media/pose bundles, NONE placeholders on V1 rows) plus the fail-closed
// lookup. The id mirror against the head FACE_* vocabulary is pinned here;
// head pins the eyes/priority mirror.

#include <string.h>
#include <unity.h>

#include "head_context.h"
#include "link/expression_catalog.h"

void test_exprcat_ids_mirror_face_vocabulary() {
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, exprcat::kBoot);
  TEST_ASSERT_EQUAL_UINT8(FACE_HAPPY, exprcat::kHappy);
  TEST_ASSERT_EQUAL_UINT8(FACE_SAD, exprcat::kSad);
  TEST_ASSERT_EQUAL_UINT8(FACE_BLINK, exprcat::kBlink);
  TEST_ASSERT_EQUAL_UINT8(FACE_ALERT, exprcat::kAlert);
  TEST_ASSERT_EQUAL_UINT8(FACE_CURIOUS, exprcat::kCurious);
  TEST_ASSERT_EQUAL_UINT8(FACE_SURPRISED, exprcat::kSurprised);
  TEST_ASSERT_EQUAL_UINT8(12, FACE_COUNT);
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::kBoot);
  TEST_ASSERT_EQUAL_UINT8(1, exprcat::kHappy);
  TEST_ASSERT_EQUAL_UINT8(2, exprcat::kSad);
  TEST_ASSERT_EQUAL_UINT8(3, exprcat::kBlink);
  TEST_ASSERT_EQUAL_UINT8(4, exprcat::kAlert);
  TEST_ASSERT_EQUAL_UINT8(6, exprcat::kCurious);
  TEST_ASSERT_EQUAL_UINT8(7, exprcat::kSurprised);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::kAngry);
  TEST_ASSERT_EQUAL_UINT8(9, exprcat::kScared);
  TEST_ASSERT_EQUAL_UINT8(10, exprcat::kExcited);
  TEST_ASSERT_EQUAL_UINT8(11, exprcat::kConfused);
  TEST_ASSERT_EQUAL_UINT8(12, exprcat::kCount);
}

void test_exprcat_count_and_order() {
  TEST_ASSERT_EQUAL_size_t(11, exprcat::count());
  TEST_ASSERT_EQUAL_size_t(11, exprcat::kEntryCount);
  static const uint8_t kOrder[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11};
  for (size_t i = 0; i < sizeof(kOrder); ++i) {
    TEST_ASSERT_EQUAL_UINT8(kOrder[i], exprcat::kTable[i].id);
  }
}

void test_exprcat_names() {
  TEST_ASSERT_EQUAL_STRING("boot", exprcat::lookup(0)->name);
  TEST_ASSERT_EQUAL_STRING("happy", exprcat::lookup(1)->name);
  TEST_ASSERT_EQUAL_STRING("sad", exprcat::lookup(2)->name);
  TEST_ASSERT_EQUAL_STRING("blink", exprcat::lookup(3)->name);
  TEST_ASSERT_EQUAL_STRING("alert", exprcat::lookup(4)->name);
  TEST_ASSERT_EQUAL_STRING("curious", exprcat::lookup(6)->name);
  TEST_ASSERT_EQUAL_STRING("surprised", exprcat::lookup(7)->name);
  TEST_ASSERT_EQUAL_STRING("angry", exprcat::lookup(8)->name);
  TEST_ASSERT_EQUAL_STRING("scared", exprcat::lookup(9)->name);
  TEST_ASSERT_EQUAL_STRING("excited", exprcat::lookup(10)->name);
  TEST_ASSERT_EQUAL_STRING("confused", exprcat::lookup(11)->name);
  static const uint8_t kV1[] = {0, 1, 2, 3, 4, 6, 7};
  for (size_t i = 0; i < sizeof(kV1); ++i) {
    TEST_ASSERT_EQUAL_STRING(faceName(kV1[i]), exprcat::lookup(kV1[i])->name);
  }
}

void test_exprcat_priorities() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(0));
  TEST_ASSERT_EQUAL_UINT8(1, exprcat::priorityFor(1));
  TEST_ASSERT_EQUAL_UINT8(3, exprcat::priorityFor(2));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(3));
  TEST_ASSERT_EQUAL_UINT8(4, exprcat::priorityFor(4));
  TEST_ASSERT_EQUAL_UINT8(2, exprcat::priorityFor(6));
  TEST_ASSERT_EQUAL_UINT8(5, exprcat::priorityFor(7));
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::priorityFor(8));
  TEST_ASSERT_EQUAL_UINT8(9, exprcat::priorityFor(9));
  TEST_ASSERT_EQUAL_UINT8(7, exprcat::priorityFor(10));
  TEST_ASSERT_EQUAL_UINT8(6, exprcat::priorityFor(11));
  TEST_ASSERT_TRUE(exprcat::priorityFor(0) < exprcat::priorityFor(1));
  TEST_ASSERT_TRUE(exprcat::priorityFor(1) < exprcat::priorityFor(6));
  TEST_ASSERT_TRUE(exprcat::priorityFor(6) < exprcat::priorityFor(2));
  TEST_ASSERT_TRUE(exprcat::priorityFor(2) < exprcat::priorityFor(4));
  TEST_ASSERT_TRUE(exprcat::priorityFor(4) < exprcat::priorityFor(7));
  TEST_ASSERT_TRUE(exprcat::priorityFor(7) < exprcat::priorityFor(11));
  TEST_ASSERT_TRUE(exprcat::priorityFor(11) < exprcat::priorityFor(10));
  TEST_ASSERT_TRUE(exprcat::priorityFor(10) < exprcat::priorityFor(8));
  TEST_ASSERT_TRUE(exprcat::priorityFor(8) < exprcat::priorityFor(9));
}

void test_exprcat_play_once_and_hold() {
  TEST_ASSERT_TRUE(exprcat::isPlayOnce(7));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(0));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(1));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(2));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(3));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(4));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(6));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(8));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(9));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(10));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(11));
  TEST_ASSERT_EQUAL_UINT32(1500, exprcat::holdMsFor(7));
  TEST_ASSERT_EQUAL_UINT32(exprcat::kSurprisedHoldMs, exprcat::holdMsFor(7));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(0));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(1));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(4));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(8));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(9));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(10));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(11));
}

void test_exprcat_eye_commands() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::lookup(0)->eye);
  TEST_ASSERT_EQUAL_UINT8(3, exprcat::lookup(1)->eye);
  TEST_ASSERT_EQUAL_UINT8(4, exprcat::lookup(2)->eye);
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::lookup(3)->eye);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::lookup(4)->eye);
  TEST_ASSERT_EQUAL_UINT8(17, exprcat::lookup(6)->eye);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::lookup(7)->eye);
  TEST_ASSERT_EQUAL_UINT8(1, exprcat::lookup(8)->eye);
  TEST_ASSERT_EQUAL_UINT8(16, exprcat::lookup(9)->eye);
  TEST_ASSERT_EQUAL_UINT8(2, exprcat::lookup(10)->eye);
  TEST_ASSERT_EQUAL_UINT8(9, exprcat::lookup(11)->eye);
}

void test_exprcat_placeholders_none() {
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kSoundNone);
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kMotionNone);
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kNeckNone);
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kArmNone);
  static const uint8_t kV1[] = {0, 1, 2, 3, 4, 6, 7};
  for (size_t i = 0; i < sizeof(kV1); ++i) {
    const exprcat::Entry* e = exprcat::lookup(kV1[i]);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundNone, e->sound);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionNone, e->motion);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckNone, e->neckPose);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kArmNone, e->armPose);
  }
}

void test_exprcat_v2_bundles() {
  const exprcat::Entry* angry = exprcat::lookup(8);
  const exprcat::Entry* scared = exprcat::lookup(9);
  const exprcat::Entry* excited = exprcat::lookup(10);
  const exprcat::Entry* confused = exprcat::lookup(11);
  TEST_ASSERT_NOT_NULL(angry);
  TEST_ASSERT_NOT_NULL(scared);
  TEST_ASSERT_NOT_NULL(excited);
  TEST_ASSERT_NOT_NULL(confused);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundAngry, angry->sound);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionShake, angry->motion);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckAngry, angry->neckPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kArmAngry, angry->armPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundScared, scared->sound);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionCower, scared->motion);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckScared, scared->neckPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kArmScared, scared->armPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundExcited, excited->sound);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionBounce, excited->motion);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckExcited, excited->neckPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kArmExcited, excited->armPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundConfused, confused->sound);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionTilt, confused->motion);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckConfused, confused->neckPose);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kArmConfused, confused->armPose);
}

void test_exprcat_lookup_fail_closed() {
  TEST_ASSERT_NULL(exprcat::lookup(5));
  TEST_ASSERT_NULL(exprcat::lookup(12));
  TEST_ASSERT_NULL(exprcat::lookup(13));
  TEST_ASSERT_NULL(exprcat::lookup(99));
  TEST_ASSERT_NULL(exprcat::lookup(0xFF));
  static const uint8_t kKnown[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11};
  for (size_t i = 0; i < sizeof(kKnown); ++i) {
    const exprcat::Entry* e = exprcat::lookup(kKnown[i]);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(kKnown[i], e->id);
  }
}

void test_exprcat_accessors_fail_closed() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(5));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(12));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(13));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(99));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(5));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(12));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(5));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(99));
}
