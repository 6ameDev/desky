#pragma once
// desky v2 pairing manager — abilities/pairing.
//
// pairing::Core: window/tries/lockout/burn + bond/era lifecycle. Zero
// sockets, zero NVS, zero RNG (firmware injects randomness at the call
// site). Transport forwards bond transitions at the same points it flips
// its own flags (see pairing_task.h; full handler extraction lands in
// Slice 6b). Wire vocabulary lives in pairing_codec.h.

#include <stddef.h>
#include <stdint.h>

#include "pairing_codec.h"

namespace pairing {

class Core {
 public:
  Core()
      : windowOpen_(false),
        bonded_(false),
        burned_(false),
        tries_(0),
        windowStartMs_(0),
        lockoutStartMs_(0),
        era_(0),
        bondMs_(0) {
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
      onPairBonded(nowMs);
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
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  void clearBond() {
    bonded_ = false;
    burned_ = false;
    windowOpen_ = false;
    tries_ = 0;
    era_ = 0;
    bondMs_ = 0;
  }

  // ── PairingExchange bond transitions (platform split 1.1) ─────────────
  // The gate above (window/tries/lockout/burn) already lived here; the
  // bond/era lifecycle joins it so UdpServer only forwards. Residual (by
  // design, documented in udp_server.h): the PSK bytes, the drive counter
  // + validCount_ persist cadence, and the PAIR_OK redelivery cache stay in
  // UdpServer — key material needs mbedTLS zeroize (firmware-only) and the
  // cache rides udpstatus::shouldRedeliver. UdpServer calls these at the
  // same points it flips its own flags (same order: verify ->
  // markAlive-position -> persist -> switch -> ACK).

  // Records a fresh bond: gate closed, era reset (fresh pair starts era 0 —
  // saveBond persists it), bond clock set. Called by tryCode on kOk AND
  // forwarded by UdpServer after the NVS save + PAIR_OK send (idempotent —
  // same values twice).
  void onPairBonded(uint32_t nowMs) {
    bonded_ = true;
    burned_ = true;
    windowOpen_ = false;
    era_ = 0;
    bondMs_ = nowMs;
  }

  // Records a Tier-1 rotation (forwarded by UdpServer after the NVS
  // saveRekey + atomic switch, BEFORE the ACK).
  void onRekeyApplied(uint16_t nextEra) { era_ = nextEra; }

  // Symmetric forget (UNBOND path): full gate reset + era/bond clock clear.
  void onUnbonded() { clearBond(); }

  bool bonded() const { return bonded_; }

  uint32_t tries() const { return tries_; }

  uint16_t era() const { return era_; }

  uint32_t bondMs() const { return bondMs_; }

 private:
  uint8_t code_[kCodeLen];
  bool windowOpen_;
  bool bonded_;
  bool burned_;
  uint32_t tries_;
  uint32_t windowStartMs_;
  uint32_t lockoutStartMs_;
  uint16_t era_;
  uint32_t bondMs_;
};

}  // namespace pairing
