#pragma once
// Pairing PIN handoff policy (Arduino-free, host-testable) over the single
// DisplayManager (stored, never owned).

#include <stdint.h>

#include "abilities/display/display_manager.h"

class PairingManager {
 public:
  PairingManager() : display_(nullptr) {}

  // Call once in setup() before the link handlers attach.
  void attach(DisplayManager* display) { display_ = display; }

  // Fail-closed PIN check, then forward. False = no display change.
  bool show(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs) {
    if (display_ == nullptr || !pairdisplay::isValidPin(pin6)) {
      return false;
    }
    DisplayTransition t;
    return display_->showPairing(pin6, validSecs, nowMs, t);
  }

  // No-op when nothing is up.
  bool clear() {
    if (display_ == nullptr) {
      return false;
    }
    DisplayTransition t;
    return display_->clearPairing(t);
  }

  bool active() const { return display_ != nullptr && display_->pairActive(); }

  bool expired(uint32_t nowMs) const { return display_ != nullptr && display_->pairExpired(nowMs); }

 private:
  DisplayManager* display_;
};
