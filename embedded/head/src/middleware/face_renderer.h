#pragma once
// desky-head face renderer — header-only middleware, owns face semantics.
//
// SHOW_FACE{id} API, procedural face art, and the ~25fps render task live
// HERE, not in the HAL: the panel driver (hal/ssd1306_display.h) exposes
// primitives only. Face vocabulary (FaceId, names, clamp) lives Arduino-free
// in include/head_context.h so host Unity tests cover it; art here is
// procedural GFX primitives (placeholder faces — the API, not the pixels,
// is the contract).
//
// Power: IActuator bool gate (OFF = panel off + render gated) plus setDim()
// for the DIM matrix row (low refresh + low contrast). OFF gates everything;
// a face id set while off is stored and rendered on next ON.

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
// Single overlay rivalry: pairing wins over legacy-QR/OTA and vice versa —
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

#ifdef ARDUINO
// ── Firmware: face renderer ─────────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#include "common/iactuator.h"
#include "common/logger.h"
#include "config.h"
#include "hal/ssd1306_display.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_FACE_FPS
#define CFG_FACE_FPS 25
#endif
#ifndef CFG_FACE_FPS_DIM
#define CFG_FACE_FPS_DIM 5
#endif
#ifndef CFG_FACE_STACK_WORDS
#define CFG_FACE_STACK_WORDS 4096
#endif
#ifndef CFG_FACE_PRIORITY_OFFSET
#define CFG_FACE_PRIORITY_OFFSET 2
#endif
#ifndef CFG_FACE_CORE
#define CFG_FACE_CORE 0
#endif
#ifndef CFG_FACE_QR_TIMEOUT_MS
#define CFG_FACE_QR_TIMEOUT_MS 120000
#endif
#ifndef CFG_FACE_OTA_TIMEOUT_MS
#define CFG_FACE_OTA_TIMEOUT_MS 120000
#endif

class FaceRenderer : public IActuator {
 public:
  explicit FaceRenderer(Ssd1306Display& panel) : panel_(panel) {}

  // Panel init owns the absent-OLED policy (never asserts); the renderer
  // then paints the banner + boot face and starts the render task.
  bool init() override {
    if (!panel_.init()) {
      return false;
    }
    enabled_ = true;
    if (panel_.present()) {
      panel_.gfx().setTextSize(1);
      panel_.gfx().setTextColor(SSD1306_WHITE);
      panel_.gfx().setCursor(0, 0);
      panel_.gfx().print(F("desky-head"));
      panel_.gfx().display();
      showFace(FACE_BOOT);
      xTaskCreatePinnedToCore(&FaceRenderer::renderTask, "face", CFG_FACE_STACK_WORDS, this,
                              tskIDLE_PRIORITY + CFG_FACE_PRIORITY_OFFSET, nullptr, CFG_FACE_CORE);
    }
    return true;
  }

  void setPowerState(bool enable) override {
    enabled_ = enable;
    panel_.setPowerState(enable);
    if (enable) {
      repaint();
    }
  }

  bool isEnabled() const override { return enabled_; }

  bool present() const { return panel_.present(); }

  // SHOW_FACE{id} API: stores (clamped) and paints immediately when live;
  // the render task repaints at the matrix cadence either way. Any FACE
  // command clears an active QR show mode, an active pairing show mode, and
  // an active OTA progress mode (same house rule — one live overlay at a
  // time).
  void showFace(uint8_t id) {
    const bool wasQr = qrActive_;
    const bool wasPair = pairActive_;
    const bool wasOta = ota_.active;
    qrActive_ = false;
    pairActive_ = false;
    otadisplay::hide(ota_);
    faceId_ = clampFace(static_cast<int>(id));
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
    if (wasQr) {
      LOG_I("FACE", "qr cleared (face)");
    }
    if (wasPair) {
      LOG_I("FACE", "pairing cleared (face)");
    }
    if (wasOta) {
      LOG_I("FACE", "ota cleared (face)");
    }
  }

  uint8_t faceId() const { return faceId_; }

  // QR show mode (H2.5): renders the link QRBITMAP's 21x21 modules until a
  // FACE command arrives or CFG_FACE_QR_TIMEOUT_MS elapses
  // (FACE-command-or-120s-timeout). Respects the present()/enabled_/dim_
  // gates exactly like drawFace: absent OLED or a disabled panel gates the
  // paint (the bitmap is still stored and paints on the next repaint). No
  // heap/String; the 63B staging buffer is a member (BSS, never stack).
  // Reserved wire type (superseded by the PAIRSHOW/PIN path below): old
  // firmwares' bitmaps still display; never removed.
  void showQr(const uint8_t qr63[qrdisplay::kQrLen]) {
    if (qr63 == nullptr) {
      return;
    }
    for (size_t i = 0; i < qrdisplay::kQrLen; ++i) {
      qrBuf_[i] = qr63[i];
    }
    if (ota_.active) {
      otadisplay::hide(ota_);  // one live overlay: QR wins over OTA here
      LOG_I("FACE", "ota cleared (qr)");
    }
    if (pairActive_) {
      pairActive_ = false;  // one live overlay: legacy QR wins over pairing
      LOG_I("FACE", "pairing cleared (qr)");
    }
    qrActive_ = true;
    qrShownMs_ = millis();
    if (panel_.present() && enabled_) {
      drawQr();
    }
    LOG_I("FACE", "qr shown len=%u", static_cast<unsigned>(qrdisplay::kQrLen));
  }

  // Bench demo pattern (deterministic border + X, NOT a scannable QR):
  // exercises the QR show/clear path from the USB CLI with no link.
  void showQrDemo() {
    uint8_t demo[qrdisplay::kQrLen] = {};
    for (int y = 0; y < qrdisplay::kModules; ++y) {
      for (int x = 0; x < qrdisplay::kModules; ++x) {
        const bool edge = (x < 2 || y < 2 || x >= qrdisplay::kModules - 2 || y >= qrdisplay::kModules - 2);
        const bool cross = (x == y) || (x + y == qrdisplay::kModules - 1);
        if (edge || cross) {
          demo[static_cast<size_t>(y) * 3u + static_cast<size_t>(x >> 3)] |= static_cast<uint8_t>(1u << (7 - (x & 7)));
        }
      }
    }
    showQr(demo);
  }

  // Clears the QR show mode (timeout/CLI path). Also clears a live pairing
  // show (single overlay rivalry — `qr clear` never leaves a stale PIN up).
  // No-op when neither is up, so only real transitions log. Repaints the
  // stored face when live.
  void clearQr(const char* why = "cli") {
    const bool wasQr = qrActive_;
    const bool wasPair = pairActive_;
    if (!wasQr && !wasPair) {
      return;
    }
    qrActive_ = false;
    pairActive_ = false;
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
    if (wasPair && !wasQr) {
      LOG_I("FACE", "pairing cleared (%s)", why != nullptr ? why : "cli");
    } else {
      LOG_I("FACE", "qr cleared (%s)", why != nullptr ? why : "cli");
    }
  }

  bool qrActive() const { return qrActive_; }

  // Pairing show mode (Pre-2): renders the authoritative PIN from a PAIRSHOW
  // frame — the QR bitmap is re-derived LOCALLY via qr::encode6Digits into
  // the existing qrBuf_ staging path (no bitmap rides the wire) and the
  // digits print beside it — until PAIRDONE, a FACE command, or the
  // valid-seconds deadline elapses (deadline = show-time + validSecs*1000,
  // re-armed on every fresh show). Fail-closed: a null/bad PIN changes no
  // display state (the link already validated, so encode cannot fail — the
  // guard is defense in depth). Respects the present()/enabled_ gates exactly
  // like showQr.
  void showPairing(const uint8_t pin6[pairdisplay::kPinLen], uint16_t validSecs) {
    if (!pairdisplay::isValidPin(pin6)) {
      return;
    }
    uint8_t modules[qr::kModules];
    if (!qr::encode6Digits(pin6, modules)) {
      return;  // impossible for validated digits: fail closed, no display change
    }
    uint8_t packed[qr::kPackedLen];
    qr::packBitmap(modules, packed);
    for (size_t i = 0; i < qrdisplay::kQrLen; ++i) {
      qrBuf_[i] = packed[i];
    }
    for (size_t i = 0; i < pairdisplay::kPinLen; ++i) {
      pairPin_[i] = pin6[i];
    }
    if (ota_.active) {
      otadisplay::hide(ota_);  // one live overlay: pairing wins over OTA here
      LOG_I("FACE", "ota cleared (pairing)");
    }
    if (qrActive_) {
      qrActive_ = false;  // one live overlay: pairing wins over legacy QR
      LOG_I("FACE", "qr cleared (pairing)");
    }
    pairActive_ = true;
    pairDeadlineMs_ = pairdisplay::deadlineFor(millis(), validSecs);
    if (panel_.present() && enabled_) {
      drawQr();
    }
    LOG_I("FACE", "pairing shown valid=%us", static_cast<unsigned>(validSecs));
  }

  // Clears the pairing show mode (PAIRDONE/timeout/CLI path). Also clears a
  // live legacy QR (single overlay — no stale bitmap survives pairing
  // completion). No-op when neither is up. Repaints the stored face when
  // live.
  void clearPairing(const char* why = "cli") {
    const bool wasPair = pairActive_;
    const bool wasQr = qrActive_;
    if (!wasPair && !wasQr) {
      return;
    }
    pairActive_ = false;
    qrActive_ = false;
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
    LOG_I("FACE", "pairing cleared (%s)", why != nullptr ? why : "cli");
  }

  bool pairActive() const { return pairActive_; }

  // OTA progress mode (carrier trigger): the updater drives this — showOta
  // arms/refreshes the fill bar (FETCH: fill proportion, VERIFY/COMMIT:
  // full bar), failOta flips to the alert face, hideOta clears to the stored
  // face on success. Any FACE command or the timeout clears (same house rule
  // as QR). Minimal bar, NO text rendering. Respects the present()/enabled_
  // gates exactly like drawFace/drawQr.
  void showOta(uint8_t pct) {
    if (qrActive_) {
      qrActive_ = false;  // one live overlay: OTA wins over QR
      LOG_I("FACE", "qr cleared (ota)");
    }
    if (pairActive_) {
      pairActive_ = false;  // one live overlay: OTA wins over pairing
      LOG_I("FACE", "pairing cleared (ota)");
    }
    otadisplay::show(ota_, pct);
    otaMs_ = millis();
    if (panel_.present() && enabled_) {
      drawOtaBar();
    }
    LOG_I("FACE", "ota progress pct=%u", static_cast<unsigned>(ota_.pct));
  }

  // Updater success: clear to the stored face (the reboot follows).
  void hideOta() {
    if (!ota_.active) {
      return;
    }
    otadisplay::hide(ota_);
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
    LOG_I("FACE", "ota done");
  }

  // Updater abort/failure: alert face now, timeout back to the stored face.
  void failOta() {
    otadisplay::markFailed(ota_);
    otaMs_ = millis();
    if (panel_.present() && enabled_) {
      drawFace(FACE_ALERT);
    }
    LOG_W("FACE", "ota failed (alert)");
  }

  bool otaActive() const { return ota_.active; }
  bool otaFailed() const { return ota_.failed; }
  uint8_t otaPct() const { return ota_.pct; }

  // Clears the OTA mode (timeout path). No-op when no OTA is up, so only
  // real transitions log. Repaints the stored face when live.
  void clearOta(const char* why = "cli") {
    if (!ota_.active) {
      return;
    }
    otadisplay::hide(ota_);
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
    LOG_I("FACE", "ota cleared (%s)", why != nullptr ? why : "cli");
  }

  // Repaints the live mode (OTA bar/alert, else pairing/QR when active, else
  // the stored face) — used when power/DIM returns without changing the face
  // id, so an overlay survives ON/DIM while any FACE command still clears it.
  void repaint() {
    if (!panel_.present() || !enabled_) {
      return;
    }
    if (ota_.active) {
      if (ota_.failed) {
        drawFace(FACE_ALERT);
      } else {
        drawOtaBar();
      }
    } else if (qrActive_ || pairActive_) {
      drawQr();
    } else {
      drawFace(faceId_);
    }
  }

  // DIM matrix row: low refresh (task reads dim_ each loop) + low contrast.
  void setDim(bool dim) {
    dim_ = dim;
    if (panel_.present() && enabled_) {
      panel_.setContrast(dim ? Ssd1306Display::kContrastDim : Ssd1306Display::kContrastOn);
    }
  }

  bool isDim() const { return dim_; }

 private:
  static void renderTask(void* arg) {
    auto* self = static_cast<FaceRenderer*>(arg);
    for (;;) {
      // Never WDT-subscribed, so no feed needed; never halts.
      if (self->enabled_ && self->panel_.present()) {
        if (self->ota_.active && (millis() - self->otaMs_ >= static_cast<uint32_t>(CFG_FACE_OTA_TIMEOUT_MS))) {
          self->clearOta("timeout");  // Wrap-safe elapsed gate; logs the clear.
        } else if (self->ota_.active) {
          if (self->ota_.failed) {
            self->drawFace(FACE_ALERT);
          } else {
            self->drawOtaBar();
          }
        } else if (self->pairActive_ && pairdisplay::isExpired(millis(), self->pairDeadlineMs_)) {
          self->clearPairing("timeout");  // Wrap-safe deadline gate; logs the clear.
        } else if (self->pairActive_) {
          self->drawQr();
        } else if (self->qrActive_ && (millis() - self->qrShownMs_ >= static_cast<uint32_t>(CFG_FACE_QR_TIMEOUT_MS))) {
          self->clearQr("timeout");  // Wrap-safe elapsed gate; logs the clear.
        } else if (self->qrActive_) {
          self->drawQr();
        } else {
          self->drawFace(self->faceId_);
        }
      }
      const uint32_t periodMs = 1000u / (self->dim_ ? CFG_FACE_FPS_DIM : CFG_FACE_FPS);
      vTaskDelay(pdMS_TO_TICKS(periodMs));
    }
  }

  void drawFace(uint8_t id) {
    if (!panel_.present()) {
      return;
    }
    auto& display = panel_.gfx();
    display.clearDisplay();
    switch (id) {
      case FACE_HAPPY:
        display.fillCircle(44, 24, 6, SSD1306_WHITE);
        display.fillCircle(84, 24, 6, SSD1306_WHITE);
        arc(display, 64, 38, 12, 25, 155);  // smile
        break;
      case FACE_SAD:
        display.fillCircle(44, 24, 4, SSD1306_WHITE);
        display.fillCircle(84, 24, 4, SSD1306_WHITE);
        arc(display, 64, 58, 12, 205, 335);  // frown
        break;
      case FACE_BLINK:
        display.drawLine(38, 24, 50, 24, SSD1306_WHITE);
        display.drawLine(78, 24, 90, 24, SSD1306_WHITE);
        display.drawLine(56, 48, 72, 48, SSD1306_WHITE);
        break;
      case FACE_ALERT:
        display.drawCircle(44, 24, 8, SSD1306_WHITE);
        display.drawCircle(84, 24, 8, SSD1306_WHITE);
        display.fillCircle(44, 24, 2, SSD1306_WHITE);
        display.fillCircle(84, 24, 2, SSD1306_WHITE);
        display.drawCircle(64, 48, 5, SSD1306_WHITE);
        break;
      case FACE_BOOT:
      default:
        display.fillRoundRect(36, 16, 16, 16, 3, SSD1306_WHITE);
        display.fillRoundRect(76, 16, 16, 16, 3, SSD1306_WHITE);
        display.drawLine(54, 48, 74, 48, SSD1306_WHITE);
        break;
    }
    display.display();
  }

  // OTA progress paint: cleared panel, 1px outline + fill-proportion inner
  // bar (no text — counters ride the serial logs, never the pixels). Gated
  // on present() like drawFace (absent OLED on-MB is normal, never assert);
  // the caller also gates on enabled_, and dim_ only changes cadence +
  // contrast via setDim, never the pixels here.
  void drawOtaBar() {
    if (!panel_.present()) {
      return;
    }
    auto& display = panel_.gfx();
    display.clearDisplay();
    display.drawRect(otadisplay::kBarX0, otadisplay::kBarY0, otadisplay::kBarW, otadisplay::kBarH, SSD1306_WHITE);
    const int fill = otadisplay::fillW(ota_.pct);
    if (fill > 0) {
      display.fillRect(otadisplay::kBarX0 + 2, otadisplay::kBarY0 + 2, fill, otadisplay::kBarH - 4, SSD1306_WHITE);
    }
    display.display();
  }

  // QR paint: cleared panel, SSD1306_WHITE 2px squares for dark modules at
  // the left qrdisplay origin (quiet zone = cleared background) plus the
  // 6-digit pairing PIN in the default font right of the block (only while
  // a pairing show is live — legacy QRBITMAP frames paint the block alone).
  // Gated on present() like drawFace (absent OLED on-MB is normal, never
  // assert); the caller also gates on enabled_, and dim_ only changes
  // cadence + contrast via setDim, never the pixels here.
  void drawQr() {
    if (!panel_.present()) {
      return;
    }
    auto& display = panel_.gfx();
    display.clearDisplay();
    for (int y = 0; y < qrdisplay::kModules; ++y) {
      for (int x = 0; x < qrdisplay::kModules; ++x) {
        if (qrdisplay::moduleAt(qrBuf_, x, y) != 0) {
          display.fillRect(qrdisplay::kX0 + x * qrdisplay::kScale, qrdisplay::kY0 + y * qrdisplay::kScale,
                           qrdisplay::kScale, qrdisplay::kScale, SSD1306_WHITE);
        }
      }
    }
    if (pairActive_) {
      char pin[pairdisplay::kPinLen + 1];
      for (size_t i = 0; i < pairdisplay::kPinLen; ++i) {
        pin[i] = static_cast<char>(pairPin_[i]);
      }
      pin[pairdisplay::kPinLen] = '\0';
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(qrdisplay::kPinX, qrdisplay::kPinY);
      display.print(pin);
    }
    display.display();
  }

  // Pixel arc for smile/frown (degrees, 0 = +x, 90 = +y down the panel).
  static void arc(Adafruit_SSD1306& display, int cx, int cy, int r, int a0, int a1) {
    for (int a = a0; a <= a1; a += 5) {
      const float rad = static_cast<float>(a) * 3.14159265f / 180.0f;
      display.drawPixel(cx + static_cast<int>(r * cosf(rad)), cy + static_cast<int>(r * sinf(rad)), SSD1306_WHITE);
    }
  }

  Ssd1306Display& panel_;
  volatile bool enabled_ = false;
  volatile bool dim_ = false;
  volatile uint8_t faceId_ = FACE_BOOT;
  // QR show mode state (H2.5): 63B staging buffer + active flag + show
  // timestamp for the FACE-command-or-120s-timeout rule. Set from the loop
  // task (link/CLI handoff), read by the Core 0 render task — single
  // volatile flag + 32-bit timestamp, benign tear-tolerant like faceId_.
  // Reserved path (superseded by pairing below): kept so old firmwares'
  // bitmaps still display.
  volatile bool qrActive_ = false;
  uint8_t qrBuf_[qrdisplay::kQrLen] = {};
  volatile uint32_t qrShownMs_ = 0;
  // Pairing show mode state (Pre-2): authoritative PIN + active flag +
  // show-deadline (show-time + validSecs*1000) for the PAIRDONE-or-FACE-or-
  // deadline rule. Same loop-task-set / render-task-read shape as the QR
  // state above. The bitmap re-derives into qrBuf_ at show time, so the two
  // flags are never live together (single overlay rivalry).
  volatile bool pairActive_ = false;
  uint8_t pairPin_[pairdisplay::kPinLen] = {};
  volatile uint32_t pairDeadlineMs_ = 0;
  // OTA progress mode state (carrier trigger): overlay state + last-update
  // timestamp for the FACE-command-or-timeout rule. Set from the loop task
  // (updater progress hooks), read by the Core 0 render task — same
  // tear-tolerant shape as the QR state above.
  otadisplay::Progress ota_;
  volatile uint32_t otaMs_ = 0;
};

#endif  // ARDUINO
