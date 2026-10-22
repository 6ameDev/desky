#pragma once
// Face render binding (firmware only): RTOS task gluing the overlay state
// machine to the panel. OFF gates everything; setDim() selects the DIM row.

#ifdef ARDUINO
// ── Firmware: face renderer ─────────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#include "abilities/face/face_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/ssd1306_display.h"
#include "services/display_port.h"

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

class FaceRenderer : public IDisplayPort {
 public:
  explicit FaceRenderer(Ssd1306Display& panel) : panel_(panel) {}

  // Absent OLED never asserts; paints banner + boot face, starts the render task.
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

  // Stores (clamped), paints when live. Clears every live overlay.
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

  // Renders the 63B staging buffer until a FACE command or timeout.
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

  // Demo pattern (not a scannable QR): exercises show/clear with no link.
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

  // Clears QR + pairing show modes. No-op when idle; repaints when live.
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

  // Renders the authoritative PIN until PAIRDONE, a FACE command, or the
  // valid-seconds deadline. Fail-closed on null/bad PIN.
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

  // Clears pairing + demo QR modes. No-op when idle; repaints when live.
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

  // Single overlay home: the pairing policy attaches to this same instance.
  FaceOverlayManager& overlays() { return mgr_; }
  const FaceOverlayManager& overlays() const { return mgr_; }

  // Minimal bar, no text. Updater drives this (show/fail/hide).
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

  void hideOta() {
    if (!mgr_.hideOta()) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    LOG_I("FACE", "ota done");
  }

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

  // No-op when no OTA is up; repaints when live.
  void clearOta(const char* why = "cli") {
    if (!mgr_.clearOta()) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawFace(mgr_.faceId());
    }
    LOG_I("FACE", "ota cleared (%s)", why != nullptr ? why : "cli");
  }

  // Repaints the live mode (overlay survives ON/DIM; FACE clears it).
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
      // Never WDT-subscribed: no feed needed.
      if (self->enabled_ && self->panel_.present()) {
        if (self->mgr_.otaExpired(millis())) {
          self->clearOta("timeout");
        } else if (self->mgr_.otaActive()) {
          if (self->mgr_.otaFailed()) {
            self->drawFace(FACE_ALERT);
          } else {
            self->drawOtaBar();
          }
        } else if (self->mgr_.pairExpired(millis())) {
          self->clearPairing("timeout");
        } else if (self->mgr_.pairActive()) {
          self->drawQr();
        } else if (self->mgr_.qrExpired(millis())) {
          self->clearQr("timeout");
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
        arc(display, 64, 38, 12, 25, 155);
        break;
      case FACE_SAD:
        display.fillCircle(44, 24, 4, SSD1306_WHITE);
        display.fillCircle(84, 24, 4, SSD1306_WHITE);
        arc(display, 64, 58, 12, 205, 335);
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

  // Fill bar, no text. Gated on present() like drawFace.
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

  // Dark modules as 2px squares + PIN while a pairing show is live.
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
