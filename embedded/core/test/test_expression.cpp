// Host unit tests for the core expression owner (no hardware).
// Run: pio test -e native-test
//
// Covers the codec id mirror + neutral predicate, the manager one-active
// policy (priority order, preemption, lower-never-displaces, neutral release,
// yield-to-neutral, play-once expiry/retrigger with fake millis, wrap
// safety, fail-closed unknown ids), and the task staged-dispatch latch.
// Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/abilities/expression/expression_codec.h"
#include "../src/abilities/expression/expression_manager.h"
#include "../src/abilities/expression/expression_task.h"

void test_exprcodec_ids_mirror_catalog() {
  TEST_ASSERT_EQUAL_UINT8(exprcat::kBoot, expression::kBoot);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kHappy, expression::kHappy);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSad, expression::kSad);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kBlink, expression::kBlink);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kAlert, expression::kAlert);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kCurious, expression::kCurious);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSurprised, expression::kSurprised);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kCount, expression::kCount);
}

void test_exprcodec_neutral_family() {
  TEST_ASSERT_TRUE(expression::isNeutral(expression::kBoot));
  TEST_ASSERT_TRUE(expression::isNeutral(expression::kBlink));
  TEST_ASSERT_FALSE(expression::isNeutral(expression::kHappy));
  TEST_ASSERT_FALSE(expression::isNeutral(expression::kCurious));
  TEST_ASSERT_FALSE(expression::isNeutral(expression::kSad));
  TEST_ASSERT_FALSE(expression::isNeutral(expression::kAlert));
  TEST_ASSERT_FALSE(expression::isNeutral(expression::kSurprised));
  TEST_ASSERT_FALSE(expression::isNeutral(5));
  TEST_ASSERT_FALSE(expression::isNeutral(0xFF));
}

void test_exprmgr_starts_neutral() {
  expression::Manager mgr;
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_priority_order() {
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kBoot) < exprcat::priorityFor(expression::kHappy));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kHappy) < exprcat::priorityFor(expression::kCurious));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kCurious) < exprcat::priorityFor(expression::kSad));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kSad) < exprcat::priorityFor(expression::kAlert));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kAlert) < exprcat::priorityFor(expression::kSurprised));
}

void test_exprmgr_higher_preempts_lower() {
  expression::Manager mgr;
  const uint8_t chain[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert,
                           expression::kSurprised};
  for (size_t i = 0; i < sizeof(chain); ++i) {
    TEST_ASSERT_TRUE(mgr.request(chain[i], 0));
    TEST_ASSERT_EQUAL_UINT8(chain[i], mgr.active());
  }
}

void test_exprmgr_lower_never_displaces_higher() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0));
  const uint8_t lower[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert};
  for (size_t i = 0; i < sizeof(lower); ++i) {
    TEST_ASSERT_FALSE(mgr.request(lower[i], 100));
    TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  }
  TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 150));
  TEST_ASSERT_TRUE(mgr.request(expression::kAlert, 200));
  TEST_ASSERT_FALSE(mgr.request(expression::kHappy, 300));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, mgr.active());
}

void test_exprmgr_neutral_releases_from_any() {
  expression::Manager mgr;
  const uint8_t moods[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert,
                           expression::kSurprised};
  for (size_t i = 0; i < sizeof(moods); ++i) {
    TEST_ASSERT_TRUE(mgr.request(moods[i], 0));
    TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 10));
    TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  }
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 20));
  TEST_ASSERT_TRUE(mgr.request(expression::kBlink, 30));
  TEST_ASSERT_EQUAL_UINT8(expression::kBlink, mgr.active());
}

void test_exprmgr_yields_neutral_not_previous_mood() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kHappy, 0));
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 10));
  TEST_ASSERT_TRUE(mgr.tick(10 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_TRUE(mgr.request(expression::kHappy, 2000));
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 2010));
  TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 2020));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_play_once_expiry() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 1000));
  TEST_ASSERT_FALSE(mgr.tick(1000 + exprcat::kSurprisedHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  TEST_ASSERT_TRUE(mgr.tick(1000 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_FALSE(mgr.tick(1000 + exprcat::kSurprisedHoldMs + 1));
}

void test_exprmgr_play_once_retrigger_extends_hold() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0));
  TEST_ASSERT_FALSE(mgr.tick(exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_FALSE(mgr.tick(2 * exprcat::kSurprisedHoldMs - 101));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  TEST_ASSERT_TRUE(mgr.tick(2 * exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_hold_wraps_and_persists() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0xFFFFFF00u));
  TEST_ASSERT_FALSE(mgr.tick(0xFFFFFF00u + 100));
  TEST_ASSERT_TRUE(mgr.tick(0xFFFFFF00u + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 5000));
  TEST_ASSERT_FALSE(mgr.tick(5000 + 100000));
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, mgr.active());
}

void test_exprmgr_unknown_ids_fail_closed() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kAlert, 0));
  const uint8_t bad[] = {5, 8, 99, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    TEST_ASSERT_FALSE(mgr.request(bad[i], 100));
    TEST_ASSERT_EQUAL_UINT8(expression::kAlert, mgr.active());
  }
}

void test_exprtask_request_stages_dispatch_id() {
  expression::Task task;
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_TRUE(task.request(expression::kSad, 0));
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.staged());
  task.markDispatched();
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.active());
}

void test_exprtask_gated_request_keeps_latch() {
  expression::Task task;
  TEST_ASSERT_TRUE(task.request(expression::kAlert, 0));
  TEST_ASSERT_FALSE(task.request(expression::kHappy, 100));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
  TEST_ASSERT_FALSE(task.request(99, 200));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
}

void test_exprtask_tick_yield_stages_neutral() {
  expression::Task task;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  task.markDispatched();
  TEST_ASSERT_FALSE(task.tick(exprcat::kSurprisedHoldMs - 1));
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_TRUE(task.tick(exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, task.staged());
}

void test_exprtask_placeholders_stay_none() {
  for (size_t i = 0; i < exprcat::count(); ++i) {
    TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundNone, exprcat::kTable[i].sound);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionNone, exprcat::kTable[i].motion);
  }
}

namespace {
// Host seam for the dispatch pump: mirrors LinkServer::sendUnicast.
struct FakeExprLink {
  bool ok = true;
  unsigned calls = 0;
  uint8_t lastType = 0;
  uint8_t lastByte = 0xFF;
  uint16_t lastLen = 0;
  bool sendUnicast(uint8_t type, const uint8_t* payload, uint16_t len, uint16_t& seqOut) {
    ++calls;
    if (!ok) {
      return false;
    }
    lastType = type;
    lastLen = len;
    lastByte = (payload != nullptr && len == 1) ? payload[0] : 0xFF;
    seqOut = 7;
    return true;
  }
};
}  // namespace

void test_exprtask_pump_sends_staged_once() {
  expression::Task task;
  FakeExprLink link;
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(0, link.calls);
  TEST_ASSERT_TRUE(task.request(expression::kSad, 0));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
  TEST_ASSERT_EQUAL_UINT8(0x08, link.lastType);
  TEST_ASSERT_EQUAL_UINT16(1, link.lastLen);
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, link.lastByte);
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
}

void test_exprtask_pump_refused_send_keeps_latch() {
  expression::Task task;
  FakeExprLink link;
  link.ok = false;
  TEST_ASSERT_TRUE(task.request(expression::kAlert, 0));
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
  link.ok = true;
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, link.lastByte);
}

void test_exprtask_pump_tick_yield_sends_neutral() {
  expression::Task task;
  FakeExprLink link;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, link.lastByte);
  TEST_ASSERT_TRUE(task.tick(exprcat::kSurprisedHoldMs));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, link.lastByte);
}
