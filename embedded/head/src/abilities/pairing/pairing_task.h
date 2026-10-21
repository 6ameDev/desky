#pragma once
// desky-head pairing ability — link→display binder (firmware only).
//
// Ability layer: pairing_task (event binder gluing transport to the pairing
// manager). Owns the LinkHandlers-compatible onPairShow/onPairDone entries
// bound in main.cpp (ctx = PairingManager*, stored never owned there):
//   - onPairShow: transport already seq-gated + ACKed + unpacked the frame
//     via linkcode::unpackPairShow (the shared digits+valid-secs grammar —
//     never re-derived here); this edge re-checks the PIN through the codec
//     vocabulary (pairdisplay::isValidPin, defense in depth — the manager
//     re-checks fail-closed too) and forwards to manager show (re-arms the
//     valid-secs deadline on every fresh show).
//   - onPairDone: forwards to manager clear (no-op when nothing is up).
// Transport keeps seq gating + ACK order untouched; this binder only decides
// show vs clear vs ignore. No sockets, no panel, no EventBus beyond what the
// manager already owns.

#ifdef ARDUINO

#include <stdint.h>

#include "abilities/pairing/pairing_codec.h"
#include "abilities/pairing/pairing_manager.h"

namespace pairingtask {

// Validated PAIRSHOW edge (digits + window unpacked by transport). Ignores
// null ctx/manager and bad PINs (no display change); otherwise hands the
// authoritative PIN + window to the manager (re-arms the deadline).
inline void onPairShow(void* ctx, const uint8_t pin6[6], uint16_t validSecs) {
  auto* mgr = static_cast<PairingManager*>(ctx);
  if (mgr == nullptr) {
    return;
  }
  if (!pairdisplay::isValidPin(pin6)) {
    return;
  }
  mgr->show(pin6, validSecs);
}

// Validated PAIRDONE edge (presence is the signal). Ignores null ctx.
inline void onPairDone(void* ctx) {
  auto* mgr = static_cast<PairingManager*>(ctx);
  if (mgr == nullptr) {
    return;
  }
  mgr->clear();
}

}  // namespace pairingtask

#endif  // ARDUINO
