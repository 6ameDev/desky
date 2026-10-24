// Host unit tests for the shared expression catalog (link/expression_catalog.h).
// Pins every V1 row (ids, names, priorities, play-once flags, eye commands,
// NONE placeholders) plus the fail-closed lookup. The id mirror against the
// head FACE_* vocabulary is pinned here; head pins the eyes/priority mirror.

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
  TEST_ASSERT_EQUAL_UINT8(FACE_COUNT, exprcat::kCount);
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::kBoot);
  TEST_ASSERT_EQUAL_UINT8(1, exprcat::kHappy);
  TEST_ASSERT_EQUAL_UINT8(2, exprcat::kSad);
  TEST_ASSERT_EQUAL_UINT8(3, exprcat::kBlink);
  TEST_ASSERT_EQUAL_UINT8(4, exprcat::kAlert);
  TEST_ASSERT_EQUAL_UINT8(6, exprcat::kCurious);
  TEST_ASSERT_EQUAL_UINT8(7, exprcat::kSurprised);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::kCount);
}

void test_exprcat_count_and_order() {
  TEST_ASSERT_EQUAL_size_t(7, exprcat::count());
  TEST_ASSERT_EQUAL_size_t(7, exprcat::kEntryCount);
  static const uint8_t kOrder[] = {0, 1, 2, 3, 4, 6, 7};
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
  for (size_t i = 0; i < exprcat::count(); ++i) {
    TEST_ASSERT_EQUAL_STRING(faceName(exprcat::kTable[i].id), exprcat::kTable[i].name);
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
  TEST_ASSERT_TRUE(exprcat::priorityFor(0) < exprcat::priorityFor(1));
  TEST_ASSERT_TRUE(exprcat::priorityFor(1) < exprcat::priorityFor(6));
  TEST_ASSERT_TRUE(exprcat::priorityFor(6) < exprcat::priorityFor(2));
  TEST_ASSERT_TRUE(exprcat::priorityFor(2) < exprcat::priorityFor(4));
  TEST_ASSERT_TRUE(exprcat::priorityFor(4) < exprcat::priorityFor(7));
}

void test_exprcat_play_once_and_hold() {
  TEST_ASSERT_TRUE(exprcat::isPlayOnce(7));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(0));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(1));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(2));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(3));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(4));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(6));
  TEST_ASSERT_EQUAL_UINT32(1500, exprcat::holdMsFor(7));
  TEST_ASSERT_EQUAL_UINT32(exprcat::kSurprisedHoldMs, exprcat::holdMsFor(7));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(0));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(1));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(4));
}

void test_exprcat_eye_commands() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::lookup(0)->eye);
  TEST_ASSERT_EQUAL_UINT8(3, exprcat::lookup(1)->eye);
  TEST_ASSERT_EQUAL_UINT8(4, exprcat::lookup(2)->eye);
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::lookup(3)->eye);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::lookup(4)->eye);
  TEST_ASSERT_EQUAL_UINT8(17, exprcat::lookup(6)->eye);
  TEST_ASSERT_EQUAL_UINT8(8, exprcat::lookup(7)->eye);
}

void test_exprcat_placeholders_none() {
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kSoundNone);
  TEST_ASSERT_EQUAL_UINT8(0xFF, exprcat::kMotionNone);
  for (size_t i = 0; i < exprcat::count(); ++i) {
    TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundNone, exprcat::kTable[i].sound);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionNone, exprcat::kTable[i].motion);
  }
}

void test_exprcat_lookup_fail_closed() {
  TEST_ASSERT_NULL(exprcat::lookup(5));
  TEST_ASSERT_NULL(exprcat::lookup(8));
  TEST_ASSERT_NULL(exprcat::lookup(99));
  TEST_ASSERT_NULL(exprcat::lookup(0xFF));
  static const uint8_t kKnown[] = {0, 1, 2, 3, 4, 6, 7};
  for (size_t i = 0; i < sizeof(kKnown); ++i) {
    const exprcat::Entry* e = exprcat::lookup(kKnown[i]);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(kKnown[i], e->id);
  }
}

void test_exprcat_accessors_fail_closed() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(5));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(8));
  TEST_ASSERT_EQUAL_UINT8(0, exprcat::priorityFor(99));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(5));
  TEST_ASSERT_FALSE(exprcat::isPlayOnce(8));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(5));
  TEST_ASSERT_EQUAL_UINT32(0, exprcat::holdMsFor(99));
}
