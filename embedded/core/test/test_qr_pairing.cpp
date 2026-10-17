// Host unit tests for the QR pairing bitmap (no hardware).
// Run: pio test -e native-test
//
// Covers Phase 0: fixed V1 21x21 geometry, finder patterns at the three
// corners, timing patterns, deterministic re-encode equality, and input
// validation (null / non-digit / short NUL-padded all reject, mirroring
// pairing::openWindow). Runner lives in test_udp_codec.cpp (single main).

#include <string.h>
#include <unity.h>

#include "../src/middleware/qr_pairing.h"

namespace {

const uint8_t kGolden[6] = {'1', '2', '3', '4', '5', '6'};

// 7x7 finder pattern with the top-left corner at (fx, fy): outer ring +
// 3x3 core dark, inner ring light.
bool finderOk(const uint8_t* m, int fx, int fy) {
  for (int dy = 0; dy < 7; ++dy) {
    for (int dx = 0; dx < 7; ++dx) {
      const bool edge = dx == 0 || dx == 6 || dy == 0 || dy == 6;
      const bool core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
      const uint8_t want = (edge || core) ? 1 : 0;
      if (qr::getModule(m, fx + dx, fy + dy) != want) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

void test_qr_size_is_21() { TEST_ASSERT_EQUAL_INT(21, qr::kSize); }

void test_qr_golden_encodes() {
  uint8_t m[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, m));
}

void test_qr_finder_patterns_three_corners() {
  uint8_t m[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, m));
  TEST_ASSERT_TRUE(finderOk(m, 0, 0));
  TEST_ASSERT_TRUE(finderOk(m, qr::kSize - 7, 0));
  TEST_ASSERT_TRUE(finderOk(m, 0, qr::kSize - 7));
}

void test_qr_timing_patterns() {
  uint8_t m[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, m));
  // Timing row 6 / column 6 run dark/light from the separators (8..12).
  for (int i = 8; i <= 12; ++i) {
    const uint8_t want = (i % 2 == 0) ? 1 : 0;
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(m, i, 6));
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(m, 6, i));
  }
  // Dark module at (8, 4*1+9=13) for V1.
  TEST_ASSERT_EQUAL_UINT8(1, qr::getModule(m, 8, 13));
}

void test_qr_deterministic_reencode_equal() {
  uint8_t a[qr::kModules];
  uint8_t b[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, a));
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, b));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, qr::kModules);
}

void test_qr_rejects_null_short_nondigit() {
  uint8_t m[qr::kModules];
  TEST_ASSERT_FALSE(qr::encode6Digits(nullptr, m));
  TEST_ASSERT_FALSE(qr::encode6Digits(kGolden, nullptr));
  const uint8_t nonDigit[6] = {'1', '2', '3', '4', '5', 'X'};
  TEST_ASSERT_FALSE(qr::encode6Digits(nonDigit, m));
  // Short NUL-terminated input ("12" + NULs) fails closed on the NUL bytes.
  const uint8_t shortCode[6] = {'1', '2', 0, 0, 0, 0};
  TEST_ASSERT_FALSE(qr::encode6Digits(shortCode, m));
  // A different valid code still encodes (and differs from the golden).
  const uint8_t other[6] = {'6', '5', '4', '3', '2', '1'};
  uint8_t g[qr::kModules];
  uint8_t o[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, g));
  TEST_ASSERT_TRUE(qr::encode6Digits(other, o));
  TEST_ASSERT_FALSE(memcmp(g, o, qr::kModules) == 0);
}

void test_qr_row_bytes_msb_first() {
  uint8_t m[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, m));
  // Finder top row is 7 dark + separator light: first 7 bits set.
  uint8_t row[qr::kRowBytes];
  qr::rowBytes(m, 0, row);
  TEST_ASSERT_EQUAL_UINT8(0xFE, row[0]);
  // Cross-check every row against getModule bit by bit.
  for (int y = 0; y < qr::kSize; ++y) {
    qr::rowBytes(m, y, row);
    for (int x = 0; x < qr::kSize; ++x) {
      const uint8_t want = qr::getModule(m, x, y);
      const uint8_t got = (row[x >> 3] >> (7 - (x & 7))) & 1u;
      TEST_ASSERT_EQUAL_UINT8(want, got);
    }
  }
  // Unused low bits of the last byte stay zero (21 modules in 3 bytes).
  for (int y = 0; y < qr::kSize; ++y) {
    qr::rowBytes(m, y, row);
    TEST_ASSERT_EQUAL_UINT8(0, row[2] & 0x07);
  }
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the QR tests are declared extern there.
