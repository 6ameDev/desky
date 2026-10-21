#pragma once
// desky-head pairing ability — link→manager binder (firmware only).
//
// Ability layer: pairing_task (event binder gluing transport to the pure
// pairing manager). Owns the LinkHandlers-compatible onPairShow/onPairDone
// entries bound in main.cpp (ctx = PairingManager*, stored never owned
// there):
//   - onPairShow: transport already seq-gated + ACKed + unpacked the frame
//     via linkcode::unpackPairShow (the shared digits+valid-secs grammar —
//     never re-derived here). Forwards the authoritative PIN + window to the
//     pure manager with millis() (the manager fail-closes on a null/bad PIN
//     and re-arms the valid-secs deadline on every fresh show), then logs
//     exactly like the renderer show path it replaces.
//   - onPairDone: forwards to manager clear (no-op when nothing is up, so
//     only real transitions log).
// Transport keeps seq gating + ACK order untouched; this binder only decides
// show vs clear vs ignore. No sockets, no panel, no PowerManager.

#ifdef ARDUINO

#include <Arduino.h>
#include <stdint.h>

#include "abilities/pairing/pairing_manager.h"
#include "common/logger.h"

namespace pairingtask {

// Validated PAIRSHOW edge (digits + window unpacked by transport). Ignores
// null ctx; the pure manager drops bad PINs with no display change.
inline void onPairShow(void* ctx, const uint8_t pin6[6], uint16_t validSecs) {
  auto* mgr = static_cast<PairingManager*>(ctx);
  if (mgr == nullptr) {
    return;
  }
  if (mgr->show(pin6, validSecs, millis())) {
    LOG_I("FACE", "pairing shown valid=%us", static_cast<unsigned>(validSecs));
  }
}

// Validated PAIRDONE edge (presence is the signal). Ignores null ctx.
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
