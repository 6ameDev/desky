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
  Core()
      : windowOpen_(false),
        bonded_(false),
        burned_(false),
        tries_(0),
        windowStartMs_(0),
        lockoutStartMs_(0) {
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
