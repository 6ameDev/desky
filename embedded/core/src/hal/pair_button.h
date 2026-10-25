#pragma once
// Pair button driver (behind no interface) — header-only HAL.
//
// Temporary home is the S3 BOOT button on GPIO0 (active-low, input pull-up).
// All pairing code refers to the pairButton name, never raw 0: a future
// button swap is one pin-constant change in the MCU header.
// BOOT strapping note: GPIO0 must read HIGH at reset; INPUT_PULLUP holds it
// HIGH and a press pulls LOW, so boot is unaffected.
//
// Top half (namespace pairbtn) is Arduino-free pure logic so host tests
// include this header directly. Bottom half (class PairButton) is
// firmware-only: pinMode + digitalRead poll, no ISR, no heap.

#include <stdint.h>

#include "config.h"

#ifndef CFG_PAIR_BUTTON_DEBOUNCE_MS
#define CFG_PAIR_BUTTON_DEBOUNCE_MS 50
#endif

namespace pairbtn {

// Forced-AP latch hold: the pair button held this long across boot jumps
// straight to the provisioning AP until reboot.
constexpr uint32_t kForceApHoldMs = 3000;

// True once the press held holdMs. Unsigned subtraction: wrap-safe across
// millis() rollover.
inline bool forceApLatched(uint32_t pressStartMs, uint32_t nowMs, uint32_t holdMs = kForceApHoldMs) {
  return (nowMs - pressStartMs) >= holdMs;
}

// Debounce state for an active-low button. stablePressed is the debounced
// level, lastChangeMs stamps the last raw transition.
struct State {
  bool stablePressed = false;
  bool lastRawPressed = false;
  uint32_t lastChangeMs = 0;
  bool seeded = false;
};

// Returns true exactly once per debounced press edge (LOW asserts).
// Pure function — the driver feeds digitalRead()==LOW as rawPressed.
inline bool update(State& s, bool rawPressed, uint32_t nowMs, uint32_t debounceMs) {
  if (!s.seeded) {
    s.seeded = true;
    s.stablePressed = rawPressed;
    s.lastRawPressed = rawPressed;
    s.lastChangeMs = nowMs;
    return false;
  }
  if (rawPressed != s.lastRawPressed) {
    s.lastRawPressed = rawPressed;
    s.lastChangeMs = nowMs;
  }
  if ((nowMs - s.lastChangeMs) >= debounceMs && s.stablePressed != s.lastRawPressed) {
    s.stablePressed = s.lastRawPressed;
    return s.stablePressed;
  }
  return false;
}

}  // namespace pairbtn

#ifdef ARDUINO

#include <Arduino.h>

#include "common/fault_manager.h"
#include "common/logger.h"

class PairButton {
 public:
  explicit PairButton(int pin, uint32_t debounceMs = CFG_PAIR_BUTTON_DEBOUNCE_MS)
      : pin_(pin), debounceMs_(debounceMs) {}

  // Arms INPUT_PULLUP; BOOT reads HIGH so setup is unaffected.
  bool init() {
    pinMode(static_cast<uint8_t>(pin_), INPUT_PULLUP);
    state_.lastChangeMs = millis();
    LOG_I("PAIRBTN", "pair button ready pin=%d debounce=%lums", pin_, static_cast<unsigned long>(debounceMs_));
    return true;
  }

  // Non-blocking poll: true once per debounced press.
  bool poll() { return pollAt(millis(), digitalRead(static_cast<uint8_t>(pin_)) == LOW); }

  // Test seam: inject time + raw level.
  bool pollAt(uint32_t nowMs, bool rawPressed) { return pairbtn::update(state_, rawPressed, nowMs, debounceMs_); }

  // Boot forced-AP latch: true once the button held holdMs across this
  // call, false on early release. Returns on one read when released, so
  // normal boots pay nothing; feeds the task WDT while held (setup
  // already subscribed it). The caller latches the result until reboot.
  bool pollForceApAtBoot(uint32_t holdMs = pairbtn::kForceApHoldMs) {
    if (digitalRead(static_cast<uint8_t>(pin_)) != LOW) {
      return false;
    }
    const uint32_t startMs = millis();
    while (digitalRead(static_cast<uint8_t>(pin_)) == LOW) {
      FaultManager::watchdogFeed();
      if (pairbtn::forceApLatched(startMs, millis(), holdMs)) {
        return true;
      }
      delay(10);
    }
    return false;
  }

 private:
  int pin_;
  uint32_t debounceMs_;
  pairbtn::State state_;
};

#endif  // ARDUINO
