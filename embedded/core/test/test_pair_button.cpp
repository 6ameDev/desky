// Host unit tests for the pair-button debounce (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace pairbtn (pairbtn::update in
// src/hal/pair_button.h): active-low short-press edge, exactly once per
// press, bounce inside the debounce window ignored. The PairButton firmware
// class (pinMode/digitalRead) is exercised on hardware. Runner lives in
// test_udp_codec.cpp.

#include <unity.h>

#include "../src/hal/pair_button.h"

namespace {

constexpr uint32_t kDebounce = 50;

}  // namespace

void test_pairbtn_press_fires_once_after_debounce() {
  pairbtn::State s;
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 0, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 10, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 10 + kDebounce - 1, kDebounce));
  TEST_ASSERT_TRUE(pairbtn::update(s, true, 10 + kDebounce, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 10 + kDebounce + 100, kDebounce));
}

void test_pairbtn_bounce_inside_window_ignored() {
  pairbtn::State s;
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 0, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 10, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 20, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 30, kDebounce));
  TEST_ASSERT_TRUE(pairbtn::update(s, true, 30 + kDebounce, kDebounce));
}

void test_pairbtn_release_never_fires_second_press_refires() {
  pairbtn::State s;
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 0, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 1000, kDebounce));
  TEST_ASSERT_TRUE(pairbtn::update(s, true, 1000 + kDebounce, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 2000, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, false, 2000 + kDebounce, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, 3000, kDebounce));
  TEST_ASSERT_TRUE(pairbtn::update(s, true, 3000 + kDebounce, kDebounce));
}

void test_pairbtn_wrap_safe_timing() {
  pairbtn::State s;
  const uint32_t nearWrap = 0xFFFFFFF0u;
  TEST_ASSERT_FALSE(pairbtn::update(s, false, nearWrap, kDebounce));
  TEST_ASSERT_FALSE(pairbtn::update(s, true, nearWrap + 5, kDebounce));
  TEST_ASSERT_TRUE(pairbtn::update(s, true, nearWrap + 5 + kDebounce, kDebounce));
}

void test_pairbtn_cfg_defaults_match_header() {
  TEST_ASSERT_EQUAL_UINT32(50, static_cast<uint32_t>(CFG_PAIR_BUTTON_DEBOUNCE_MS));
}

void test_pairbtn_force_ap_hold_threshold() {
  TEST_ASSERT_EQUAL_UINT32(3000, pairbtn::kForceApHoldMs);
  TEST_ASSERT_FALSE(pairbtn::forceApLatched(0u, 0u));
  TEST_ASSERT_FALSE(pairbtn::forceApLatched(0u, 2999u));
  TEST_ASSERT_TRUE(pairbtn::forceApLatched(0u, 3000u));
  TEST_ASSERT_TRUE(pairbtn::forceApLatched(0u, 60000u));
}

void test_pairbtn_force_ap_hold_injectable_and_wrap_safe() {
  TEST_ASSERT_FALSE(pairbtn::forceApLatched(0u, 499u, 500u));
  TEST_ASSERT_TRUE(pairbtn::forceApLatched(0u, 500u, 500u));
  TEST_ASSERT_FALSE(pairbtn::forceApLatched(0xFFFFFF00u, 0xFFFFFF00u + 2999u));
  TEST_ASSERT_TRUE(pairbtn::forceApLatched(0xFFFFFF00u, 0xFFFFFF00u + 3000u));
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the pairbtn tests are declared extern there.
