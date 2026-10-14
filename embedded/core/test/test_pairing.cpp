// Host unit tests for the pairing core (no hardware).
// Run: pio test -e native-test
//
// Covers Task 2: golden accept + burn-once, bad-code tries, 5th-try lockout
// with 30s expiry, 5-min window expiry, pre-window attempts, malformed
// input. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/middleware/pairing.h"

namespace {

const uint8_t kCode[pairing::kCodeLen] = {'1', '2', '3', '4', '5', '6'};
const uint8_t kWrong[pairing::kCodeLen] = {'6', '5', '4', '3', '2', '1'};

}  // namespace

void test_pair_golden_accept_burns_once() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(1000, kCode));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kOk),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 1001)));
  TEST_ASSERT_TRUE(core.bonded());
  // Same correct code replayed post-bond: never OK twice.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kAlreadyBonded),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 1002)));
}

void test_pair_bad_code_counts_tries() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 10)));
  TEST_ASSERT_EQUAL_UINT32(1, core.tries());
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 11)));
  TEST_ASSERT_EQUAL_UINT32(2, core.tries());
  TEST_ASSERT_FALSE(core.bonded());
}

void test_pair_fifth_try_locks_then_expires() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  for (uint32_t i = 0; i < pairing::kMaxTries - 1; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                      static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 10 + i)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kLocked),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 20)));
  // Still locked just before 30s.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kLocked),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 20 + pairing::kLockoutMs - 1)));
  // After lockout the correct code works in the same window.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kOk),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 20 + pairing::kLockoutMs)));
}

void test_pair_window_expiry_rejects() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  TEST_ASSERT_EQUAL(
      static_cast<int>(pairing::Result::kNotInWindow),
      static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, pairing::kWindowMs + 1)));
}

void test_pair_pre_window_rejects() {
  pairing::Core core;
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kNotInWindow),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 0)));
}

void test_pair_malformed_input_is_bad_code() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  const uint8_t shortCode[4] = {'1', '2', '3', '4'};
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(shortCode, sizeof(shortCode), 10)));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(nullptr, pairing::kCodeLen, 11)));
  const uint8_t nonDigit[pairing::kCodeLen] = {'1', '2', '3', '4', '5', 'X'};
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(nonDigit, pairing::kCodeLen, 12)));
  TEST_ASSERT_FALSE(core.bonded());
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the pairing tests are declared extern there.
