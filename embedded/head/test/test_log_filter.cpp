// Host unit tests for the shared log filter (no hardware).
// Run: pio test -e native-test
//
// The matcher + registry are Arduino-free, so the firmware shim's policy
// half is fully covered here; the vprintf install itself is proven on
// hardware (quirk line gone, real errors still print).

#include <unity.h>

#include "common/log_filter.h"

// NOTE: the shared runner (test_main.cpp) owns setUp/tearDown, so each
// test clears the registry explicitly first — rules never leak between
// cases in the single test program.

void test_empty_table_suppresses_nothing() {
  logfilter::clear();
  TEST_ASSERT_EQUAL_UINT8(0, logfilter::ruleCount());
  TEST_ASSERT_FALSE(
      logfilter::isSuppressed("E (314803) gpio: gpio_install_isr_service(534): GPIO isr service already installed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("anything at all"));
}

void test_quirk_line_matches_with_sdk_prefix() {
  logfilter::clear();
  TEST_ASSERT_TRUE(logfilter::addRule("GPIO isr service already installed"));
  TEST_ASSERT_EQUAL_UINT8(1, logfilter::ruleCount());
  TEST_ASSERT_TRUE(
      logfilter::isSuppressed("E (314803) gpio: gpio_install_isr_service(534): GPIO isr service already installed"));
}

void test_other_gpio_errors_still_print() {
  logfilter::clear();
  TEST_ASSERT_TRUE(logfilter::addRule("GPIO isr service already installed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("E (1) gpio: gpio_config failed for pin 99"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("E (2) gpio: gpio_isr_handler_add failed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("W (3) gpio: some other warning"));
}

void test_partial_phrase_does_not_match() {
  logfilter::clear();
  TEST_ASSERT_TRUE(logfilter::addRule("GPIO isr service already installed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("GPIO isr service"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("already installed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed("gpio: something else entirely"));
}

void test_null_and_empty_input_never_match() {
  logfilter::clear();
  TEST_ASSERT_TRUE(logfilter::addRule("GPIO isr service already installed"));
  TEST_ASSERT_FALSE(logfilter::isSuppressed(nullptr));
  TEST_ASSERT_FALSE(logfilter::isSuppressed(""));
}

void test_rejects_bad_rules() {
  logfilter::clear();
  TEST_ASSERT_FALSE(logfilter::addRule(nullptr));
  TEST_ASSERT_FALSE(logfilter::addRule(""));
  TEST_ASSERT_EQUAL_UINT8(0, logfilter::ruleCount());
}

void test_table_full_rejects_overflow() {
  logfilter::clear();
  for (uint8_t i = 0; i < logfilter::kMaxRules; ++i) {
    TEST_ASSERT_TRUE(logfilter::addRule("rule"));
  }
  TEST_ASSERT_EQUAL_UINT8(logfilter::kMaxRules, logfilter::ruleCount());
  TEST_ASSERT_FALSE(logfilter::addRule("one too many"));
}

void test_clear_resets_policy() {
  logfilter::clear();
  TEST_ASSERT_TRUE(logfilter::addRule("GPIO isr service already installed"));
  TEST_ASSERT_TRUE(logfilter::isSuppressed("GPIO isr service already installed"));
  logfilter::clear();
  TEST_ASSERT_EQUAL_UINT8(0, logfilter::ruleCount());
  TEST_ASSERT_FALSE(logfilter::isSuppressed("GPIO isr service already installed"));
}
