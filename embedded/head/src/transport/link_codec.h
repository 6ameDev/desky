#pragma once
// desky-head link transport codec — dumb pipes only (framing, frag geometry,
// seq gate, HB/assoc timing). No business state, no sockets. Moved verbatim
// from the Arduino-free top half of middleware/wifi_link.h (Task 1 split).

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"

// The frozen codec namespace `link` collides with POSIX ::link() (declared
// via the WiFi/lwIP stack) in any Arduino TU, and the codec itself must not
// be touched. Included once here inside linkwrap__ and aliased back on ALL
// platforms (uniform spelling, immune to include order).
namespace linkwrap__ {
#include "link/link_codec.h"
}  // namespace linkwrap__
namespace linkcode = linkwrap__::link;

// Fallback keeps host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins (same value, never re-derived).
#ifndef CFG_LINK_MAX_MISSED
#define CFG_LINK_MAX_MISSED 3
#endif

namespace wifilink {

// Transport MTU for fragmentation: one link frame max (header + 1024B
// payload). Per-frame payload + frame count derive from the codec so the
// README MTU rule stays single-sourced.
constexpr size_t kLinkMtu = linkcode::kMaxFrame;

inline size_t perFramePayload() { return linkcode::maxPayloadFor(kLinkMtu); }

inline size_t fragTotalFor(size_t totalLen) { return linkcode::fragCount(totalLen, kLinkMtu); }

// Seq gate verdict for one inbound core->head frame (host-testable). Fresh =
// first-ever frame or wrap-aware strictly greater (linkcode::isFreshSeq);
// Duplicate = exact retransmit of the last accepted seq (caller re-ACKs
// idempotently without re-executing, mirror of the core duplicate rule);
// Stale = anything else (replay/older, silent drop).
enum class RxVerdict : uint8_t { kFresh, kDuplicate, kStale };

inline RxVerdict classifyRx(bool haveSeen, uint16_t lastSeen, uint16_t got) {
  if (!haveSeen) {
    return RxVerdict::kFresh;
  }
  if (got == lastSeen) {
    return RxVerdict::kDuplicate;
  }
  return linkcode::isFreshSeq(lastSeen, got) ? RxVerdict::kFresh : RxVerdict::kStale;
}

// Log-only type name (never payload bytes — see the counters-only rule).
// Returns "UNKNOWN" for anything outside the frozen contract.
inline const char* typeName(uint8_t type) {
  switch (type) {
    case linkcode::kTypeFace:
      return "FACE";
    case linkcode::kTypeOled:
      return "OLED";
    case linkcode::kTypeCamCtl:
      return "CAMCTL";
    case linkcode::kTypeQrBitmap:
      return "QRBITMAP";
    case linkcode::kTypePairShow:
      return "PAIRSHOW";
    case linkcode::kTypePairDone:
      return "PAIRDONE";
    case linkcode::kTypeOta:
      return "OTA";
    case linkcode::kTypeJpeg:
      return "JPEG";
    case linkcode::kTypeHb:
      return "HB";
    case linkcode::kTypeHbAck:
      return "HB_ACK";
    case linkcode::kTypeAck:
      return "ACK";
    default:
      return "UNKNOWN";
  }
}

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
      out.arg = static_cast<int32_t>(id);  // clamped downstream by dispatch/clampFace
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

// OTA consumer gate: the codec-validated 1..128B length is the gate here;
// the byte grammar (printable ASCII, no '@'/whitespace) + the bounded copy
// live once in otamanifest::copyOtaUrl, which handleCmd calls — never
// re-derived. Fresh frames hand the URL to the existing OTA updater entry;
// duplicates only re-ACK, never re-trigger an in-flight download.
inline bool isOtaFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypeOta && len >= linkcode::kOtaUrlMinLen && len <= linkcode::kOtaUrlMaxLen;
}

// Wrap-safe periodic gates (mirror the `now - last >= interval` idiom used
// by main.cpp and the stub).
inline bool shouldSendHb(uint32_t nowMs, uint32_t lastHbMs, uint32_t intervalMs) {
  return (nowMs - lastHbMs) >= intervalMs;
}

inline bool retryDue(uint32_t nowMs, uint32_t lastTryMs, uint32_t retryMs) { return (nowMs - lastTryMs) >= retryMs; }

// Association-attempt driver (host-testable). WiFi.begin() is ASYNC: the
// handshake runs in the background and WL_CONNECTED appears on a later tick.
// connecting is true between begin() and the connect/timeout verdict.
// Returns what the caller must do this tick. NEVER re-begin an in-flight
// attempt (that aborts the handshake, so same-tick re-begin loops can never
// connect); restart only after timeoutMs with no connect. Wrap-safe via
// unsigned subtraction (same idiom as retryDue above).
enum class AssocAction : uint8_t { kBegin, kWait, kConnected, kRestart };

inline AssocAction assocStep(bool connecting, bool connected, uint32_t nowMs, uint32_t attemptStartMs,
                             uint32_t timeoutMs) {
  if (connected) {
    return AssocAction::kConnected;
  }
  if (!connecting) {
    return AssocAction::kBegin;
  }
  return ((nowMs - attemptStartMs) >= timeoutMs) ? AssocAction::kRestart : AssocAction::kWait;
}

// Link-down rule: 3 missed HB ACKs (~3s at the 1Hz HB rate).
inline bool linkDownAfter(uint8_t missed) { return missed >= CFG_LINK_MAX_MISSED; }

// Byte slice of message [0, totalLen) carried by fragment `idx` of `total`
// at `perFrame` payload bytes per frame. False on any bad index/geometry.
inline bool fragSlice(size_t totalLen, size_t perFrame, uint8_t idx, uint8_t total, size_t& offOut, size_t& lenOut) {
  offOut = 0;
  lenOut = 0;
  if (perFrame == 0 || total == 0 || idx >= total) {
    return false;
  }
  if (totalLen == 0) {
    return total == 1 && idx == 0;  // empty message rides one empty frame
  }
  const size_t off = static_cast<size_t>(idx) * perFrame;
  if (off >= totalLen) {
    return false;
  }
  const size_t remain = totalLen - off;
  offOut = off;
  lenOut = remain > perFrame ? perFrame : remain;
  return true;
}

}  // namespace wifilink
