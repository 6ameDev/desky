// Host unit tests for the shared link crypto contract (AES-128-GCM seal/open
// + deterministic AP passphrase derivation, no hardware).
// Run: pio test -e native-test
//
// Golden vectors pin exact bytes so core and head can never diverge silently:
//   GCM zero key/zero nonce, empty plaintext -> tag 58e2fcce...455a
//   GCM zero key/zero nonce, 16 zero bytes -> ct 0388dace...fe78, tag ab6e47d4...bddf
//   seal(key 00..0f, counter 7, "hello link") -> the 26 sealed bytes below
//   passphrase(secret 00..0f) -> desky-pfub5ysryc
//   passphrase(secret ff..00 step pattern) -> desky-tjdlabuih7
// All key material here is short and obviously synthetic; tests print none of it.

#include <string.h>
#include <unity.h>

#include "link/link_crypto.h"

namespace {

const uint8_t kKeyA[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                           0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
const uint8_t kKeyB[16] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
                           0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
const uint8_t kZeroKey[16] = {};
const uint8_t kZeroNonce[12] = {};

const uint8_t kZeroEmptyTag[16] = {0x58, 0xE2, 0xFC, 0xCE, 0xFA, 0x7E, 0x30, 0x61,
                                   0x36, 0x7F, 0x1D, 0x57, 0xA4, 0xE7, 0x45, 0x5A};
const uint8_t kZero16Ct[16] = {0x03, 0x88, 0xDA, 0xCE, 0x60, 0xB6, 0xA3, 0x92,
                               0xF3, 0x28, 0xC2, 0xB9, 0x71, 0xB2, 0xFE, 0x78};
const uint8_t kZero16Tag[16] = {0xAB, 0x6E, 0x47, 0xD4, 0x2C, 0xEC, 0x13, 0xBD,
                                0xF5, 0x3A, 0x67, 0xB2, 0x12, 0x57, 0xBD, 0xDF};
const uint8_t kHelloSealed[26] = {0x21, 0xF6, 0x45, 0x9E, 0x0B, 0xD4, 0xD1, 0xA4, 0x0B, 0x61, 0x82, 0xBD, 0x6B,
                                  0x48, 0x71, 0x64, 0x96, 0x7C, 0x0F, 0xA7, 0xC9, 0xE7, 0xD7, 0xB7, 0xA3, 0x09};

bool nonceFor(uint32_t counter, uint8_t out[12]) { return linkcrypto::makeCounterNonce(counter, out); }

}  // namespace

void test_linkcrypto_provision_exact_length_only() {
  linkcrypto::Key good = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  TEST_ASSERT_TRUE(good.ok);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, good.bytes, sizeof(kKeyA));
  linkcrypto::Key empty = linkcrypto::provision(nullptr, 0);
  TEST_ASSERT_FALSE(empty.ok);
  linkcrypto::Key null16 = linkcrypto::provision(nullptr, 16);
  TEST_ASSERT_FALSE(null16.ok);
  const uint8_t short15[15] = {};
  TEST_ASSERT_FALSE(linkcrypto::provision(short15, sizeof(short15)).ok);
  const uint8_t long17[17] = {};
  TEST_ASSERT_FALSE(linkcrypto::provision(long17, sizeof(long17)).ok);
  const uint8_t one[1] = {0x42};
  TEST_ASSERT_FALSE(linkcrypto::provision(one, sizeof(one)).ok);
}

void test_linkcrypto_key_constants() {
  TEST_ASSERT_EQUAL_size_t(16, linkcrypto::kKeyLen);
  TEST_ASSERT_EQUAL_size_t(12, linkcrypto::kNonceLen);
  TEST_ASSERT_EQUAL_size_t(16, linkcrypto::kTagLen);
  TEST_ASSERT_EQUAL_size_t(32, linkcrypto::kSecretHexLen);
  TEST_ASSERT_EQUAL_size_t(16, linkcrypto::kPassphraseLen);
  TEST_ASSERT_EQUAL_size_t(17, linkcrypto::kPassphraseCap);
  TEST_ASSERT_TRUE(linkcrypto::kPassphraseLen >= 8);
  TEST_ASSERT_EQUAL_STRING("desky-link-ap-v1", linkcrypto::kApLabel);
}

void test_linkcrypto_counter_nonce_layout() {
  uint8_t n[12] = {};
  TEST_ASSERT_TRUE(nonceFor(7, n));
  const uint8_t want[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, n, sizeof(n));
  TEST_ASSERT_TRUE(nonceFor(0x01020304, n));
  const uint8_t want2[12] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want2, n, sizeof(n));
  TEST_ASSERT_FALSE(linkcrypto::makeCounterNonce(1, nullptr));
}

void test_linkcrypto_gcm_zero_empty_vector() {
  linkcrypto::Key k = linkcrypto::provision(kZeroKey, sizeof(kZeroKey));
  uint8_t sealed[16] = {};
  TEST_ASSERT_EQUAL_size_t(16, linkcrypto::seal(k, kZeroNonce, nullptr, 0, sealed, sizeof(sealed)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kZeroEmptyTag, sealed, sizeof(sealed));
  uint8_t plain[1] = {0xAA};
  size_t got = 9;
  TEST_ASSERT_TRUE(linkcrypto::open(k, kZeroNonce, sealed, sizeof(sealed), plain, sizeof(plain), got));
  TEST_ASSERT_EQUAL_size_t(0, got);
}

void test_linkcrypto_gcm_zero_16_vector() {
  linkcrypto::Key k = linkcrypto::provision(kZeroKey, sizeof(kZeroKey));
  const uint8_t zeros[16] = {};
  uint8_t sealed[32] = {};
  TEST_ASSERT_EQUAL_size_t(32, linkcrypto::seal(k, kZeroNonce, zeros, sizeof(zeros), sealed, sizeof(sealed)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kZero16Ct, sealed, 16);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kZero16Tag, sealed + 16, 16);
  uint8_t plain[16] = {};
  size_t got = 0;
  TEST_ASSERT_TRUE(linkcrypto::open(k, kZeroNonce, sealed, sizeof(sealed), plain, sizeof(plain), got));
  TEST_ASSERT_EQUAL_size_t(16, got);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, plain, sizeof(plain));
}

void test_linkcrypto_seal_deterministic_golden() {
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(7, nonce));
  const char* msg = "hello link";
  uint8_t sealed[26] = {};
  TEST_ASSERT_EQUAL_size_t(sizeof(kHelloSealed), linkcrypto::seal(k, nonce, reinterpret_cast<const uint8_t*>(msg), 10,
                                                                  sealed, sizeof(sealed)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kHelloSealed, sealed, sizeof(sealed));
  uint8_t plain[10] = {};
  size_t got = 0;
  TEST_ASSERT_TRUE(linkcrypto::open(k, nonce, sealed, sizeof(sealed), plain, sizeof(plain), got));
  TEST_ASSERT_EQUAL_size_t(10, got);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, plain, 10);
}

void test_linkcrypto_round_trip_sizes() {
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  static const size_t kLens[] = {0, 1, 15, 16, 17, 64};
  for (size_t c = 0; c < sizeof(kLens) / sizeof(kLens[0]); ++c) {
    uint8_t pt[64];
    for (size_t i = 0; i < kLens[c]; ++i) {
      pt[i] = static_cast<uint8_t>((c * 31u + i * 7u) & 0xFFu);
    }
    uint8_t nonce[12];
    TEST_ASSERT_TRUE(nonceFor(static_cast<uint32_t>(100 + c), nonce));
    uint8_t sealed[64 + 16] = {};
    const size_t n = linkcrypto::seal(k, nonce, kLens[c] == 0 ? nullptr : pt, kLens[c], sealed, sizeof(sealed));
    TEST_ASSERT_EQUAL_size_t(kLens[c] + 16, n);
    uint8_t back[64] = {};
    size_t got = 0;
    TEST_ASSERT_TRUE(linkcrypto::open(k, nonce, sealed, n, back, sizeof(back), got));
    TEST_ASSERT_EQUAL_size_t(kLens[c], got);
    if (kLens[c] > 0) {
      TEST_ASSERT_EQUAL_UINT8_ARRAY(pt, back, kLens[c]);
    }
  }
}

void test_linkcrypto_tamper_rejected() {
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(7, nonce));
  uint8_t sealed[sizeof(kHelloSealed)];
  for (size_t i = 0; i < sizeof(sealed); ++i) {
    sealed[i] = kHelloSealed[i];
  }
  // Flipped ciphertext byte fails.
  sealed[3] ^= 0x01;
  uint8_t plain[10] = {};
  size_t got = 0;
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, sealed, sizeof(sealed), plain, sizeof(plain), got));
  TEST_ASSERT_EQUAL_size_t(0, got);
  // Flipped tag byte fails.
  for (size_t i = 0; i < sizeof(sealed); ++i) {
    sealed[i] = kHelloSealed[i];
  }
  sealed[sizeof(sealed) - 1] ^= 0x80;
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, sealed, sizeof(sealed), plain, sizeof(plain), got));
}

void test_linkcrypto_wrong_key_and_nonce_rejected() {
  linkcrypto::Key ka = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  linkcrypto::Key kb = linkcrypto::provision(kKeyB, sizeof(kKeyB));
  uint8_t n7[12];
  uint8_t n8[12];
  TEST_ASSERT_TRUE(nonceFor(7, n7));
  TEST_ASSERT_TRUE(nonceFor(8, n8));
  uint8_t sealed[sizeof(kHelloSealed)];
  for (size_t i = 0; i < sizeof(sealed); ++i) {
    sealed[i] = kHelloSealed[i];
  }
  uint8_t plain[10] = {};
  size_t got = 0;
  TEST_ASSERT_FALSE(linkcrypto::open(kb, n7, sealed, sizeof(sealed), plain, sizeof(plain), got));
  TEST_ASSERT_FALSE(linkcrypto::open(ka, n8, sealed, sizeof(sealed), plain, sizeof(plain), got));
}

void test_linkcrypto_truncated_and_short_rejected() {
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(7, nonce));
  uint8_t plain[10] = {};
  size_t got = 0;
  uint8_t tiny[15] = {};
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, tiny, sizeof(tiny), plain, sizeof(plain), got));
  uint8_t cut[sizeof(kHelloSealed) - 1];
  for (size_t i = 0; i < sizeof(cut); ++i) {
    cut[i] = kHelloSealed[i];
  }
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, cut, sizeof(cut), plain, sizeof(plain), got));
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, kHelloSealed, sizeof(kHelloSealed), plain, 9, got));
}

void test_linkcrypto_nonce_reuse_is_visible() {
  // Same (key, nonce, plaintext) seals identically every time: determinism the
  // caller can see. Rotation and persistence are the caller's job; nothing
  // here refuses a reused counter.
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(7, nonce));
  const uint8_t pt[4] = {1, 2, 3, 4};
  uint8_t first[20] = {};
  uint8_t second[20] = {};
  TEST_ASSERT_EQUAL_size_t(sizeof(first), linkcrypto::seal(k, nonce, pt, sizeof(pt), first, sizeof(first)));
  TEST_ASSERT_EQUAL_size_t(sizeof(second), linkcrypto::seal(k, nonce, pt, sizeof(pt), second, sizeof(second)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first, second, sizeof(first));
  uint8_t other[12];
  TEST_ASSERT_TRUE(nonceFor(8, other));
  uint8_t third[20] = {};
  TEST_ASSERT_EQUAL_size_t(sizeof(third), linkcrypto::seal(k, other, pt, sizeof(pt), third, sizeof(third)));
  TEST_ASSERT_TRUE(memcmp(first, third, sizeof(first)) != 0);
}

void test_linkcrypto_null_and_cap_safety() {
  linkcrypto::Key k = linkcrypto::provision(kKeyA, sizeof(kKeyA));
  linkcrypto::Key bad;
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(1, nonce));
  const uint8_t pt[2] = {9, 9};
  uint8_t out[32] = {};
  size_t got = 0;
  TEST_ASSERT_EQUAL_size_t(0, linkcrypto::seal(bad, nonce, pt, sizeof(pt), out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, linkcrypto::seal(k, nullptr, pt, sizeof(pt), out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, linkcrypto::seal(k, nonce, nullptr, 2, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, linkcrypto::seal(k, nonce, pt, sizeof(pt), nullptr, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, linkcrypto::seal(k, nonce, pt, sizeof(pt), out, sizeof(pt)));
  TEST_ASSERT_FALSE(linkcrypto::open(bad, nonce, kHelloSealed, sizeof(kHelloSealed), out, sizeof(out), got));
  TEST_ASSERT_FALSE(linkcrypto::open(k, nullptr, kHelloSealed, sizeof(kHelloSealed), out, sizeof(out), got));
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, nullptr, 20, out, sizeof(out), got));
  TEST_ASSERT_FALSE(linkcrypto::open(k, nonce, kHelloSealed, sizeof(kHelloSealed), nullptr, sizeof(out), got));
  TEST_ASSERT_FALSE(linkcrypto::tagEqual(nullptr, kZero16Tag, 16));
  TEST_ASSERT_FALSE(linkcrypto::tagEqual(kZero16Tag, nullptr, 16));
  TEST_ASSERT_TRUE(linkcrypto::tagEqual(kZero16Tag, kZero16Tag, 16));
  TEST_ASSERT_FALSE(linkcrypto::tagEqual(kZero16Tag, kZeroEmptyTag, 16));
}

void test_linkcrypto_open_failure_zeroes_output() {
  linkcrypto::Key kb = linkcrypto::provision(kKeyB, sizeof(kKeyB));
  uint8_t nonce[12];
  TEST_ASSERT_TRUE(nonceFor(7, nonce));
  uint8_t plain[10];
  for (size_t i = 0; i < sizeof(plain); ++i) {
    plain[i] = 0xAA;
  }
  size_t got = 0;
  TEST_ASSERT_FALSE(linkcrypto::open(kb, nonce, kHelloSealed, sizeof(kHelloSealed), plain, sizeof(plain), got));
  const uint8_t zeros[10] = {};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, plain, sizeof(plain));
}

void test_linkcrypto_passphrase_goldens() {
  char pass[linkcrypto::kPassphraseCap] = {};
  TEST_ASSERT_TRUE(linkcrypto::deriveApPassphrase(kKeyA, sizeof(kKeyA), pass, sizeof(pass)));
  TEST_ASSERT_EQUAL_STRING("desky-pfub5ysryc", pass);
  TEST_ASSERT_TRUE(linkcrypto::deriveApPassphrase(kKeyB, sizeof(kKeyB), pass, sizeof(pass)));
  TEST_ASSERT_EQUAL_STRING("desky-tjdlabuih7", pass);
}

void test_linkcrypto_passphrase_shape() {
  char pass[linkcrypto::kPassphraseCap] = {};
  char again[linkcrypto::kPassphraseCap] = {};
  TEST_ASSERT_TRUE(linkcrypto::deriveApPassphrase(kKeyA, sizeof(kKeyA), pass, sizeof(pass)));
  TEST_ASSERT_EQUAL_size_t(linkcrypto::kPassphraseLen, strlen(pass));
  TEST_ASSERT_TRUE(strncmp(pass, "desky-", 6) == 0);
  for (size_t i = 6; pass[i] != '\0'; ++i) {
    TEST_ASSERT_TRUE(strchr(linkcrypto::kPassAlphabet, pass[i]) != nullptr);
  }
  TEST_ASSERT_TRUE(linkcrypto::deriveApPassphrase(kKeyA, sizeof(kKeyA), again, sizeof(again)));
  TEST_ASSERT_EQUAL_STRING(pass, again);
  TEST_ASSERT_TRUE(linkcrypto::deriveApPassphrase(kKeyB, sizeof(kKeyB), again, sizeof(again)));
  TEST_ASSERT_TRUE(strcmp(pass, again) != 0);
  char tiny[8] = {};
  TEST_ASSERT_FALSE(linkcrypto::deriveApPassphrase(kKeyA, sizeof(kKeyA), tiny, sizeof(tiny)));
  TEST_ASSERT_FALSE(linkcrypto::deriveApPassphrase(nullptr, 16, pass, sizeof(pass)));
  TEST_ASSERT_FALSE(linkcrypto::deriveApPassphrase(kKeyA, 0, pass, sizeof(pass)));
  TEST_ASSERT_FALSE(linkcrypto::deriveApPassphrase(kKeyA, sizeof(kKeyA), nullptr, sizeof(pass)));
}
