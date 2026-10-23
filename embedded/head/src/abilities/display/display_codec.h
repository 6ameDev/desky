#pragma once
// Display ability codec: pure display contracts (Arduino-free, host-tested).

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"
#include "link/qr_codec.h"

// QR show mode geometry + payload contract.
// Symbol: 21x21 modules, 63B payload row-major MSB-first (3 packed bytes/row).
// Render: 2px/module + 8px quiet zone per side -> 58px square at (4, 3);
// the pairing column (CODE label, 6 digits, countdown) prints right of the
// block (x=66, 6x8 font): rows y=10 / y=22 / y=38, each 8px tall.
namespace qrdisplay {
constexpr int kModules = 21;
constexpr size_t kQrLen = 63;
constexpr int kScale = 2;
constexpr int kQuietPx = 8;
constexpr int kPanelW = 128;
constexpr int kPanelH = 64;
constexpr int kExtentPx = kModules * kScale + 2 * kQuietPx;
constexpr int kX0 = 4;
constexpr int kY0 = (kPanelH - kExtentPx) / 2;

constexpr int kPinX = 66;
constexpr int kCodeY = 10;
constexpr int kPinY = 22;
constexpr int kCountdownY = 38;
constexpr int kPinCols = 6;
constexpr int kPinCharW = 6;
constexpr int kPinCharH = 8;
constexpr int kPinW = kPinCols * kPinCharW;
constexpr int kPinH = kPinCharH;
constexpr int kCodeCols = 4;
constexpr int kCodeW = kCodeCols * kPinCharW;
constexpr int kCountdownMaxCols = 9;
constexpr int kCountdownMaxW = kCountdownMaxCols * kPinCharW;

static_assert(kX0 >= 0 && kY0 >= 0 && kX0 + kExtentPx <= kPanelW && kY0 + kExtentPx <= kPanelH,
              "qr block must fit the panel");
static_assert(kPinX >= 0 && kPinY >= 0 && kPinX + kPinW <= kPanelW && kPinY + kPinH <= kPanelH,
              "pin line must fit the panel");
static_assert(kPinX + kCodeW <= kPanelW && kCodeY >= 0 && kCodeY + kPinCharH <= kPanelH,
              "code label must fit the panel");
static_assert(kPinX + kCountdownMaxW <= kPanelW && kCountdownY >= 0 && kCountdownY + kPinCharH <= kPanelH,
              "countdown line must fit the panel");
static_assert(kPinX >= kX0 + kExtentPx, "pair column must sit right of the qr block");

inline bool fitsPanel() { return kX0 >= 0 && kY0 >= 0 && kX0 + kExtentPx <= kPanelW && kY0 + kExtentPx <= kPanelH; }

inline bool fitsPinLine() {
  return kPinX >= 0 && kPinY >= 0 && kPinX + kPinW <= kPanelW && kPinY + kPinH <= kPanelH && kPinX >= kX0 + kExtentPx;
}

inline bool fitsPairColumn() {
  return fitsPinLine() && kPinX + kCodeW <= kPanelW && kCodeY >= 0 && kCodeY + kPinCharH <= kPanelH &&
         kPinX + kCountdownMaxW <= kPanelW && kCountdownY >= 0 && kCountdownY + kPinCharH <= kPanelH;
}

inline bool isValidQrPayloadLen(uint16_t len) { return len == kQrLen; }

// Module bit, MSB-first row-major. Null/out-of-range -> 0 (fail-closed).
inline uint8_t moduleAt(const uint8_t* qr63, int x, int y) {
  if (qr63 == nullptr || x < 0 || x >= kModules || y < 0 || y >= kModules) {
    return 0;
  }
  const uint8_t b = qr63[static_cast<size_t>(y) * 3u + static_cast<size_t>(x >> 3)];
  return static_cast<uint8_t>((b >> (7 - (x & 7))) & 1u);
}

}  // namespace qrdisplay

// Pairing show mode state. Bitmap re-derived locally via qr::encode6Digits
// (no bitmap rides the wire). Deadline = show-time + validSecs*1000.
namespace pairdisplay {
constexpr uint32_t kMsPerSec = 1000;
constexpr size_t kPinLen = 6;
constexpr size_t kCountdownBuf = 12;

inline bool isValidPin(const uint8_t pin6[kPinLen]) {
  if (pin6 == nullptr) {
    return false;
  }
  for (size_t i = 0; i < kPinLen; ++i) {
    if (pin6[i] < '0' || pin6[i] > '9') {
      return false;
    }
  }
  return true;
}

inline uint32_t deadlineFor(uint32_t nowMs, uint16_t validSecs) {
  return nowMs + static_cast<uint32_t>(validSecs) * kMsPerSec;
}

inline bool isExpired(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

// Whole seconds left, rounded up so the first displayed second survives a
// full second. Wrap-safe, clamped at zero once the deadline passes.
inline uint32_t remainingSecs(uint32_t nowMs, uint32_t deadlineMs) {
  const int32_t diff = static_cast<int32_t>(deadlineMs - nowMs);
  if (diff <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(diff + static_cast<int32_t>(kMsPerSec - 1)) / kMsPerSec;
}

// "Mm SSs" with zero-padded seconds ("5m 00s", "0m 09s"). False on null/short buffer.
inline bool formatCountdown(uint32_t totalSecs, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  const uint32_t mins = totalSecs / 60u;
  const uint32_t secs = totalSecs % 60u;
  char mbuf[10];
  size_t mlen = 0;
  if (mins == 0) {
    mbuf[0] = '0';
    mlen = 1;
  } else {
    char rev[10];
    size_t rlen = 0;
    uint32_t m = mins;
    while (m > 0 && rlen < sizeof(rev)) {
      rev[rlen++] = static_cast<char>('0' + (m % 10u));
      m /= 10u;
    }
    if (m != 0) {
      return false;
    }
    mlen = rlen;
    for (size_t i = 0; i < rlen; ++i) {
      mbuf[i] = rev[rlen - 1 - i];
    }
  }
  if (mlen + 6 > outLen) {
    return false;
  }
  for (size_t i = 0; i < mlen; ++i) {
    out[i] = mbuf[i];
  }
  out[mlen] = 'm';
  out[mlen + 1] = ' ';
  out[mlen + 2] = static_cast<char>('0' + (secs / 10u));
  out[mlen + 3] = static_cast<char>('0' + (secs % 10u));
  out[mlen + 4] = 's';
  out[mlen + 5] = '\0';
  return true;
}

struct State {
  bool active = false;
  uint8_t pin[kPinLen] = {};
  uint32_t deadlineMs = 0;
};

inline void show(State& s, const uint8_t pin6[kPinLen], uint32_t nowMs, uint16_t validSecs) {
  if (!isValidPin(pin6)) {
    return;
  }
  for (size_t i = 0; i < kPinLen; ++i) {
    s.pin[i] = pin6[i];
  }
  s.deadlineMs = deadlineFor(nowMs, validSecs);
  s.active = true;
}

inline void hide(State& s) {
  s.active = false;
  s.deadlineMs = 0;
}

}  // namespace pairdisplay

// OTA progress mode geometry + state: fill bar + alert face on failure.
namespace otadisplay {
constexpr int kBarX0 = 14;
constexpr int kBarY0 = 27;
constexpr int kBarW = 100;
constexpr int kBarH = 10;

inline bool fitsPanel() {
  return kBarX0 >= 0 && kBarY0 >= 0 && kBarX0 + kBarW <= qrdisplay::kPanelW && kBarY0 + kBarH <= qrdisplay::kPanelH;
}

inline int fillW(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  return (kBarW - 4) * pct / 100;
}

struct Progress {
  bool active = false;
  bool failed = false;
  uint8_t pct = 0;
};

inline void show(Progress& p, uint8_t pct) {
  p.active = true;
  p.failed = false;
  p.pct = (pct > 100) ? 100 : pct;
}

inline void markFailed(Progress& p) {
  p.active = true;
  p.failed = true;
}

inline void hide(Progress& p) {
  p.active = false;
  p.failed = false;
  p.pct = 0;
}

}  // namespace otadisplay

// Eyes request map: V1 FACE_* ids -> vendored eyes eEmotions ordinals.
// Pure + host-tested; the firmware render binding static_asserts these
// against eEmotions. Blink is an action, not a preset, so FACE_BLINK lands
// on neutral eyes while the blink assistant pulses.
namespace eyesmap {
constexpr uint8_t kEmotionNormal = 0;
constexpr uint8_t kEmotionHappy = 3;
constexpr uint8_t kEmotionSad = 4;
constexpr uint8_t kEmotionSurprised = 8;
constexpr uint8_t kEmotionAwe = 17;

inline uint8_t emotionForFace(uint8_t faceId) {
  switch (faceId) {
    case FACE_HAPPY:
      return kEmotionHappy;
    case FACE_SAD:
      return kEmotionSad;
    case FACE_ALERT:
    case FACE_SURPRISED:
      return kEmotionSurprised;
    case FACE_CURIOUS:
      return kEmotionAwe;
    case FACE_BOOT:
    case FACE_BLINK:
    default:
      return kEmotionNormal;
  }
}

}  // namespace eyesmap

// Expression priority: one active id, higher preempts lower, equal refreshes,
// lower never displaces higher (release via the neutral family first).
// SURPRISED is play-once: it falls back to neutral after kSurprisedHoldMs
// while holding ids persist. The library has no curious preset; CURIOUS rides
// the wide Awe preset (firmware adds the sideways glance).
namespace exprprio {
constexpr uint32_t kSurprisedHoldMs = 1500;

inline uint8_t priorityFor(uint8_t faceId) {
  switch (faceId) {
    case FACE_SURPRISED:
      return 5;
    case FACE_ALERT:
      return 4;
    case FACE_SAD:
      return 3;
    case FACE_CURIOUS:
      return 2;
    case FACE_HAPPY:
      return 1;
    case FACE_BOOT:
    case FACE_BLINK:
    default:
      return 0;
  }
}

inline bool isPlayOnce(uint8_t faceId) { return faceId == FACE_SURPRISED; }

inline uint32_t holdMsFor(uint8_t faceId) { return isPlayOnce(faceId) ? kSurprisedHoldMs : 0; }

}  // namespace exprprio
