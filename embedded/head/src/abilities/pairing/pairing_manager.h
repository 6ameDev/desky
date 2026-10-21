#pragma once
// desky-head pairing ability — PIN handoff policy (Arduino-free, host-testable).
//
// Ability layer: pairing_manager (state machine + business logic, zero socket
// deps). Owns the PairShow/PairDone display handoff as pure policy over the
// single FaceOverlayManager (face_manager.h, the single home of overlay +
// deadline state — never forked here): show() fail-closed validates the
// 6-digit PIN via pairdisplay::isValidPin, then forwards (every fresh show
// re-arms the valid-secs deadline downstream); clear() clears the pairing
// show (no-op when nothing is up); expired() delegates the wrap-safe deadline
// gate (no local deadline copy). Paint + logs + expiry cadence stay in the
// render binding (face_task.h, over the same overlays instance main shares);
// millis() + the LinkHandlers edge stay in the binder (pairing_task.h). No
// sockets, no WiFi, no PowerManager — main attaches the shared overlays
// instance (stored, never owned).

#include <stdint.h>

#include "abilities/face/face_manager.h"

class PairingManager {
 public:
  PairingManager() : face_(nullptr) {}

  // Binds the single overlay owner (stored, never owned). Call once in
  // setup() with the renderer's overlays before the link handlers attach.
  void attach(FaceOverlayManager* face) { face_ = face; }

  // Fresh PairShow handoff: fail-closed PIN check, then forward (re-arms the
  // valid-secs deadline on every call). True = forwarded; false = no display
  // change (null face or bad PIN).
  bool show(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs, uint32_t nowMs) {
    if (face_ == nullptr || !pairdisplay::isValidPin(pin6)) {
      return false;
    }
    FaceTransition t;
    return face_->showPairing(pin6, validSecs, nowMs, t);
  }

  // PairDone handoff: clears the pairing show (no-op when nothing is up, so
  // only real transitions log downstream). True = an overlay was up.
  bool clear() {
    if (face_ == nullptr) {
      return false;
    }
    FaceTransition t;
    return face_->clearPairing(t);
  }

  bool active() const { return face_ != nullptr && face_->pairActive(); }

  // Deadline gate passthrough: the face manager owns pairDeadlineMs_ — this
  // keeps no local copy (single home, no fork).
  bool expired(uint32_t nowMs) const { return face_ != nullptr && face_->pairExpired(nowMs); }

 private:
  FaceOverlayManager* face_;  // overlay owner (main binds, never owned here)
};
