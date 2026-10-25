#pragma once
// Core<->head link crypto — shared, header-only, Arduino-free.
// Provision (key bytes in), seal (plaintext in, nonce + ciphertext + tag out),
// open (fail-closed authenticated decrypt). No WiFi/pairing concepts here.
// Backend: AES-128-GCM. Firmware toolchains use mbedTLS when present;
// every other toolchain uses the self-contained primitive below, which is
// byte-identical AES-128-GCM (pinned against published zero vectors in host
// tests), so both sides always agree.
// Nonces are explicit and caller-owned: reuse of one (key, nonce) pair voids
// GCM security. This header never enforces uniqueness.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if __has_include(<mbedtls/gcm.h>)
#include <mbedtls/gcm.h>
#define LINK_CRYPTO_HAVE_MBEDTLS_GCM 1
#else
#define LINK_CRYPTO_HAVE_MBEDTLS_GCM 0
#endif

namespace linkcrypto {

// 128-bit keys: 16 raw bytes, or 32 hex chars in the build environment.
constexpr size_t kKeyLen = 16;
constexpr size_t kSecretHexLen = 32;
// GCM fixed to the 12-byte nonce form; no arbitrary-length IV path.
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;

// AP passphrase: readable prefix plus hash chars, 16 total (WPA2 needs 8..63).
constexpr char kApLabel[] = "desky-link-ap-v1";
constexpr char kPassPrefix[] = "desky-";
constexpr size_t kPassHashChars = 10;
constexpr size_t kPassphraseLen = 6 + kPassHashChars;
constexpr size_t kPassphraseCap = kPassphraseLen + 1;
// 32 unambiguous lowercase + digits (5 bits per char).
constexpr char kPassAlphabet[] = "abcdefghijklmnopqrstuvwxyz234567";

struct Key {
  uint8_t bytes[kKeyLen] = {};
  bool ok = false;
};

// Exact-length only: short, empty, and long inputs are all refused.
inline Key provision(const uint8_t* secret, size_t len) {
  Key k;
  if (secret == nullptr || len != kKeyLen) {
    return k;
  }
  for (size_t i = 0; i < kKeyLen; ++i) {
    k.bytes[i] = secret[i];
  }
  k.ok = true;
  return k;
}

inline bool tagEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

// Counter (u32, big-endian) in the low 4 bytes, zeros above. Callers own the
// increment and any persistence across reboots.
inline bool makeCounterNonce(uint32_t counter, uint8_t out[kNonceLen]) {
  if (out == nullptr) {
    return false;
  }
  for (size_t i = 0; i < 8; ++i) {
    out[i] = 0;
  }
  out[8] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out[9] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out[10] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out[11] = static_cast<uint8_t>(counter & 0xFFu);
  return true;
}

#if !LINK_CRYPTO_HAVE_MBEDTLS_GCM
// ── Self-contained AES-128 + GCM (12-byte nonces only) ──────────────────────
namespace portable {

const uint8_t kSBox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80u) != 0 ? 0x1Bu : 0x00u)); }

inline void expandKey(const uint8_t key[kKeyLen], uint8_t rk[11][16]) {
  static const uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
  for (size_t i = 0; i < 16; ++i) {
    rk[0][i] = key[i];
  }
  for (int round = 1; round <= 10; ++round) {
    uint8_t* prev = rk[round - 1];
    uint8_t* cur = rk[round];
    const uint8_t t0 = kSBox[prev[13]] ^ kRcon[round - 1];
    const uint8_t t1 = kSBox[prev[14]];
    const uint8_t t2 = kSBox[prev[15]];
    const uint8_t t3 = kSBox[prev[12]];
    cur[0] = prev[0] ^ t0;
    cur[1] = prev[1] ^ t1;
    cur[2] = prev[2] ^ t2;
    cur[3] = prev[3] ^ t3;
    for (size_t i = 4; i < 16; ++i) {
      cur[i] = prev[i] ^ cur[i - 4];
    }
  }
}

inline void encryptBlock(const uint8_t rk[11][16], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  for (size_t i = 0; i < 16; ++i) {
    s[i] = in[i] ^ rk[0][i];
  }
  for (int round = 1; round <= 10; ++round) {
    for (size_t i = 0; i < 16; ++i) {
      s[i] = kSBox[s[i]];
    }
    // ShiftRows (column-major state).
    uint8_t t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[3];
    s[3] = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = t;
    if (round != 10) {
      for (size_t c = 0; c < 4; ++c) {
        const uint8_t a0 = s[4 * c];
        const uint8_t a1 = s[4 * c + 1];
        const uint8_t a2 = s[4 * c + 2];
        const uint8_t a3 = s[4 * c + 3];
        const uint8_t x0 = xtime(a0);
        const uint8_t x1 = xtime(a1);
        const uint8_t x2 = xtime(a2);
        const uint8_t x3 = xtime(a3);
        s[4 * c] = static_cast<uint8_t>(x0 ^ (x1 ^ a1) ^ a2 ^ a3);
        s[4 * c + 1] = static_cast<uint8_t>(a0 ^ x1 ^ (x2 ^ a2) ^ a3);
        s[4 * c + 2] = static_cast<uint8_t>(a0 ^ a1 ^ x2 ^ (x3 ^ a3));
        s[4 * c + 3] = static_cast<uint8_t>((x0 ^ a0) ^ a1 ^ a2 ^ x3);
      }
    }
    for (size_t i = 0; i < 16; ++i) {
      s[i] ^= rk[round][i];
    }
  }
  for (size_t i = 0; i < 16; ++i) {
    out[i] = s[i];
  }
}

// GF(2^128) multiply, right-shift form (reduction constant 0xE1 at the top).
inline void ghashMul(const uint8_t x[16], const uint8_t h[16], uint8_t out[16]) {
  uint8_t z[16] = {};
  uint8_t v[16];
  for (size_t i = 0; i < 16; ++i) {
    v[i] = h[i];
  }
  for (size_t i = 0; i < 16; ++i) {
    uint8_t b = x[i];
    for (int j = 0; j < 8; ++j) {
      if ((b & 0x80u) != 0) {
        for (size_t k = 0; k < 16; ++k) {
          z[k] ^= v[k];
        }
      }
      const uint8_t lsb = static_cast<uint8_t>(v[15] & 1u);
      for (size_t k = 15; k > 0; --k) {
        v[k] = static_cast<uint8_t>((v[k] >> 1) | (v[k - 1] << 7));
      }
      v[0] >>= 1;
      if (lsb != 0) {
        v[0] ^= 0xE1;
      }
      b = static_cast<uint8_t>(b << 1);
    }
  }
  for (size_t i = 0; i < 16; ++i) {
    out[i] = z[i];
  }
}

inline void ghash(const uint8_t h[16], const uint8_t* ct, size_t ctLen, uint8_t out[16]) {
  uint8_t y[16] = {};
  uint8_t blk[16];
  size_t off = 0;
  while (off < ctLen) {
    size_t n = ctLen - off;
    if (n > 16) {
      n = 16;
    }
    for (size_t i = 0; i < 16; ++i) {
      blk[i] = (i < n) ? ct[off + i] : 0;
    }
    for (size_t i = 0; i < 16; ++i) {
      blk[i] ^= y[i];
    }
    ghashMul(blk, h, y);
    off += n;
  }
  uint8_t lens[16] = {};
  const uint64_t bits = static_cast<uint64_t>(ctLen) * 8u;
  for (int i = 0; i < 8; ++i) {
    lens[8 + i] = static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
  }
  for (size_t i = 0; i < 16; ++i) {
    lens[i] ^= y[i];
  }
  ghashMul(lens, h, out);
}

inline void crypt(const uint8_t rk[11][16], const uint8_t nonce[kNonceLen], const uint8_t* in, size_t len,
                  uint8_t* out) {
  uint8_t ctr[16] = {};
  for (size_t i = 0; i < kNonceLen; ++i) {
    ctr[i] = nonce[i];
  }
  ctr[15] = 1;
  size_t off = 0;
  while (off < len) {
    uint32_t c = (static_cast<uint32_t>(ctr[12]) << 24) | (static_cast<uint32_t>(ctr[13]) << 16) |
                 (static_cast<uint32_t>(ctr[14]) << 8) | static_cast<uint32_t>(ctr[15]);
    ++c;
    ctr[12] = static_cast<uint8_t>((c >> 24) & 0xFFu);
    ctr[13] = static_cast<uint8_t>((c >> 16) & 0xFFu);
    ctr[14] = static_cast<uint8_t>((c >> 8) & 0xFFu);
    ctr[15] = static_cast<uint8_t>(c & 0xFFu);
    uint8_t ks[16];
    encryptBlock(rk, ctr, ks);
    size_t n = len - off;
    if (n > 16) {
      n = 16;
    }
    for (size_t i = 0; i < n; ++i) {
      out[off + i] = static_cast<uint8_t>(in[off + i] ^ ks[i]);
    }
    off += n;
  }
}

}  // namespace portable
#endif  // !LINK_CRYPTO_HAVE_MBEDTLS_GCM

// Seal: out gets ciphertext || 16-byte tag. Returns sealed bytes, 0 on error.
inline size_t seal(const Key& key, const uint8_t nonce[kNonceLen], const uint8_t* plain, size_t plainLen, uint8_t* out,
                   size_t outCap) {
  if (!key.ok || nonce == nullptr || out == nullptr || (plainLen > 0 && plain == nullptr)) {
    return 0;
  }
  if (outCap < plainLen + kTagLen || plainLen + kTagLen < plainLen) {
    return 0;
  }
#if LINK_CRYPTO_HAVE_MBEDTLS_GCM
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.bytes, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plainLen, nonce, kNonceLen, nullptr, 0, plain, out,
                                   kTagLen, out + plainLen);
  }
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    return 0;
  }
  return plainLen + kTagLen;
#else
  uint8_t rk[11][16];
  portable::expandKey(key.bytes, rk);
  portable::crypt(rk, nonce, plain, plainLen, out);
  uint8_t h[16] = {};
  portable::encryptBlock(rk, h, h);
  uint8_t s[16];
  portable::ghash(h, out, plainLen, s);
  uint8_t j0[16] = {};
  for (size_t i = 0; i < kNonceLen; ++i) {
    j0[i] = nonce[i];
  }
  j0[15] = 1;
  uint8_t e0[16];
  portable::encryptBlock(rk, j0, e0);
  for (size_t i = 0; i < kTagLen; ++i) {
    out[plainLen + i] = static_cast<uint8_t>(s[i] ^ e0[i]);
  }
  return plainLen + kTagLen;
#endif
}

// Open: sealed holds ciphertext || 16-byte tag. Fail-closed: false leaves no
// plaintext behind (out is zeroed up to outCap).
inline bool open(const Key& key, const uint8_t nonce[kNonceLen], const uint8_t* sealed, size_t sealedLen, uint8_t* out,
                 size_t outCap, size_t& plainLen) {
  plainLen = 0;
  if (!key.ok || nonce == nullptr || out == nullptr || (sealedLen > 0 && sealed == nullptr)) {
    return false;
  }
  if (sealedLen < kTagLen || outCap < sealedLen - kTagLen) {
    return false;
  }
  const size_t bodyLen = sealedLen - kTagLen;
  const uint8_t* tag = sealed + bodyLen;
#if LINK_CRYPTO_HAVE_MBEDTLS_GCM
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.bytes, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&ctx, bodyLen, nonce, kNonceLen, nullptr, 0, tag, kTagLen, sealed, out);
  }
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    for (size_t i = 0; i < outCap; ++i) {
      out[i] = 0;
    }
    return false;
  }
  plainLen = bodyLen;
  return true;
#else
  uint8_t rk[11][16];
  portable::expandKey(key.bytes, rk);
  uint8_t h[16] = {};
  portable::encryptBlock(rk, h, h);
  uint8_t s[16];
  portable::ghash(h, sealed, bodyLen, s);
  uint8_t j0[16] = {};
  for (size_t i = 0; i < kNonceLen; ++i) {
    j0[i] = nonce[i];
  }
  j0[15] = 1;
  uint8_t e0[16];
  portable::encryptBlock(rk, j0, e0);
  uint8_t want[16];
  for (size_t i = 0; i < kTagLen; ++i) {
    want[i] = static_cast<uint8_t>(s[i] ^ e0[i]);
  }
  if (!tagEqual(want, tag, kTagLen)) {
    for (size_t i = 0; i < outCap; ++i) {
      out[i] = 0;
    }
    return false;
  }
  portable::crypt(rk, nonce, sealed, bodyLen, out);
  plainLen = bodyLen;
  return true;
#endif
}

// ── SHA-256 (self-contained so host and firmware derive identical bytes) ─────
namespace portable {

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint64_t total = 0;
  uint8_t buf[64] = {};
  size_t buffered = 0;
};

inline void shaCompress(Sha256& c, const uint8_t blk[64]) {
  static const uint32_t kK[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(blk[4 * i]) << 24) | (static_cast<uint32_t>(blk[4 * i + 1]) << 16) |
           (static_cast<uint32_t>(blk[4 * i + 2]) << 8) | static_cast<uint32_t>(blk[4 * i + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.h[0];
  uint32_t b = c.h[1];
  uint32_t cc = c.h[2];
  uint32_t d = c.h[3];
  uint32_t e = c.h[4];
  uint32_t f = c.h[5];
  uint32_t g = c.h[6];
  uint32_t hh = c.h[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c.h[0] += a;
  c.h[1] += b;
  c.h[2] += cc;
  c.h[3] += d;
  c.h[4] += e;
  c.h[5] += f;
  c.h[6] += g;
  c.h[7] += hh;
}

inline void shaUpdate(Sha256& c, const uint8_t* data, size_t len) {
  if (data == nullptr) {
    return;
  }
  c.total += static_cast<uint64_t>(len);
  while (len > 0) {
    size_t n = 64 - c.buffered;
    if (n > len) {
      n = len;
    }
    for (size_t i = 0; i < n; ++i) {
      c.buf[c.buffered + i] = data[i];
    }
    c.buffered += n;
    data += n;
    len -= n;
    if (c.buffered == 64) {
      shaCompress(c, c.buf);
      c.buffered = 0;
    }
  }
}

inline void shaFinal(Sha256& c, uint8_t out[32]) {
  const uint64_t bits = c.total * 8u;
  uint8_t pad = 0x80;
  shaUpdate(c, &pad, 1);
  uint8_t zero = 0;
  while (c.buffered != 56) {
    shaUpdate(c, &zero, 1);
  }
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; ++i) {
    lenBytes[i] = static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
  }
  shaUpdate(c, lenBytes, 8);
  for (size_t i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<uint8_t>((c.h[i] >> 24) & 0xFFu);
    out[4 * i + 1] = static_cast<uint8_t>((c.h[i] >> 16) & 0xFFu);
    out[4 * i + 2] = static_cast<uint8_t>((c.h[i] >> 8) & 0xFFu);
    out[4 * i + 3] = static_cast<uint8_t>(c.h[i] & 0xFFu);
  }
}

}  // namespace portable

// Deterministic AP passphrase from the raw secret: both sides compute it with
// zero communication. SHA-256 over (fixed label, zero separator, secret); the
// first 50 digest bits pick 10 alphabet chars after the readable prefix.
inline bool deriveApPassphrase(const uint8_t* secret, size_t secretLen, char* out, size_t outCap) {
  if (secret == nullptr || secretLen == 0 || out == nullptr || outCap < kPassphraseCap) {
    return false;
  }
  portable::Sha256 c;
  portable::shaUpdate(c, reinterpret_cast<const uint8_t*>(kApLabel), sizeof(kApLabel) - 1);
  const uint8_t sep = 0;
  portable::shaUpdate(c, &sep, 1);
  portable::shaUpdate(c, secret, secretLen);
  uint8_t digest[32];
  portable::shaFinal(c, digest);
  for (size_t i = 0; i < 6; ++i) {
    out[i] = kPassPrefix[i];
  }
  // 10 chars x 5 bits from the first 7 digest bytes (70 bits, 50 used).
  uint64_t acc = 0;
  for (size_t i = 0; i < 7; ++i) {
    acc = (acc << 8) | digest[i];
  }
  for (size_t i = 0; i < kPassHashChars; ++i) {
    const int shift = static_cast<int>((kPassHashChars - 1 - i) * 5 + 6);
    out[6 + i] = kPassAlphabet[(acc >> shift) & 31u];
  }
  out[kPassphraseLen] = '\0';
  return true;
}

#ifdef ARDUINO
// ── Firmware build binding: shared secret from the build environment ─────────
// DESKY_LINK_SECRET_HEX arrives via -D from ${sysenv.DESKY_LINK_SECRET} (see
// platformio.ini). Absent or malformed values stop the build here, never a
// silent fallback. Including this header from firmware pulls in the check.
#ifndef DESKY_LINK_SECRET_HEX
#error "DESKY_LINK_SECRET is not defined: export 32 hex chars (see embedded/.env.example)"
#endif
#define LINK_CRYPTO_STR_INNER(x) #x
#define LINK_CRYPTO_STR(x) LINK_CRYPTO_STR_INNER(x)
constexpr char kSecretHex[] = LINK_CRYPTO_STR(DESKY_LINK_SECRET_HEX);
#undef LINK_CRYPTO_STR
#undef LINK_CRYPTO_STR_INNER
constexpr bool isSecretHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
constexpr bool isSecretHexStr(const char* s, size_t i) {
  return i == kSecretHexLen ? (s[i] == '\0') : (isSecretHexChar(s[i]) && isSecretHexStr(s, i + 1));
}
static_assert(sizeof(kSecretHex) - 1 == kSecretHexLen,
              "DESKY_LINK_SECRET must be exactly 32 hex chars (16 bytes, see embedded/.env.example)");
static_assert(isSecretHexStr(kSecretHex, 0), "DESKY_LINK_SECRET must be hex (0-9, a-f, see embedded/.env.example)");

inline uint8_t secretHexVal(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(c - 'a' + 10);
  }
  return static_cast<uint8_t>(c - 'A' + 10);
}

// Provisioned unit key. The static_asserts above already pinned the shape,
// so this cannot fail on a passing build; still fail-closed by construction.
inline Key provisionedKey() {
  uint8_t raw[kKeyLen] = {};
  for (size_t i = 0; i < kKeyLen; ++i) {
    raw[i] = static_cast<uint8_t>((secretHexVal(kSecretHex[2 * i]) << 4) | secretHexVal(kSecretHex[2 * i + 1]));
  }
  return provision(raw, sizeof(raw));
}
#endif  // ARDUINO

}  // namespace linkcrypto
