#pragma once
// Face ability codec: pure display contracts (Arduino-free, host-tested).

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"
#include "link/qr_codec.h"

// QR show mode geometry + payload contract.
// Symbol: 21x21 modules, 63B payload row-major MSB-first (3 packed bytes/row).
// Render: 2px/module + 8px quiet zone per side -> 58px square at (3, 3);
// the pairing PIN prints right of the block (x=66). Never shrink the quiet zone.
namespace qrdisplay {
constexpr int kModules = 21;
constexpr size_t kQrLen = 63;
constexpr int kScale = 2;
constexpr int kQuietPx = 8;
constexpr int kPanelW = 128;
constexpr int kPanelH = 64;
constexpr int kExtentPx = kModules * kScale + 2 * kQuietPx;
constexpr int kX0 = 3;
constexpr int kY0 = (kPanelH - kExtentPx) / 2;

constexpr int kPinX = 66;
constexpr int kPinY = 28;
constexpr int kPinCols = 6;
constexpr int kPinCharW = 6;
constexpr int kPinCharH = 8;
constexpr int kPinW = kPinCols * kPinCharW;
constexpr int kPinH = kPinCharH;

static_assert(kX0 >= 0 && kY0 >= 0 && kX0 + kExtentPx <= kPanelW && kY0 + kExtentPx <= kPanelH,
              "qr block must fit the panel");
static_assert(kPinX >= 0 && kPinY >= 0 && kPinX + kPinW <= kPanelW && kPinY + kPinH <= kPanelH,
              "pin line must fit the panel");
static_assert(kPinX >= kX0 + kExtentPx, "pin line must sit right of the qr block");

inline bool fitsPanel() { return kX0 >= 0 && kY0 >= 0 && kX0 + kExtentPx <= kPanelW && kY0 + kExtentPx <= kPanelH; }

inline bool fitsPinLine() {
  return kPinX >= 0 && kPinY >= 0 && kPinX + kPinW <= kPanelW && kPinY + kPinH <= kPanelH && kPinX >= kX0 + kExtentPx;
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
