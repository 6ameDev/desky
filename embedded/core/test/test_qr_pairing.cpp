// Host unit tests for the QR pairing bitmap (no hardware).
// Run: pio test -e native-test
//
// Covers Phase 0: fixed V1 21x21 geometry, finder patterns at the three
// corners, timing patterns, deterministic re-encode equality, and input
// validation (null / non-digit / short NUL-padded all reject, mirroring
// pairing::openWindow). Runner lives in test_udp_codec.cpp (single main).

#include <string.h>
#include <unity.h>

#include "../src/abilities/pairing/pairing_codec.h"
#include "link/link_codec.h"

// Compile-time wire-length pin (P1b): the QR encoder's packed length and
// the frozen link codec's QRBITMAP length are one contract. The firmware
// spells the codec via the linkcode:: alias (linkwrap__ in link_server.h,
// POSIX ::link collision); host tests use the canonical link:: spelling.
static_assert(qr::kPackedLen == static_cast<int>(link::kQrBitmapLen),
              "qr::kPackedLen must equal link::kQrBitmapLen (63B QRBITMAP)");

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

void test_qr_packed_matches_golden_rows() {
  // Wire contract: packBitmap("123456") must equal the Phase-0 golden rows
  // (qr_pair_check.py GOLDEN_123456 — independently cross-decoded SCAN-OK).
  // Regression for the 2026-10-18 head render bug: staging the first 63
  // UNPACKED module bytes as the 63B QRBITMAP lit only columns 7 and 15
  // on the OLED (two dotted bars, unscannable) instead of this payload.
  static const uint8_t kGoldenRows[qr::kPackedLen] = {
      0xFE, 0x5B, 0xF8, 0x82, 0x72, 0x08, 0xBA, 0xDA, 0xE8, 0xBA, 0x52, 0xE8, 0xBA, 0x2A, 0xE8, 0x82,
      0x0A, 0x08, 0xFE, 0xAB, 0xF8, 0x00, 0xD8, 0x00, 0xEF, 0xF6, 0x20, 0xA0, 0xC2, 0x08, 0x22, 0xA8,
      0xC0, 0x7D, 0xE2, 0x48, 0xBA, 0x4A, 0x98, 0x00, 0xB5, 0x50, 0xFE, 0xF7, 0x20, 0x82, 0xBD, 0xC0,
      0xBA, 0xB7, 0x28, 0xBA, 0x02, 0x30, 0xBA, 0xE8, 0x88, 0x82, 0xA2, 0x20, 0xFE, 0xCA, 0xA8};
  uint8_t m[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, m));
  uint8_t packed[qr::kPackedLen];
  qr::packBitmap(m, packed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kGoldenRows, packed, qr::kPackedLen);
  // Null-safe no-ops (fail-closed, never crashes the call site).
  for (size_t i = 0; i < qr::kPackedLen; ++i) {
    packed[i] = 0xA5;
  }
  qr::packBitmap(nullptr, packed);
  qr::packBitmap(m, nullptr);
  for (size_t i = 0; i < qr::kPackedLen; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0xA5, packed[i]);
  }
}

void test_qr_pack_equals_rowbytes_concat() {
  // P1a: packBitmap == 21x rowBytes-concatenation + bit round-trip via
  // getModule + pad bits zero, for 3+ codes.
  static const uint8_t kCodes[3][6] = {
      {'1', '2', '3', '4', '5', '6'}, {'6', '5', '4', '3', '2', '1'}, {'0', '0', '0', '0', '0', '0'}};
  for (int c = 0; c < 3; ++c) {
    uint8_t m[qr::kModules];
    TEST_ASSERT_TRUE(qr::encode6Digits(kCodes[c], m));
    uint8_t packed[qr::kPackedLen];
    qr::packBitmap(m, packed);
    // pack == rowBytes concatenation, row by row.
    for (int y = 0; y < qr::kSize; ++y) {
      uint8_t row[qr::kRowBytes];
      qr::rowBytes(m, y, row);
      for (int i = 0; i < qr::kRowBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(row[i], packed[static_cast<size_t>(y) * qr::kRowBytes + i]);
      }
      // Pad bits zero (21 modules in 3 bytes leave 3 unused low bits).
      TEST_ASSERT_EQUAL_UINT8(0, row[2] & 0x07);
    }
    // Bit round-trip: packed bit == getModule over all 441 modules.
    for (int y = 0; y < qr::kSize; ++y) {
      for (int x = 0; x < qr::kSize; ++x) {
        const uint8_t want = qr::getModule(m, x, y);
        const uint8_t got =
            (packed[static_cast<size_t>(y) * qr::kRowBytes + static_cast<size_t>(x >> 3)] >> (7 - (x & 7))) & 1u;
        TEST_ASSERT_EQUAL_UINT8(want, got);
      }
    }
  }
}

void test_qr_rs_syndrome_zero() {
  // WS4 Reed-Solomon EC check.
  //
  // NOTE: no trustworthy external EC codeword vector could be established
  // in this offline environment (no Nayuki/Thonky RS worked-example bytes
  // on hand beyond the algorithm provenance already cited in pairing_codec.h),
  // so this asserts the syndrome-zero property instead: recomputing the EC
  // remainder over data+EC yields zero. Algorithm provenance (generator
  // poly x^10+x^8+x^5+x^4+x^2+x+1 per ISO 18004 §7, Russian-peasant GF
  // multiply): Nayuki's QR generator (MIT) via Thonky's tutorial figures,
  // as cited in pairing_codec.h.
  static const uint8_t kCode[6] = {'1', '2', '3', '4', '5', '6'};
  // Rebuild the 19 data codewords (numeric mode, same layout as the
  // encoder: mode 0001, 10-bit count, 2x10-bit groups, terminator, pad).
  uint8_t data[qr::kDataCodewords];
  for (int i = 0; i < qr::kDataCodewords; ++i) {
    data[i] = 0;
  }
  int bitLen = 0;
  auto appendBits = [&](uint16_t val, int n) {
    for (int i = n - 1; i >= 0; --i) {
      if (((val >> i) & 1u) != 0) {
        data[bitLen >> 3] |= static_cast<uint8_t>(1u << (7 - (bitLen & 7)));
      }
      ++bitLen;
    }
  };
  appendBits(0x1, 4);
  appendBits(6, 10);
  for (int g = 0; g < 2; ++g) {
    const int d0 = kCode[g * 3] - '0';
    const int d1 = kCode[g * 3 + 1] - '0';
    const int d2 = kCode[g * 3 + 2] - '0';
    appendBits(static_cast<uint16_t>(d0 * 100 + d1 * 10 + d2), 10);
  }
  const int capacityBits = qr::kDataCodewords * 8;
  int term = capacityBits - bitLen;
  if (term > 4) {
    term = 4;
  }
  appendBits(0, term);
  appendBits(0, -bitLen & 7);
  const int used = bitLen >> 3;
  for (int i = used; i < qr::kDataCodewords; ++i) {
    data[i] = ((i - used) % 2 == 0) ? 0xEC : 0x11;
  }
  // Generator poly (degree 7, sans leading term) + remainder loop: same
  // construction as the encoder, then the syndrome pass over data+EC.
  uint8_t divisor[qr::kEcCodewords];
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    divisor[i] = 0;
  }
  divisor[qr::kEcCodewords - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    for (int j = 0; j < qr::kEcCodewords; ++j) {
      divisor[j] = qr::detail::rsMul(divisor[j], root);
      if (j + 1 < qr::kEcCodewords) {
        divisor[j] ^= divisor[j + 1];
      }
    }
    root = qr::detail::rsMul(root, 0x02);
  }
  uint8_t ec[qr::kEcCodewords];
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    ec[i] = 0;
  }
  for (int i = 0; i < qr::kDataCodewords; ++i) {
    const uint8_t factor = static_cast<uint8_t>(data[i] ^ ec[0]);
    for (int j = 0; j < qr::kEcCodewords - 1; ++j) {
      ec[j] = ec[j + 1];
    }
    ec[qr::kEcCodewords - 1] = 0;
    for (int j = 0; j < qr::kEcCodewords; ++j) {
      ec[j] ^= qr::detail::rsMul(divisor[j], factor);
    }
  }
  // Syndrome: remainder over the full 26-codeword message must be zero.
  uint8_t syn[qr::kEcCodewords];
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    syn[i] = 0;
  }
  for (int i = 0; i < qr::kDataCodewords; ++i) {
    const uint8_t factor = static_cast<uint8_t>(data[i] ^ syn[0]);
    for (int j = 0; j < qr::kEcCodewords - 1; ++j) {
      syn[j] = syn[j + 1];
    }
    syn[qr::kEcCodewords - 1] = 0;
    for (int j = 0; j < qr::kEcCodewords; ++j) {
      syn[j] ^= qr::detail::rsMul(divisor[j], factor);
    }
  }
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    const uint8_t factor = static_cast<uint8_t>(ec[i] ^ syn[0]);
    for (int j = 0; j < qr::kEcCodewords - 1; ++j) {
      syn[j] = syn[j + 1];
    }
    syn[qr::kEcCodewords - 1] = 0;
    for (int j = 0; j < qr::kEcCodewords; ++j) {
      syn[j] ^= qr::detail::rsMul(divisor[j], factor);
    }
  }
  for (int i = 0; i < qr::kEcCodewords; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0, syn[i]);
  }
}

void test_qr_boundary_goldens_self_consistent() {
  // WS4 boundary goldens for "000000" and "999999": self-consistency is
  // REQUIRED (pack == concat rows, finder/timing intact, differs from the
  // "123456" golden). Independent cross-decode via
  // embedded/scripts/qr_pair_check.py --golden is best-effort only: the
  // script carries just the 123456 golden rows (verified SCAN-OK here), so
  // no script vector exists for these boundary codes.
  static const uint8_t kZero[6] = {'0', '0', '0', '0', '0', '0'};
  static const uint8_t kNine[6] = {'9', '9', '9', '9', '9', '9'};
  uint8_t mz[qr::kModules];
  uint8_t mn[qr::kModules];
  uint8_t mg[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(kZero, mz));
  TEST_ASSERT_TRUE(qr::encode6Digits(kNine, mn));
  TEST_ASSERT_TRUE(qr::encode6Digits(kGolden, mg));
  // Finder + timing intact on both boundary codes.
  TEST_ASSERT_TRUE(finderOk(mz, 0, 0));
  TEST_ASSERT_TRUE(finderOk(mz, qr::kSize - 7, 0));
  TEST_ASSERT_TRUE(finderOk(mz, 0, qr::kSize - 7));
  TEST_ASSERT_TRUE(finderOk(mn, 0, 0));
  TEST_ASSERT_TRUE(finderOk(mn, qr::kSize - 7, 0));
  TEST_ASSERT_TRUE(finderOk(mn, 0, qr::kSize - 7));
  for (int i = 8; i <= 12; ++i) {
    const uint8_t want = (i % 2 == 0) ? 1 : 0;
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(mz, i, 6));
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(mz, 6, i));
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(mn, i, 6));
    TEST_ASSERT_EQUAL_UINT8(want, qr::getModule(mn, 6, i));
  }
  // pack == rowBytes concatenation on both.
  uint8_t pz[qr::kPackedLen];
  uint8_t pn[qr::kPackedLen];
  qr::packBitmap(mz, pz);
  qr::packBitmap(mn, pn);
  for (int y = 0; y < qr::kSize; ++y) {
    uint8_t rz[qr::kRowBytes];
    uint8_t rn[qr::kRowBytes];
    qr::rowBytes(mz, y, rz);
    qr::rowBytes(mn, y, rn);
    for (int i = 0; i < qr::kRowBytes; ++i) {
      TEST_ASSERT_EQUAL_UINT8(rz[i], pz[static_cast<size_t>(y) * qr::kRowBytes + i]);
      TEST_ASSERT_EQUAL_UINT8(rn[i], pn[static_cast<size_t>(y) * qr::kRowBytes + i]);
    }
  }
  // Both differ from the "123456" golden packed payload.
  uint8_t pg[qr::kPackedLen];
  qr::packBitmap(mg, pg);
  TEST_ASSERT_FALSE(memcmp(pz, pg, qr::kPackedLen) == 0);
  TEST_ASSERT_FALSE(memcmp(pn, pg, qr::kPackedLen) == 0);
  TEST_ASSERT_FALSE(memcmp(pz, pn, qr::kPackedLen) == 0);
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the QR tests are declared extern there.
