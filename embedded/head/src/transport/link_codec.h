#pragma once
// desky-head link transport codec — dumb pipes only (framing, frag geometry,
// seq gate, HB/assoc timing). No business state, no sockets. Moved verbatim
// from the Arduino-free top half of middleware/wifi_link.h (Task 1 split).

#include <stddef.h>
#include <stdint.h>

// The frozen codec namespace `link` collides with POSIX ::link() (declared
// via the WiFi/lwIP stack) in any Arduino TU, and the codec itself must not
// be touched. Included once here inside linkwrap__ and aliased back on ALL
// platforms (uniform spelling, immune to include order).
namespace linkwrap__ {
#include "link/link_codec.h"
}  // namespace linkwrap__
namespace linkcode = linkwrap__::link;

// Fallback keeps host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins. Mirrors middleware/wifi_link.h
// (guarded so both can coexist; same value, never re-derived).
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
