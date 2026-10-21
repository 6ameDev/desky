#pragma once
// desky-head pairing ability — PIN handoff policy (firmware only).
//
// Ability layer: pairing_manager (business logic, zero socket deps).
// Owns the PairShow/PairDone display handoff: validates the 6-digit PIN via
// pairdisplay::isValidPin (fail-closed — a null/bad PIN changes no display
// state), then forwards show/clear through the existing face API
// (PowerManager::showPairing/clearPairing -> FaceRenderer -> the single
// FaceOverlayManager, so the Task 3 rivalry rules are never forked here).
// Every show() forwards, so each fresh PairShow re-arms the valid-secs
// deadline (no once-latch); transport already dedups (duplicate seqs only
// re-ACK, never re-render). No panel, no sockets, no EventBus — the manager
// stores the PowerManager (never owned, main binds it) and calls only the
// two pairing forwarders.

#ifdef ARDUINO

#include <stdint.h>

#include "abilities/face/face_codec.h"
#include "services/power_manager.h"

class PairingManager {
 public:
  PairingManager() : power_(nullptr) {}

  // Binds the display owner (stored, never owned). Call once in setup()
  // after PowerManager::begin(), before the link handlers attach.
  void attach(PowerManager* power) { power_ = power; }

  // Fresh PairShow handoff: fail-closed PIN check, then forward (re-arms the
  // valid-secs deadline on every call). True = forwarded; false = no display
  // change (null power or bad PIN).
  bool show(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs) {
    if (power_ == nullptr || !pairdisplay::isValidPin(pin6)) {
      return false;
    }
    power_->showPairing(pin6, validSecs);
    return true;
  }

  // PairDone handoff: clears the pairing show (no-op when nothing is up, so
  // only real transitions log downstream). Returns whether a show was up.
  bool clear() {
    if (power_ == nullptr) {
      return false;
    }
    const bool was = power_->pairActive();
    power_->clearPairing("link");
    return was;
  }

  bool active() const { return power_ != nullptr && power_->pairActive(); }

 private:
  PowerManager* power_;  // display owner (main binds, never owned here)
};

#endif  // ARDUINO
