#pragma once
// desky-head WiFi link mapping — MIDDLEWARE compatibility shim (Task 2 split).
//
// The socket client (class LinkClient) + framing predicates now live in
// transport/link_client.h + transport/link_codec.h as dumb pipes (business
// decisions arrive there as injected callbacks bound in main.cpp). This shim
// re-exports those transport headers and retains the business mapping
// helpers below (mapCmd/isPairShowFrame/isPairDoneFrame/isOtaFrame) VERBATIM
// for Tasks 3-5, so existing host tests including this path keep passing
// with zero test changes.
//
// POSIX gotcha (discovered on core, not rediscovered here): newlib declares
// ::link(), so `namespace link` at global scope BREAKS Arduino TUs. The
// codec is included ONCE inside linkwrap__ (see transport/link_codec.h) and
// used via the `linkcode` alias uniformly (same workaround as core's
// link_server.h). The Arduino-free helpers below and host tests still
// exercise the canonical codec paths through the alias (identical entities;
// canonical `link::` spelling stays pinned by core's test_link_codec.cpp
// goldens).

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"
#include "transport/link_client.h"
#include "transport/link_codec.h"

namespace wifilink {

// Codec frame -> ONE command-table PowerCommand (FACE/OLED/CAMCTL only).
// Value-checked through the shared codec vocabulary (linkcode::unpackFace /
// unpackOled / unpackCamCtl — the wire values ARE the vocabulary, validated
// once in link/link_codec.h): OLED wire 0/1/2 maps onto the head DisplayState
// order (which inverts the wire order — see the note on unpackOled, never
// re-derived here), CAMCTL 0/1. False = not a command frame or bad value
// (caller drops; QRBITMAP/JPEG/HB/HB_ACK/ACK/unknown never map here).
inline bool mapCmd(uint8_t type, const uint8_t* payload, uint16_t len, PowerCommand& out) {
  if (len > 0 && payload == nullptr) {
    return false;
  }
  switch (type) {
    case linkcode::kTypeFace: {
      if (len != linkcode::kFaceLen) {
        return false;
      }
      uint8_t id = 0;
      if (!linkcode::unpackFace(payload[0], id)) {
        return false;
      }
      out.target = CmdTarget::FACE;
      out.arg = static_cast<int32_t>(id);  // clamped downstream by apply/clampFace
      return true;
    }
    case linkcode::kTypeOled: {
      if (len != linkcode::kOledLen) {
        return false;
      }
      uint8_t mode = 0;
      if (!linkcode::unpackOled(payload[0], mode)) {
        return false;
      }
      out.target = CmdTarget::OLED;
      switch (mode) {
        case linkcode::kOledOn:
          out.arg = static_cast<int32_t>(OLED_ON);
          break;
        case linkcode::kOledDim:
          out.arg = static_cast<int32_t>(OLED_DIM);
          break;
        case linkcode::kOledOff:
          out.arg = static_cast<int32_t>(OLED_OFF);
          break;
        default:
          return false;  // unreachable: unpackOled accepted only the three above
      }
      return true;
    }
    case linkcode::kTypeCamCtl: {
      if (len != linkcode::kCamCtlLen) {
        return false;
      }
      uint8_t wire = 0;
      if (!linkcode::unpackCamCtl(payload[0], wire)) {
        return false;
      }
      out.target = CmdTarget::CAM;
      out.arg = (wire == linkcode::kCamOn) ? 1 : 0;
      return true;
    }
    default:
      return false;
  }
}

// QRBITMAP in this task (historical): the Slice 6c bitmap push is gone —
// the flag-day removal dropped the wire trigger, so no frame is ever handed
// to the FaceRenderer from the link. The reserved 63B length contract stays
// frozen in the codec (never reused); the USB CLI `qr` demo still exercises
// the same 63B staging path locally via showQrDemo (no link involved).

// PAIRSHOW in this task: the codec-validated 8B length is the gate here;
// the 6-digit vocabulary lives once in linkcode::pack/unpackPairShow (the
// shared digits+valid-secs contract), which handleCmd calls — never
// re-derived. Fresh frames hand the PIN + window to the FaceRenderer via the
// power manager (authoritative-PIN show mode) + ACKs; duplicates only
// re-ACK, never re-render.
inline bool isPairShowFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairShow && len == linkcode::kPairShowLen;
}

// PAIRDONE in this task: the fixed-empty contract (0B — the frame presence
// is the signal, enforced by the codec length gate in decodeFrame). Fresh
// frames clear the pairing show via the power manager + ACKs; duplicates
// only re-ACK, never re-clear.
inline bool isPairDoneFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypePairDone && len == linkcode::kPairDoneLen;
}

// OTA in this task: the codec-validated 1..128B length is the gate here;
// the byte grammar (printable ASCII, no '@'/whitespace) + the bounded copy
// live once in otamanifest::copyOtaUrl, which handleCmd calls — never
// re-derived. Fresh frames hand the URL to the existing OTA updater entry;
// duplicates only re-ACK, never re-trigger an in-flight download.
inline bool isOtaFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypeOta && len >= linkcode::kOtaUrlMinLen && len <= linkcode::kOtaUrlMaxLen;
}

}  // namespace wifilink
