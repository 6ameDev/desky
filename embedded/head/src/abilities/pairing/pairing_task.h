#pragma once
// Pairing link-to-manager binder (firmware only): LinkHandlers-compatible
// onPairShow/onPairDone entries (transport seq-gated + ACKed already).

#ifdef ARDUINO

#include <Arduino.h>
#include <stdint.h>

#include "abilities/pairing/pairing_manager.h"
#include "common/logger.h"

namespace pairingtask {

// Ignores null ctx; the manager drops bad PINs with no display change.
inline void onPairShow(void* ctx, const uint8_t pin6[6], uint16_t validSecs) {
  auto* mgr = static_cast<PairingManager*>(ctx);
  if (mgr == nullptr) {
    return;
  }
  if (mgr->show(pin6, validSecs, millis())) {
    LOG_I("FACE", "pairing shown valid=%us", static_cast<unsigned>(validSecs));
  }
}

inline void onPairDone(void* ctx) {
  auto* mgr = static_cast<PairingManager*>(ctx);
  if (mgr == nullptr) {
    return;
  }
  if (mgr->clear()) {
    LOG_I("FACE", "pairing cleared (link)");
  }
}

}  // namespace pairingtask

#endif  // ARDUINO
