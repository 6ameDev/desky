// Host unit tests for the UART POC FEC core (no hardware).
// Run: pio test -e native-test
//
// Mirrored core<->head (identical content): systematic Vandermonde erasure
// code over GF(2^8), poly 0x11D. Default K=2 is a call-site concern — here K
// ranges 0..4 to pin the core's capability.

#include <string.h>
#include <unity.h>

#include "link/uart_fec.h"

namespace {

uint32_t g_fec_rng = 0x12345678UL;

uint32_t nextFecRand() {
  g_fec_rng ^= g_fec_rng << 13;
  g_fec_rng ^= g_fec_rng >> 17;
  g_fec_rng ^= g_fec_rng << 5;
  return g_fec_rng;
}

void fillRand(uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    p[i] = static_cast<uint8_t>(nextFecRand() & 0xFFU);
  }
}

}  // namespace

void test_fec_k0_passthrough() {
  // encode() with K=0 is a no-op reject; recover() passes data through.
  constexpr uint8_t kN = 3;
  constexpr size_t kS = 16;
  uint8_t data[kN][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2]};
  size_t lens[kN] = {kS, kS, kS};
  uint8_t dummy[1] = {0};
  uint8_t* pptr[1] = {dummy};
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, kN, 0, kS, lens, pptr));

  uint8_t* ioptr[kN] = {data[0], data[1], data[2]};
  const bool none[kN] = {false, false, false};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, none, kN, nullptr, nullptr, 0, kS, 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  const bool one[kN] = {false, true, false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, one, kN, nullptr, nullptr, 0, kS, 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
}

void test_fec_single_erasure_random() {
  constexpr uint8_t kN = 5;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 48;
  uint8_t data[kN][kS];
  uint8_t orig[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
    memcpy(orig[i], data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3], data[4]};
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kS, kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  const uint8_t lost = static_cast<uint8_t>(nextFecRand() % kN);
  memset(data[lost], 0, kS);  // Simulate loss; recover must rewrite the block.
  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3], data[4]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  bool missing[kN] = {};
  missing[lost] = true;
  const bool pok[kK] = {true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  for (uint8_t i = 0; i < kN; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[i], data[i], kS);
  }
}

void test_fec_burst_adjacent_erasures() {
  constexpr uint8_t kN = 6;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 48;
  uint8_t data[kN][kS];
  uint8_t orig[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
    memcpy(orig[i], data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3], data[4], data[5]};
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kS, kS, kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  const uint8_t start = static_cast<uint8_t>(nextFecRand() % (kN - 1));
  memset(data[start], 0, kS);
  memset(data[start + 1], 0, kS);
  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3], data[4], data[5]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  bool missing[kN] = {};
  missing[start] = true;
  missing[static_cast<uint8_t>(start + 1)] = true;
  const bool pok[kK] = {true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  for (uint8_t i = 0; i < kN; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[i], data[i], kS);
  }
}

void test_fec_last_chunk_missing_truncation() {
  constexpr uint8_t kN = 4;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 48;
  constexpr size_t kLast = 17;
  uint8_t data[kN][kS];
  uint8_t orig[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
    memcpy(orig[i], data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3]};
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kLast};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  memset(data[kN - 1], 0, kS);  // LAST chunk lost.
  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  const bool missing[kN] = {false, false, false, true};
  const bool pok[kK] = {true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, kN, pcptr, pok, kK, kS, kLast, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  // Caller truncates block N-1 to last_len: only the prefix must match.
  for (uint8_t i = 0; i < kN - 1; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[i], data[i], kS);
  }
  TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[kN - 1], data[kN - 1], kLast);
  // Padding region solves to the encode-time zeros.
  TEST_ASSERT_EQUAL_UINT8(0, data[kN - 1][kLast]);
  TEST_ASSERT_EQUAL_UINT8(0, data[kN - 1][kS - 1]);
}

void test_fec_kplus1_unrecoverable() {
  constexpr uint8_t kN = 5;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 32;
  uint8_t data[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3], data[4]};
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kS, kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3], data[4]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  const bool missing[kN] = {true, false, true, false, true};  // 3 losses > K=2.
  const bool pok[kK] = {true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
}

void test_fec_allzero_allff() {
  constexpr uint8_t kN = 4;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 32;
  uint8_t data[kN][kS];
  uint8_t par[kK][kS];
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kS};
  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};

  memset(data, 0, sizeof(data));
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3]};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));
  const bool missA[kN] = {false, true, false, true};
  const bool pok[kK] = {true, true};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missA, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  for (uint8_t i = 0; i < kN; ++i) {
    for (size_t b = 0; b < kS; ++b) {
      TEST_ASSERT_EQUAL_UINT8(0, data[i][b]);
    }
  }

  memset(data, 0xFF, sizeof(data));
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));
  const bool missB[kN] = {true, false, true, false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missB, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  for (uint8_t i = 0; i < kN; ++i) {
    for (size_t b = 0; b < kS; ++b) {
      TEST_ASSERT_EQUAL_UINT8(0xFF, data[i][b]);
    }
  }
}

void test_fec_stride_padding_agreement() {
  // Bytes past lens[N-1] must not affect parity (encode zero-pads internally).
  constexpr uint8_t kN = 3;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 48;
  constexpr size_t kLast = 17;
  uint8_t data[kN][kS];
  uint8_t parA[kK][kS];
  uint8_t parB[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2]};
  uint8_t* paPtr[kK] = {parA[0], parA[1]};
  uint8_t* pbPtr[kK] = {parB[0], parB[1]};
  size_t lens[kN] = {kS, kS, kLast};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, paPtr));

  memset(data[kN - 1] + kLast, 0, kS - kLast);
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pbPtr));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(parA[0], parB[0], kS);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(parA[1], parB[1], kS);

  memset(data[kN - 1] + kLast, 0xAA, kS - kLast);  // Garbage tail still ignored.
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pbPtr));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(parA[0], parB[0], kS);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(parA[1], parB[1], kS);
}

void test_fec_corrupted_parity() {
  constexpr uint8_t kN = 4;
  constexpr uint8_t kK = 2;
  constexpr size_t kS = 48;
  uint8_t data[kN][kS];
  uint8_t orig[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
    memcpy(orig[i], data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3]};
  uint8_t* pptr[kK] = {par[0], par[1]};
  size_t lens[kN] = {kS, kS, kS, kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  uint8_t* ioptr[kN] = {data[0], data[1], data[2], data[3]};
  const uint8_t* pcptr[kK] = {par[0], par[1]};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  const bool pok[kK] = {true, true};

  // Needed parity corrupted -> UNRECOVERABLE (never corrupt output).
  for (size_t b = 0; b < kS; ++b) {
    par[1][b] = static_cast<uint8_t>(par[1][b] ^ 0xFF);
  }
  const bool miss[kN] = {false, true, false, false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);

  // Corrupted parity unneeded (nothing lost) -> OK.
  memcpy(data[1], orig[1], kS);
  const bool none[kN] = {false, false, false, false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, none, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);

  // Corrupted parity excluded via parity_ok -> OK on the good row.
  memset(data[1], 0, kS);
  const bool oneGood[kK] = {true, false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, kN, pcptr, oneGood, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[1], data[1], kS);
}

void test_fec_determinism_idempotence() {
  constexpr uint8_t kN = 5;
  constexpr uint8_t kK = 3;
  constexpr size_t kS = 64;
  uint8_t data[kN][kS];
  uint8_t parA[kK][kS];
  uint8_t parB[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
  }
  const uint8_t* dptr[kN] = {data[0], data[1], data[2], data[3], data[4]};
  uint8_t* paPtr[kK] = {parA[0], parA[1], parA[2]};
  uint8_t* pbPtr[kK] = {parB[0], parB[1], parB[2]};
  size_t lens[kN] = {kS, kS, kS, kS, kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, paPtr));
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pbPtr));
  for (uint8_t j = 0; j < kK; ++j) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(parA[j], parB[j], kS);
  }
}

void test_fec_rejects_bad_args() {
  uint8_t blk[16] = {};
  uint8_t par[16] = {};
  const uint8_t* dptr[1] = {blk};
  uint8_t* pptr[1] = {par};
  const uint8_t* pcptr[1] = {par};
  size_t lens[1] = {sizeof(blk)};
  size_t bigLens[1] = {sizeof(blk) + 1};
  uint8_t* ioptr[1] = {blk};
  const bool noMiss[1] = {false};
  const bool miss[1] = {true};
  const bool pok[1] = {true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  uint8_t tiny[4] = {};

  TEST_ASSERT_FALSE(uartpoc::fec::encode(nullptr, 1, 1, sizeof(blk), lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, sizeof(blk), nullptr, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, sizeof(blk), lens, nullptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 0, 1, sizeof(blk), lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 65, 1, sizeof(blk), lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 0, sizeof(blk), lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 5, sizeof(blk), lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, 0, lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, 1025, lens, pptr));
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, sizeof(blk), bigLens, pptr));
  const uint8_t* nullData[1] = {nullptr};
  TEST_ASSERT_FALSE(uartpoc::fec::encode(nullData, 1, 1, sizeof(blk), lens, pptr));
  uint8_t* nullPar[1] = {nullptr};
  TEST_ASSERT_FALSE(uartpoc::fec::encode(dptr, 1, 1, sizeof(blk), lens, nullPar));

  TEST_ASSERT_TRUE(uartpoc::fec::recover(nullptr, noMiss, 1, pcptr, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, nullptr, 1, pcptr, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, noMiss, 0, pcptr, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, noMiss, 1, pcptr, pok, 5, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, noMiss, 1, pcptr, pok, 1, 0, 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, noMiss, 1, pcptr, pok, 1, 1025, 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, noMiss, 1, pcptr, pok, 1, sizeof(blk), sizeof(blk) + 1, scratch,
                                         sizeof(scratch)) == uartpoc::fec::Recover::UNRECOVERABLE);
  uint8_t* nullIo[1] = {nullptr};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(nullIo, noMiss, 1, pcptr, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, nullptr, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, pcptr, nullptr, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  const uint8_t* nullPc[1] = {nullptr};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, nullPc, pok, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, pcptr, pok, 1, sizeof(blk), 0, nullptr, 0) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, pcptr, pok, 1, sizeof(blk), 0, tiny, sizeof(tiny)) ==
                   uartpoc::fec::Recover::UNRECOVERABLE);
  // Losses within K but short on good parity -> NEED_MORE (retryable).
  const bool noneOk[1] = {false};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, pcptr, noneOk, 1, sizeof(blk), 0, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::NEED_MORE);
}

void test_fec_edge_n1_k1() {
  constexpr size_t kS = 32;
  uint8_t data[1][kS];
  uint8_t par[1][kS];
  fillRand(data[0], kS);
  uint8_t keep[kS] = {};
  memcpy(keep, data[0], kS);
  const uint8_t* dptr[1] = {data[0]};
  uint8_t* pptr[1] = {par[0]};
  size_t lens[1] = {kS};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, 1, 1, kS, lens, pptr));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data[0], par[0], kS);  // V=[1]: parity is the block.

  memset(data[0], 0, kS);
  uint8_t* ioptr[1] = {data[0]};
  const uint8_t* pcptr[1] = {par[0]};
  const bool miss[1] = {true};
  const bool pok[1] = {true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, miss, 1, pcptr, pok, 1, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(keep, data[0], kS);
}

void test_fec_max_n64_k4() {
  constexpr uint8_t kN = 64;
  constexpr uint8_t kK = 4;
  constexpr size_t kS = 16;
  uint8_t data[kN][kS];
  uint8_t orig[kN][kS];
  uint8_t par[kK][kS];
  for (uint8_t i = 0; i < kN; ++i) {
    fillRand(data[i], kS);
    memcpy(orig[i], data[i], kS);
  }
  const uint8_t* dptr[kN];
  uint8_t* ioptr[kN];
  size_t lens[kN];
  bool missing[kN];
  for (uint8_t i = 0; i < kN; ++i) {
    dptr[i] = data[i];
    ioptr[i] = data[i];
    lens[i] = kS;
    missing[i] = false;
  }
  uint8_t* pptr[kK] = {par[0], par[1], par[2], par[3]};
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, kN, kK, kS, lens, pptr));

  const uint8_t lost[kK] = {0, 1, 32, 63};  // Adjacent pair + spread.
  for (uint8_t l = 0; l < kK; ++l) {
    memset(data[lost[l]], 0, kS);
    missing[lost[l]] = true;
  }
  const uint8_t* pcptr[kK] = {par[0], par[1], par[2], par[3]};
  const bool pok[kK] = {true, true, true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, kN, pcptr, pok, kK, kS, kS, scratch, sizeof(scratch)) ==
                   uartpoc::fec::Recover::OK);
  for (uint8_t i = 0; i < kN; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(orig[i], data[i], kS);
  }
}
