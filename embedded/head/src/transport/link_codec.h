#pragma once
// Link transport codec: framing, frag geometry, seq gate, HB/assoc timing.
// No business state, no sockets.

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"

// `link` collides with POSIX ::link() in Arduino TUs: included once here, aliased back.
namespace linkwrap__ {
#include "link/link_codec.h"
}  // namespace linkwrap__
namespace linkcode = linkwrap__::link;

#ifndef CFG_LINK_MAX_MISSED
#define CFG_LINK_MAX_MISSED 3
#endif

namespace wifilink {

// One link frame max; per-frame payload + frame count derive from the codec.
constexpr size_t kLinkMtu = linkcode::kMaxFrame;

inline size_t perFramePayload() { return linkcode::maxPayloadFor(kLinkMtu); }

inline size_t fragTotalFor(size_t totalLen) { return linkcode::fragCount(totalLen, kLinkMtu); }

// Fresh = first-ever or wrap-aware strictly greater; Duplicate = exact
// retransmit (caller re-ACKs, never re-executes); Stale = replay/older drop.
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

// Log-only type name ("UNKNOWN" outside the frozen contract).
inline const char* typeName(uint8_t type) {
  switch (type) {
    case linkcode::kTypeFace:
      return "FACE";
    case linkcode::kTypeOled:
      return "OLED";
    case linkcode::kTypeCamCtl:
      return "CAMCTL";
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

// Codec frame -> PowerCommand (FACE/OLED/CAMCTL only, value-checked through
// the shared codec vocabulary). False = not a command frame or bad value.
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
      out.arg = static_cast<int32_t>(id);  // clamped downstream
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
          return false;
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

// OTA consumer gate: codec-validated length only (grammar lives in otamanifest::copyOtaUrl).
inline bool isOtaFrame(uint8_t type, uint16_t len) {
  return type == linkcode::kTypeOta && len >= linkcode::kOtaUrlMinLen && len <= linkcode::kOtaUrlMaxLen;
}

inline bool shouldSendHb(uint32_t nowMs, uint32_t lastHbMs, uint32_t intervalMs) {
  return (nowMs - lastHbMs) >= intervalMs;
}

inline bool retryDue(uint32_t nowMs, uint32_t lastTryMs, uint32_t retryMs) { return (nowMs - lastTryMs) >= retryMs; }

// Association-attempt driver (host-testable). WiFi.begin() is async; never
// re-begin an in-flight attempt. Wrap-safe via unsigned subtraction.
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

// Link-down rule: 3 missed HB ACKs.
inline bool linkDownAfter(uint8_t missed) { return missed >= CFG_LINK_MAX_MISSED; }

// Byte slice of message [0, totalLen) carried by fragment `idx` of `total`.
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
