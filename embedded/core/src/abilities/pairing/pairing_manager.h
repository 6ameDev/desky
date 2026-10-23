#pragma once
// Pairing manager — abilities/pairing.
//
// pairing::Core: window/tries/lockout/burn + pair/era lifecycle. Zero
// sockets, zero NVS, zero RNG (firmware injects randomness at the call
// site). Wire vocabulary lives in pairing_codec.h.

#include <stddef.h>
#include <stdint.h>

#include "pairing_codec.h"

namespace pairing {

class Core {
 public:
  Core()
      : windowOpen_(false),
        paired_(false),
        burned_(false),
        tries_(0),
        windowStartMs_(0),
        lockoutStartMs_(0),
        era_(0),
        pairMs_(0) {
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

  // Single-use attempt. Unsigned subtraction: wrap-safe across millis()
  // rollover.
  Result tryCode(const uint8_t* in, size_t len, uint32_t nowMs) {
    if (paired_) {
      return Result::kAlreadyPaired;
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
      onPaired(nowMs);
      return Result::kOk;
    }
    ++tries_;
    if (tries_ >= kMaxTries) {
      lockoutStartMs_ = nowMs;
      return Result::kLocked;
    }
    return Result::kBadCode;
  }

  // Explicit unpair / 10s-hold factory reset. Never implicit: rotation or
  // reboot must not call this.
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  void clearPair() {
    paired_ = false;
    burned_ = false;
    windowOpen_ = false;
    tries_ = 0;
    era_ = 0;
    pairMs_ = 0;
  }

  // Pair transitions. PairingTask calls these at the same points it flips
  // its own flags (same order: verify -> persist -> switch -> ACK).

  // Records a fresh pair: gate closed, era reset, pair clock set. Called by
  // tryCode on kOk AND forwarded after the NVS save + PAIR_OK send
  // (idempotent).
  void onPaired(uint32_t nowMs) {
    paired_ = true;
    burned_ = true;
    windowOpen_ = false;
    era_ = 0;
    pairMs_ = nowMs;
  }

  // Records a Tier-1 rotation (forwarded after the NVS savePairRefresh + atomic
  // switch, BEFORE the ACK).
  void onPairRefreshApplied(uint16_t nextEra) { era_ = nextEra; }

  // Symmetric forget (UNPAIR path): full gate reset + era/pair clock clear.
  void onUnpaired() { clearPair(); }

  bool isPaired() const { return paired_; }

  // Open-window read for the mode policy: true while a window is live
  // (unexpired, unburned, unpaired). A lockout still counts as open: the
  // window outlives the retry budget.
  bool windowOpen(uint32_t nowMs) const {
    return windowOpen_ && !burned_ && !paired_ && ((nowMs - windowStartMs_) < kWindowMs);
  }

  uint32_t tries() const { return tries_; }

  uint16_t era() const { return era_; }

  uint32_t pairMs() const { return pairMs_; }

 private:
  uint8_t code_[kCodeLen];
  bool windowOpen_;
  bool paired_;
  bool burned_;
  uint32_t tries_;
  uint32_t windowStartMs_;
  uint32_t lockoutStartMs_;
  uint16_t era_;
  uint32_t pairMs_;
};

}  // namespace pairing
