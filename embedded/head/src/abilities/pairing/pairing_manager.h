#pragma once
// Pairing PIN handoff policy (Arduino-free, host-testable) over the single
// FaceOverlayManager (stored, never owned).

#include <stdint.h>

#include "abilities/face/face_manager.h"

class PairingManager {
 public:
  PairingManager() : face_(nullptr) {}

  // Call once in setup() before the link handlers attach.
  void attach(FaceOverlayManager* face) { face_ = face; }

  // Fail-closed PIN check, then forward. False = no display change.
  bool show(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs) {
    if (face_ == nullptr || !pairdisplay::isValidPin(pin6)) {
      return false;
    }
    FaceTransition t;
    return face_->showPairing(pin6, validSecs, nowMs, t);
  }

  // No-op when nothing is up.
  bool clear() {
    if (face_ == nullptr) {
      return false;
    }
    FaceTransition t;
    return face_->clearPairing(t);
  }

  bool active() const { return face_ != nullptr && face_->pairActive(); }

  bool expired(uint32_t nowMs) const { return face_ != nullptr && face_->pairExpired(nowMs); }

 private:
  FaceOverlayManager* face_;
};
