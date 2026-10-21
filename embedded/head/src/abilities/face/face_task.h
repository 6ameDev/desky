#pragma once
// desky-head face ability — RTOS render binding (firmware only).
//
// Ability layer: face_task (RTOS task + event binder gluing the overlay state
// machine to the panel). Split from middleware/face_renderer.h (Task 3): the
// FaceRenderer shell keeps its name, Core 0 render task params, 25/5fps
// cadence, present/enabled gating, and LOG lines; overlay decisions delegate
// to face_manager.h and geometry to face_codec.h.
//
// Power: IActuator bool gate (OFF = panel off + render gated) plus setDim()
// for the DIM matrix row (low refresh + low contrast). OFF gates everything;
// a face id set while off is stored and rendered on next ON.

#ifdef ARDUINO
// ── Firmware: face renderer ─────────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#include "abilities/face/face_manager.h"
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
    FaceTransition t;
    mgr_.showFace(id, t);
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    if (t.clearedQr) {
      LOG_I("FACE", "qr cleared (face)");
    }
    if (t.clearedPair) {
      LOG_I("FACE", "pairing cleared (face)");
    }
    if (t.clearedOta) {
      LOG_I("FACE", "ota cleared (face)");
    }
  }

  uint8_t faceId() const { return mgr_.faceId(); }

  // QR show mode: renders 21x21 modules from the 63B staging buffer until a
  // FACE command arrives or CFG_FACE_QR_TIMEOUT_MS elapses
  // (FACE-command-or-120s-timeout). Respects the present()/enabled_/dim_
  // gates exactly like drawFace: the bitmap is still stored and paints on
  // the next repaint. No heap/String; the 63B staging buffer is a manager
  // member (BSS, never stack).
  // (Historical: Slice 6c filled this buffer from the wire QRBITMAP via the
  // link; the flag-day removal dropped that trigger — the buffer is now
  // filled by the USB `qr` demo and the local PairShow re-derive below.)
  void showQr(const uint8_t qr63[qrdisplay::kQrLen]) {
    FaceTransition t;
    if (!mgr_.showQr(qr63, millis(), t)) {
      return;
    }
    if (t.clearedOta) {
      LOG_I("FACE", "ota cleared (qr)");
    }
    if (t.clearedPair) {
      LOG_I("FACE", "pairing cleared (qr)");
    }
    if (panel_.present() && enabled_) {
      drawQr();
    }
    LOG_I("FACE", "qr shown len=%u", static_cast<unsigned>(qrdisplay::kQrLen));
  }

  // Bench demo pattern (deterministic border + X, NOT a scannable QR):
  // exercises the QR show/clear path from the USB CLI with no link.
  void showQrDemo() {
    FaceTransition t;
    mgr_.showQrDemo(millis(), t);
    if (t.clearedOta) {
      LOG_I("FACE", "ota cleared (qr)");
    }
    if (t.clearedPair) {
      LOG_I("FACE", "pairing cleared (qr)");
    }
    if (panel_.present() && enabled_) {
      drawQr();
    }
    LOG_I("FACE", "qr shown len=%u", static_cast<unsigned>(qrdisplay::kQrLen));
  }

  // Clears the QR show mode (timeout/CLI path). Also clears a live pairing
  // show (single overlay rivalry — `qr clear` never leaves a stale PIN up).
  // No-op when neither is up, so only real transitions log. Repaints the
  // stored face when live.
  void clearQr(const char* why = "cli") {
    FaceTransition t;
    if (!mgr_.clearQr(t)) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    if (t.clearedPair && !t.clearedQr) {
      LOG_I("FACE", "pairing cleared (%s)", why != nullptr ? why : "cli");
    } else {
      LOG_I("FACE", "qr cleared (%s)", why != nullptr ? why : "cli");
    }
  }

  bool qrActive() const { return mgr_.qrActive(); }

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
    FaceTransition t;
    if (!mgr_.showPairing(pin6, validSecs, millis(), t)) {
      return;
    }
    if (t.clearedOta) {
      LOG_I("FACE", "ota cleared (pairing)");
    }
    if (t.clearedQr) {
      LOG_I("FACE", "qr cleared (pairing)");
    }
    if (panel_.present() && enabled_) {
      drawQr();
    }
    LOG_I("FACE", "pairing shown valid=%us", static_cast<unsigned>(validSecs));
  }

  // Clears the pairing show mode (PAIRDONE/timeout/CLI path). Also clears a
  // live demo QR (single overlay — no stale bitmap survives pairing
  // completion). No-op when neither is up. Repaints the stored face when
  // live.
  void clearPairing(const char* why = "cli") {
    FaceTransition t;
    if (!mgr_.clearPairing(t)) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    LOG_I("FACE", "pairing cleared (%s)", why != nullptr ? why : "cli");
  }

  bool pairActive() const { return mgr_.pairActive(); }

  // OTA progress mode (carrier trigger): the updater drives this — showOta
  // arms/refreshes the fill bar (FETCH: fill proportion, VERIFY/COMMIT:
  // full bar), failOta flips to the alert face, hideOta clears to the stored
  // face on success. Any FACE command or the timeout clears (same house rule
  // as QR). Minimal bar, NO text rendering. Respects the present()/enabled_
  // gates exactly like drawFace/drawQr.
  void showOta(uint8_t pct) {
    FaceTransition t;
    mgr_.showOta(pct, millis(), t);
    if (t.clearedQr) {
      LOG_I("FACE", "qr cleared (ota)");
    }
    if (t.clearedPair) {
      LOG_I("FACE", "pairing cleared (ota)");
    }
    if (panel_.present() && enabled_) {
      drawOtaBar();
    }
    LOG_I("FACE", "ota progress pct=%u", static_cast<unsigned>(mgr_.otaPct()));
  }

  // Updater success: clear to the stored face (the reboot follows).
  void hideOta() {
    if (!mgr_.hideOta()) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    LOG_I("FACE", "ota done");
  }

  // Updater abort/failure: alert face now, timeout back to the stored face.
  void failOta() {
    mgr_.failOta(millis());
    if (panel_.present() && enabled_) {
      drawFace(FACE_ALERT);
    }
    LOG_W("FACE", "ota failed (alert)");
  }

  bool otaActive() const { return mgr_.otaActive(); }
  bool otaFailed() const { return mgr_.otaFailed(); }
  uint8_t otaPct() const { return mgr_.otaPct(); }

  // Clears the OTA mode (timeout path). No-op when no OTA is up, so only
  // real transitions log. Repaints the stored face when live.
  void clearOta(const char* why = "cli") {
    if (!mgr_.clearOta()) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
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
    switch (mgr_.liveMode()) {
      case FaceLiveMode::kOtaAlert:
        drawFace(FACE_ALERT);
        break;
      case FaceLiveMode::kOtaBar:
        drawOtaBar();
        break;
      case FaceLiveMode::kQr:
        drawQr();
        break;
      case FaceLiveMode::kFace:
      default:
        drawFace(mgr_.faceId());
        break;
    }
  }

  // DIM matrix row: low refresh (task reads dim each loop) + low contrast.
  void setDim(bool dim) {
    mgr_.setDim(dim);
    if (panel_.present() && enabled_) {
      panel_.setContrast(dim ? Ssd1306Display::kContrastDim : Ssd1306Display::kContrastOn);
    }
  }

  bool isDim() const { return mgr_.isDim(); }

 private:
  static void renderTask(void* arg) {
    auto* self = static_cast<FaceRenderer*>(arg);
    for (;;) {
      // Never WDT-subscribed, so no feed needed; never halts.
      if (self->enabled_ && self->panel_.present()) {
        if (self->mgr_.otaExpired(millis())) {
          self->clearOta("timeout");  // Wrap-safe elapsed gate; logs the clear.
        } else if (self->mgr_.otaActive()) {
          if (self->mgr_.otaFailed()) {
            self->drawFace(FACE_ALERT);
          } else {
            self->drawOtaBar();
          }
        } else if (self->mgr_.pairExpired(millis())) {
          self->clearPairing("timeout");  // Wrap-safe deadline gate; logs the clear.
        } else if (self->mgr_.pairActive()) {
          self->drawQr();
        } else if (self->mgr_.qrExpired(millis())) {
          self->clearQr("timeout");  // Wrap-safe elapsed gate; logs the clear.
        } else if (self->mgr_.qrActive()) {
          self->drawQr();
        } else {
          self->drawFace(self->mgr_.faceId());
        }
      }
      const uint32_t periodMs = 1000u / (self->mgr_.isDim() ? CFG_FACE_FPS_DIM : CFG_FACE_FPS);
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
    const int fill = otadisplay::fillW(mgr_.otaPct());
    if (fill > 0) {
      display.fillRect(otadisplay::kBarX0 + 2, otadisplay::kBarY0 + 2, fill, otadisplay::kBarH - 4, SSD1306_WHITE);
    }
    display.display();
  }

  // QR paint: cleared panel, SSD1306_WHITE 2px squares for dark modules at
  // the left qrdisplay origin (quiet zone = cleared background) plus the
  // 6-digit pairing PIN in the default font right of the block (only while
  // a pairing show is live — the bitmap-only demo paints the block alone).
  // Gated on present() like drawFace (absent OLED on-MB is normal, never
  // assert); the caller also gates on enabled_, and dim_ only changes
  // cadence + contrast via setDim, never the pixels here.
  void drawQr() {
    if (!panel_.present()) {
      return;
    }
    auto& display = panel_.gfx();
    const uint8_t* qrBuf = mgr_.qrBuf();
    display.clearDisplay();
    for (int y = 0; y < qrdisplay::kModules; ++y) {
      for (int x = 0; x < qrdisplay::kModules; ++x) {
        if (qrdisplay::moduleAt(qrBuf, x, y) != 0) {
          display.fillRect(qrdisplay::kX0 + x * qrdisplay::kScale, qrdisplay::kY0 + y * qrdisplay::kScale,
                           qrdisplay::kScale, qrdisplay::kScale, SSD1306_WHITE);
        }
      }
    }
    if (mgr_.pairActive()) {
      const uint8_t* pairPin = mgr_.pairPin();
      char pin[pairdisplay::kPinLen + 1];
      for (size_t i = 0; i < pairdisplay::kPinLen; ++i) {
        pin[i] = static_cast<char>(pairPin[i]);
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
  FaceOverlayManager mgr_;
  volatile bool enabled_ = false;
};

#endif  // ARDUINO
