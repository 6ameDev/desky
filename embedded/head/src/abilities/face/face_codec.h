#pragma once
// desky-head face ability — pure display contracts (Arduino-free, host-tested).
//
// Ability layer: face_codec (pure serialization/geometry, 100% native-testable).
// Moved verbatim from middleware/face_renderer.h (Task 3 split): the QR show
// mode geometry + payload contract, the pairing show mode state, and the OTA
// progress mode geometry + state. No ARDUINO, no GFX — host Unity tests pin
// every constant and transition here.

#include <stddef.h>
#include <stdint.h>

#include "head_context.h"
#include "link/qr_codec.h"

// ── QR show mode geometry + payload contract (Arduino-free, host-tested) ─
// QR symbol: Version 1, 21x21 modules. The link QRBITMAP payload is 63B
// row-major MSB-first (3 packed bytes/row); kQrLen pins the same 63 here
// and the host tests cross-check it against linkcode::kQrBitmapLen, so the
// frozen link contract stays single-sourced.
// Render: 2px/module + spec 4-module quiet zone per side (8px @2px; 16px
// total) -> 21*2+16 = 58px square. The block sits LEFT on the 128x64 panel
// (origin (3, 3), right edge 61) so the 6-digit pairing PIN renders beside
// it in the default 5x7 font (6px/char -> 36px at x=66, vertically centered
// at y=28). The full quiet zone is load-bearing for phone scanners;
// never shrink it to gain margins.
namespace qrdisplay {
constexpr int kModules = 21;
constexpr size_t kQrLen = 63;
constexpr int kScale = 2;
constexpr int kQuietPx = 8;
constexpr int kPanelW = 128;
constexpr int kPanelH = 64;
constexpr int kExtentPx = kModules * kScale + 2 * kQuietPx;  // 58
constexpr int kX0 = 3;                                       // left block (PIN beside it)
constexpr int kY0 = (kPanelH - kExtentPx) / 2;               // 3
// Pairing PIN text line (default font, 6px/char x 8px line): "123456" spans
// 36px at x=66..102, y=28..36 — right of the QR block (gap 61->66 = 5px).
constexpr int kPinX = 66;
constexpr int kPinY = 28;
constexpr int kPinCols = 6;
constexpr int kPinCharW = 6;
constexpr int kPinCharH = 8;
constexpr int kPinW = kPinCols * kPinCharW;  // 36
constexpr int kPinH = kPinCharH;             // 8

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

// Module bit, MSB-first row-major: byte = y*3 + x/8, bit = 7-(x%8).
// Null/out-of-range -> 0 (fail-closed, never crashes the call site).
inline uint8_t moduleAt(const uint8_t* qr63, int x, int y) {
  if (qr63 == nullptr || x < 0 || x >= kModules || y < 0 || y >= kModules) {
    return 0;
  }
  const uint8_t b = qr63[static_cast<size_t>(y) * 3u + static_cast<size_t>(x >> 3)];
  return static_cast<uint8_t>((b >> (7 - (x & 7))) & 1u);
}

}  // namespace qrdisplay

// ── Pairing show mode state (authoritative-PIN display, Arduino-free) ──────
// The core sends the 6 digits + validity window (PAIRSHOW) and the head
// renders them: the QR bitmap is re-derived LOCALLY via qr::encode6Digits
// into the existing qrBuf_ staging path (no bitmap rides the wire), and the
// digits print beside it. Deadline = show-time + validSecs*1000 (u32 wrap
// natural); the render task clears on expiry. Re-arm on every fresh show.
// Single overlay rivalry: pairing wins over demo-QR/OTA and vice versa —
// the show paths enforce at most one live overlay.
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

// Wrap-safe expiry: true once now has reached/passed the deadline.
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

// ── OTA progress mode geometry + state (Arduino-free, host-tested) ─────────
// Carrier-triggered pull display: a minimal fill bar (no text), then an
// alert face on failure. The bar is centered on the 128x64 panel (same panel
// size as qrdisplay — referenced, never redefined): 100x10 outline + 1px
// border, inner fill grows with pct. Any FACE command clears the mode and a
// fail/timeout falls back to the stored face (the QR show/clear house rule).
namespace otadisplay {
constexpr int kBarX0 = 14;
constexpr int kBarY0 = 27;
constexpr int kBarW = 100;
constexpr int kBarH = 10;

inline bool fitsPanel() {
  return kBarX0 >= 0 && kBarY0 >= 0 && kBarX0 + kBarW <= qrdisplay::kPanelW && kBarY0 + kBarH <= qrdisplay::kPanelH;
}

// Inner fill width for pct 0..100 (clamped): 2px border each side.
inline int fillW(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  return (kBarW - 4) * pct / 100;
}

// Overlay state: inactive by default; show() arms (or refreshes) the bar,
// markFailed() flips a live bar to the alert face, hide() clears to the
// stored face. Failed-without-show still shows (any updater failure lands on
// the alert face, never silently).
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
