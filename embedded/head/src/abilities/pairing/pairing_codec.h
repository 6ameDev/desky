#pragma once
// desky-head pairing ability — consumer frame vocabulary (Arduino-free,
// host-testable).
//
// Ability layer: pairing_codec (pure length gates over the shared link
// vocabulary, 100% native-testable). Moved verbatim from
// middleware/wifi_link.h (Task 4 split): the PairShow/PairDone frame gates
// the transport handleCmd branch mirrors. The 6-digit + valid-secs grammar
// lives once in linkcode::pack/unpackPairShow (shared/link, never
// re-derived here); the frozen wire contract (PairShow 0x06 8B / PairDone
// 0x07 0B) is pinned by the static_asserts below AND the literal-byte
// goldens in test/test_pair_show.cpp. No ARDUINO, no sockets, no display.

#include <stdint.h>

#include "transport/link_codec.h"

namespace pairinglink {

// PAIRSHOW consumer gate: the codec-validated 8B length is the gate here;
// the 6-digit vocabulary lives once in linkcode::pack/unpackPairShow (the
// shared digits+valid-secs contract), which handleCmd calls — never
// re-derived. Fresh frames hand the PIN + window to the display via the
// pairing manager + ACKs; duplicates only re-ACK, never re-render.
inline bool isPairShowFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairShow && len == linkcode::kPairShowLen;
}

// PAIRDONE consumer gate: the fixed-empty contract (0B — the frame presence
// is the signal, enforced by the codec length gate in decodeFrame). Fresh
// frames clear the pairing show via the pairing manager + ACKs; duplicates
// only re-ACK, never re-clear.
inline bool isPairDoneFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairDone && len == linkcode::kPairDoneLen;
}

static_assert(linkcode::kTypePairShow == 0x06, "frozen PairShow type byte");
static_assert(linkcode::kPairShowLen == 8, "frozen PairShow payload length");
static_assert(linkcode::kTypePairDone == 0x07, "frozen PairDone type byte");
static_assert(linkcode::kPairDoneLen == 0, "frozen PairDone payload length");

}  // namespace pairinglink

// wifilink:: spelling (Tasks 2-5 shim re-export): host goldens address these
// gates through wifilink::isPairShowFrame/isPairDoneFrame — same entities as
// pairinglink:: above, so test sources keep working with include-path updates
// only. New code prefers the canonical pairinglink:: spelling.
namespace wifilink {
using pairinglink::isPairDoneFrame;
using pairinglink::isPairShowFrame;
}  // namespace wifilink
