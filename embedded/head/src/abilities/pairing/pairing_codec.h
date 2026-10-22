#pragma once
// Pairing consumer frame vocabulary (Arduino-free, host-testable):
// length gates over the shared link vocabulary.

#include <stdint.h>

#include "transport/link_codec.h"

namespace pairinglink {

// PAIRSHOW gate: codec-validated 8B length (digits+window live in linkcode).
inline bool isPairShowFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairShow && len == linkcode::kPairShowLen;
}

// PAIRDONE gate: empty payload (presence is the signal).
inline bool isPairDoneFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairDone && len == linkcode::kPairDoneLen;
}

static_assert(linkcode::kTypePairShow == 0x06, "frozen PairShow type byte");
static_assert(linkcode::kPairShowLen == 8, "frozen PairShow payload length");
static_assert(linkcode::kTypePairDone == 0x07, "frozen PairDone type byte");
static_assert(linkcode::kPairDoneLen == 0, "frozen PairDone payload length");

}  // namespace pairinglink

// wifilink:: spelling for the same gates (test sources use either).
namespace wifilink {
using pairinglink::isPairDoneFrame;
using pairinglink::isPairShowFrame;
}  // namespace wifilink
