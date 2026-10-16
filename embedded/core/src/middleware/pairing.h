#pragma once
// desky v2 pairing core — header-only, Task 2 of pairing/auth v1.
//
// One-time enrolment gate (Tier 1): a 6-digit ephemeral code proves physical
// presence; success mints the Tier 2 bond flag (the PSK itself lands in
// Task 3 NVS + Task 4 HKDF/AES-GCM delivery). Rotation of the code never
// evicts an existing bond; single owner (a new bond requires an explicit
// clearBond, e.g. the 10s button hold).
//
// Structure mirrors udp_server.h / coordinator.h: this header is Arduino-free
// pure logic (stdint/stddef only) so host Unity tests include it directly.
// No heap, no mbedTLS here (constant-time compare is an inline byte loop;
// the call site swaps in mbedtls_ct_memcmp in Task 4 without changing
// semantics). Firmware RNG (esp_random digits) is injected, never called.

#include <stddef.h>
#include <stdint.h>

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as coordinator.h / udp_server.h).
#ifndef CFG_PAIR_CODE_LEN
#define CFG_PAIR_CODE_LEN 6
#endif
#ifndef CFG_PAIR_WINDOW_MS
#define CFG_PAIR_WINDOW_MS 300000
#endif
#ifndef CFG_PAIR_MAX_TRIES
#define CFG_PAIR_MAX_TRIES 5
#endif
#ifndef CFG_PAIR_LOCKOUT_MS
#define CFG_PAIR_LOCKOUT_MS 30000
#endif

namespace pairing {

constexpr size_t kCodeLen = CFG_PAIR_CODE_LEN;
constexpr uint32_t kWindowMs = CFG_PAIR_WINDOW_MS;
constexpr uint32_t kMaxTries = CFG_PAIR_MAX_TRIES;
constexpr uint32_t kLockoutMs = CFG_PAIR_LOCKOUT_MS;

// PAIR_OK persist-interval byte (v1.1 addendum to the frozen v1 vectors):
// the GCM plaintext grows psk16 -> psk16||persist_u8 (17B), so PAIR_OK
// grows 38B -> 39B [0x51|srvNonce4|ct17|tag16|XOR]. The byte rides inside
// the AES-GCM ct (auth by tag+AAD like the PSK itself — never a bare
// field), is accepted once at pair only, and seeds the app's DriveCounter
// persist cadence. Guard is minimal (not an allowlist): the wire holds the
// u8 subset (8..255) of the 8..1024 app-domain guard — 0..7 (incl.
// div-zero 0) and anything above 255 (incl. the app-reserved 256..1024,
// a future u16 field) map to the default. An old (v1) robot sends 38B with
// no byte — the app defaults to 128 in that case.
constexpr uint8_t kPairPersistDefault = 128;  // == COUNTER_PERSIST_EVERY both sides
constexpr uint16_t kPairPersistMin = 8;
constexpr uint16_t kPairPersistMax = 1024;  // App-domain max; wire holds the u8 subset (see below).
constexpr size_t kPairWrapPlainLen = 17;    // psk16 || persist_u8
constexpr size_t kPairCtLen = 17;
constexpr size_t kPairOkSize = 39;
constexpr size_t kPairOkLegacySize = 38;  // v1 robots without the interval byte

// Minimal guard for the negotiated persist cadence: N in 8..255 passes
// (the wire-representable subset of the 8..1024 app-domain guard),
// anything else -> 128. Kills 0 (div-zero in the % cadence) + absurd
// values; accepted once at pair only (see the wrapPsk call site).
inline uint8_t sanitizePersistEvery(uint16_t v) {
  if (v < kPairPersistMin || v > kPairPersistMax || v > 255) {
    return kPairPersistDefault;
  }
  return static_cast<uint8_t>(v);
}

// ── Tier-1 silent auto-rekey (era + PSK rotation, fully silent) ──────────
// Wire (see shared/protocol/README.md §"Tier-1 silent auto-rekey"):
//   REKEY_REQ  App -> Robot, 14B [0x53 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(oldPsk, "REKEY" || eraBE2 || counterBE4))
//     era = requested new era (stored era + 1, u16 wrap mod 65536).
//   REKEY_ACK Robot -> App, 10B [0x54 | eraBE2 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(newPsk, "REKEYACK" || eraBE2)).
// Key schedule (no key transport — mirrors TLS KeyUpdate / IPsec rekey):
//   PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16)
// both sides derive; only the era needs sync + confirm. Counter resets to
// the bond baseline (0 persisted, first drive uses 1 — same as fresh pair)
// so the session resumes gapless before EXHAUSTION_GATE. Old PSK is dropped
// immediately on switch (no reorder window — documented); a duplicate REQ
// with era == current era re-ACKs idempotently without re-deriving.
constexpr uint8_t kRekeyReqHeader = 0x53;
constexpr size_t kRekeyReqSize = 14;
constexpr uint8_t kRekeyAckHeader = 0x54;
constexpr size_t kRekeyAckSize = 10;
constexpr size_t kRekeyTagLen = 6;
constexpr size_t kRekeyMsgLen = 11;     // "REKEY"(5) || eraBE2 || counterBE4
constexpr size_t kRekeyAckMsgLen = 10;  // "REKEYACK"(8) || eraBE2

// REKEY_REQ tag message (11B, host-testable): "REKEY" ASCII || eraBE2 ||
// counterBE4. Domain-separates the rekey tag from drive tags (no prefix)
// and unbond tags ("UNBOND" prefix).
inline void rekeyTagMsg(uint16_t era, uint32_t counter, uint8_t out11[kRekeyMsgLen]) {
  if (out11 == nullptr) {
    return;
  }
  out11[0] = 'R';
  out11[1] = 'E';
  out11[2] = 'K';
  out11[3] = 'E';
  out11[4] = 'Y';
  out11[5] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out11[6] = static_cast<uint8_t>(era & 0xFFu);
  out11[7] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out11[8] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out11[9] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out11[10] = static_cast<uint8_t>(counter & 0xFFu);
}

// REKEY_ACK tag message (10B, host-testable): "REKEYACK" ASCII || eraBE2.
// Authenticated under the NEW PSK so the ACK proves the switch.
inline void rekeyAckTagMsg(uint16_t era, uint8_t out10[kRekeyAckMsgLen]) {
  if (out10 == nullptr) {
    return;
  }
  out10[0] = 'R';
  out10[1] = 'E';
  out10[2] = 'K';
  out10[3] = 'E';
  out10[4] = 'Y';
  out10[5] = 'A';
  out10[6] = 'C';
  out10[7] = 'K';
  out10[8] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out10[9] = static_cast<uint8_t>(era & 0xFFu);
}

// ── ALIVE + DISCONNECT (robot alive rule, Phase-2 keepalive) ───────────────
// Wire (see shared/protocol/README.md §"ALIVE + DISCONNECT + robot alive
// rule"):
//   ALIVE      App -> Robot, 14B [0x55 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "ALIVE" || eraBE2 || counterBE4))
//     era = CURRENT bonded era (stored era; a rekey moves it, see below).
//   DISCONNECT App -> Robot, 14B [0x56 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "DISCONNECT" || eraBE2 || counterBE4)).
// Both ride the shared strictly-increasing counter (same holder as DRIVE_V2
// + REKEY_REQ, allocated app-side): freshness is isFreshCounter, and a valid
// frame consumes lastCounter_ exactly like drive. Era must equal the stored
// era (current session); old-key frames die via HMAC with no layout change.
// Headers 0x55/0x56 are claimed here — never reuse 0xAA/0xAC/0xBB/0x50/
// 0x51/0x52/0x53/0x54/0xAD.
constexpr uint8_t kAliveHeader = 0x55;
constexpr size_t kAliveSize = 14;
constexpr uint8_t kDisconnectHeader = 0x56;
constexpr size_t kDisconnectSize = 14;
constexpr size_t kAliveTagLen = 6;
constexpr size_t kAliveMsgLen = 11;  // "ALIVE"(5) || eraBE2 || counterBE4
constexpr size_t kDisconnectTagLen = 6;
constexpr size_t kDisconnectMsgLen = 16;  // "DISCONNECT"(10) || eraBE2 || counterBE4

// ALIVE tag message (11B, host-testable): "ALIVE" ASCII || eraBE2 ||
// counterBE4. Domain-separates the alive tag from drive tags (no prefix),
// unbond tags ("UNBOND" prefix), and rekey tags ("REKEY" prefix).
inline void aliveTagMsg(uint16_t era, uint32_t counter, uint8_t out11[kAliveMsgLen]) {
  if (out11 == nullptr) {
    return;
  }
  out11[0] = 'A';
  out11[1] = 'L';
  out11[2] = 'I';
  out11[3] = 'V';
  out11[4] = 'E';
  out11[5] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out11[6] = static_cast<uint8_t>(era & 0xFFu);
  out11[7] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out11[8] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out11[9] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out11[10] = static_cast<uint8_t>(counter & 0xFFu);
}

// DISCONNECT tag message (16B, host-testable): "DISCONNECT" ASCII || eraBE2
// || counterBE4. Same domain-separation rule as ALIVE above.
inline void disconnectTagMsg(uint16_t era, uint32_t counter, uint8_t out16[kDisconnectMsgLen]) {
  if (out16 == nullptr) {
    return;
  }
  out16[0] = 'D';
  out16[1] = 'I';
  out16[2] = 'S';
  out16[3] = 'C';
  out16[4] = 'O';
  out16[5] = 'N';
  out16[6] = 'N';
  out16[7] = 'E';
  out16[8] = 'C';
  out16[9] = 'T';
  out16[10] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out16[11] = static_cast<uint8_t>(era & 0xFFu);
  out16[12] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out16[13] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out16[14] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out16[15] = static_cast<uint8_t>(counter & 0xFFu);
}

// Wire reasons in shared/protocol/README.md §v1 (PAIR_ERR reason byte).
// kOk is transport-internal (PAIR_OK); it never appears on the wire.
enum class Result : uint8_t {
  kOk = 0xFF,
  kBadCode = 0,
  kLocked = 1,
  kNotInWindow = 2,
  kAlreadyBonded = 3,
};

inline uint8_t toWireReason(Result r) { return static_cast<uint8_t>(r); }

inline bool isDigitByte(uint8_t b) { return b >= '0' && b <= '9'; }

// Constant-time 6-byte compare: accumulates the full diff, no early exit.
// (Call-site rule in Task 4: prefer mbedtls_ct_memcmp where mbedTLS is
// linked; this loop is the portable semantic the host tests pin.)
inline bool constantTimeEqual6(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kCodeLen; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

class Core {
 public:
  Core() : windowOpen_(false), bonded_(false), burned_(false), tries_(0), windowStartMs_(0), lockoutStartMs_(0) {
    for (size_t i = 0; i < kCodeLen; ++i) {
      code_[i] = '0';
    }
  }

  // Opens a pairing window showing `code` (6 ASCII digits). Returns false on
  // null/non-digit code (firmware bug — window stays as-was). Resets tries.
  bool openWindow(uint32_t nowMs, const uint8_t* code) {
    if (code == nullptr) {
      return false;
    }
    for (size_t i = 0; i < kCodeLen; ++i) {
      if (!isDigitByte(code[i])) {
        return false;
      }
    }
    for (size_t i = 0; i < kCodeLen; ++i) {
      code_[i] = code[i];
    }
    windowOpen_ = true;
    burned_ = false;
    tries_ = 0;
    windowStartMs_ = nowMs;
    return true;
  }

  // Single-use attempt. Unsigned subtraction throughout: wrap-safe across
  // the ~49-day millis() rollover (same idiom as coordinator.h).
  Result tryCode(const uint8_t* in, size_t len, uint32_t nowMs) {
    if (bonded_) {
      return Result::kAlreadyBonded;
    }
    if (!windowOpen_ || burned_ || (nowMs - windowStartMs_) >= kWindowMs) {
      windowOpen_ = false;
      return Result::kNotInWindow;
    }
    if (tries_ >= kMaxTries && (nowMs - lockoutStartMs_) < kLockoutMs) {
      return Result::kLocked;
    }
    if (tries_ >= kMaxTries) {
      tries_ = 0;  // Lockout expired: fresh set of tries in the same window.
    }
    bool match = false;
    if (in != nullptr && len == kCodeLen) {
      match = constantTimeEqual6(code_, in);
    }
    if (match) {
      bonded_ = true;
      burned_ = true;
      windowOpen_ = false;
      return Result::kOk;
    }
    ++tries_;
    if (tries_ >= kMaxTries) {
      lockoutStartMs_ = nowMs;
      return Result::kLocked;
    }
    return Result::kBadCode;
  }

  // Explicit unpair / 10s-hold factory reset (Task 3 persists it). Never
  // implicit: rotation or reboot must not call this.
  void clearBond() {
    bonded_ = false;
    burned_ = false;
    windowOpen_ = false;
    tries_ = 0;
  }

  bool bonded() const { return bonded_; }

  uint32_t tries() const { return tries_; }

 private:
  uint8_t code_[kCodeLen];
  bool windowOpen_;
  bool bonded_;
  bool burned_;
  uint32_t tries_;
  uint32_t windowStartMs_;
  uint32_t lockoutStartMs_;
};

}  // namespace pairing

#ifdef ARDUINO
// ── Firmware: pairing crypto glue (mbedTLS, Task 4) ───────────────────────
// Arduino-ESP32 (S3) bundles mbedTLS; headers live under the framework
// package and are included as <mbedtls/...>. No heap, no String, and no
// logging of key material anywhere in this section (never LOG key, nonce,
// or code bytes) — failures surface as silent bools for the PAIR_ERR path.
//
// CALL-ORDER CONTRACT (store-before-send; the PAIR_OK send path must follow
// it exactly so app-bonded/robot-unbonded is impossible):
//   1. Core::tryCode(in, len, nowMs) == Result::kOk (code verified + burned
//      inside Core; single-use, window/lockout enforced by the top half).
//   2. Fresh PSK = 16 esp_random bytes into a caller-owned stack buffer.
//   3. ConfigStore::saveBond(psk, 1) FIRST — persist before anything leaves
//      the radio. A failed save aborts: answer PAIR_ERR, never PAIR_OK.
//   4. deriveKek(code, clientNonce, serverNonce, kek), then
//      wrapPsk(psk, kek, clientNonce, serverNonce, persistU8, ct, tag)
//      with persistU8 = kPairPersistDefault (128) — sanitized inside wrap.
//   5. Send PAIR_OK ([0x51 | server_nonce | ct17 | tag | XOR]); zeroize the
//      kek/psk stack copies after the send.
// Layer rule: crypto glue is middleware (this header); NVS persistence is
// services/ConfigStore; policy/state is pairing::Core above.

#include <mbedtls/cipher.h>
// NOTE: this vendored constant_time.h ships WITHOUT the extern "C" guard
// every other mbedtls header has, so a plain include mangles
// mbedtls_ct_memcmp in C++ and ctEqual below fails at link (undefined
// _Z17mbedtls_ct_memcmp...). The prebuilt lib provides the C symbol, hence
// the explicit C-linkage wrapper (harmless if a future header adds its own).
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "common/fault_manager.h"  // DESKY_ASSERT (firmware side only)

namespace pairing {

// Sizes frozen by shared/protocol/README.md §"Pairing + authenticated drive
// v1". Local constexprs (not ConfigStore) so this header stays includable
// without the Arduino Preferences stack.
constexpr size_t kPskLen = 16;
constexpr size_t kKekLen = 16;
constexpr size_t kPairNonceLen = 4;
constexpr size_t kPairIvLen = 12;
constexpr size_t kPairAadLen = 9;  // 0x51 || client_nonce(4) || server_nonce(4)
constexpr size_t kPairTagLen = 16;
constexpr uint8_t kPairOkHeader = 0x51;

// Derives the pairing KEK: KEK = HKDF-SHA256(salt=client_nonce||server_nonce,
// IKM=6 ASCII code digits, info="desky-pair-v1", L=16). Returns false on null
// input or any mbedTLS error (kekOut zeroized on failure). Salt lives in a
// fixed-size stack buffer and is zeroized on all paths.
inline bool deriveKek(const uint8_t code6[kCodeLen], const uint8_t clientNonce[kPairNonceLen],
                      const uint8_t serverNonce[kPairNonceLen], uint8_t kekOut[kKekLen]) {
  DESKY_ASSERT(code6 != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(kekOut != nullptr);
  if (code6 == nullptr || clientNonce == nullptr || serverNonce == nullptr || kekOut == nullptr) {
    return false;
  }
  uint8_t salt[kPairNonceLen * 2];
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[kPairNonceLen + i] = serverNonce[i];
  }
  static const char kInfo[] = "desky-pair-v1";
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, salt, sizeof(salt), code6, kCodeLen, reinterpret_cast<const unsigned char*>(kInfo),
                      sizeof(kInfo) - 1, kekOut, kKekLen);
  }
  mbedtls_platform_zeroize(salt, sizeof(salt));
  if (rc != 0) {
    mbedtls_platform_zeroize(kekOut, kKekLen);
    return false;
  }
  return true;
}

// Wraps the bonded PSK + persist-interval byte for PAIR_OK:
// ct||tag = AES-128-GCM(KEK, nonce12=server_nonce||client_nonce||00 00 00
// 00, AAD=0x51||client_nonce||server_nonce, psk16||persist_u8). The 17th
// plaintext byte carries sanitizePersistEvery(persistU8) (default 128), so
// the cadence is authenticated by the GCM tag+AAD exactly like the PSK.
// Layout: PAIR_OK 39B [0x51|srvNonce4|ct17|tag16|XOR] (see
// shared/protocol/README.md §v1.1 addendum). Returns false on null input
// or any mbedTLS error; gcm_free runs on all paths, and ct/tag outputs are
// zeroized on failure (caller still owns psk/kek — const here — and must
// zeroize its own copies after the send per the contract above).
inline bool wrapPsk(const uint8_t psk[kPskLen], const uint8_t kek[kKekLen], const uint8_t clientNonce[kPairNonceLen],
                    const uint8_t serverNonce[kPairNonceLen], uint8_t persistU8, uint8_t ctOut[kPairCtLen],
                    uint8_t tagOut[kPairTagLen]) {
  DESKY_ASSERT(psk != nullptr);
  DESKY_ASSERT(kek != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(ctOut != nullptr);
  DESKY_ASSERT(tagOut != nullptr);
  if (psk == nullptr || kek == nullptr || clientNonce == nullptr || serverNonce == nullptr || ctOut == nullptr ||
      tagOut == nullptr) {
    return false;
  }
  uint8_t iv[kPairIvLen];
  uint8_t aad[kPairAadLen];
  uint8_t plain[kPairWrapPlainLen];
  for (size_t i = 0; i < kPskLen; ++i) {
    plain[i] = psk[i];
  }
  plain[kPskLen] = sanitizePersistEvery(persistU8);
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[i] = serverNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[kPairNonceLen + i] = clientNonce[i];
  }
  for (size_t i = 2 * kPairNonceLen; i < kPairIvLen; ++i) {
    iv[i] = 0x00;
  }
  aad[0] = kPairOkHeader;
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + kPairNonceLen + i] = serverNonce[i];
  }
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, kPairWrapPlainLen, iv, sizeof(iv), aad, sizeof(aad),
                                   plain, ctOut, kPairTagLen, tagOut);
  }
  mbedtls_gcm_free(&ctx);
  mbedtls_platform_zeroize(iv, sizeof(iv));
  mbedtls_platform_zeroize(aad, sizeof(aad));
  mbedtls_platform_zeroize(plain, sizeof(plain));
  if (rc != 0) {
    mbedtls_platform_zeroize(ctOut, kPairCtLen);
    mbedtls_platform_zeroize(tagOut, kPairTagLen);
    return false;
  }
  return true;
}

// Constant-time buffer compare for the Task 5 DRIVE_V2 verify path
// (HMAC tag check). Thin wrapper over mbedtls_ct_memcmp; returns false on
// null input. Note: pairing::constantTimeEqual6 above stays the host-testable
// semantic for the 6-byte code compare — same no-early-exit rule, Arduino-free
// so Unity pins it; this wrapper is just the firmware call-site rule.
inline bool ctEqual(const void* a, const void* b, size_t n) {
  DESKY_ASSERT(a != nullptr);
  DESKY_ASSERT(b != nullptr);
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return mbedtls_ct_memcmp(a, b, n) == 0;
}

// Tier-1 rekey key schedule (firmware side):
//   PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16).
// Salt lives in ROM (string literal, no stack copy needed); the 2-byte era
// info is built on the stack and zeroized. Returns false on null input or
// any mbedTLS error (out zeroized on failure). Caller owns oldPsk/newPsk
// lifetime and must zeroize its own copies after the switch per the
// store-before-ACK contract (see udp_server.h handleRekey).
inline bool deriveRekeyPsk(const uint8_t oldPsk[kPskLen], uint16_t nextEra, uint8_t nextPskOut[kPskLen]) {
  DESKY_ASSERT(oldPsk != nullptr);
  DESKY_ASSERT(nextPskOut != nullptr);
  if (oldPsk == nullptr || nextPskOut == nullptr) {
    return false;
  }
  static const char kSalt[] = "desky-rekey-v1";
  uint8_t info[2];
  info[0] = static_cast<uint8_t>((nextEra >> 8) & 0xFFu);
  info[1] = static_cast<uint8_t>(nextEra & 0xFFu);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, reinterpret_cast<const unsigned char*>(kSalt), sizeof(kSalt) - 1, oldPsk, kPskLen, info,
                      sizeof(info), nextPskOut, kPskLen);
  }
  mbedtls_platform_zeroize(info, sizeof(info));
  if (rc != 0) {
    mbedtls_platform_zeroize(nextPskOut, kPskLen);
    return false;
  }
  return true;
}

}  // namespace pairing

#endif  // ARDUINO
