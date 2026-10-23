#pragma once
// Display render binding (firmware only): RTOS task gluing the overlay state
// machine to the panel. OFF gates everything; setDim() selects the DIM row.

#ifdef ARDUINO
// ── Firmware: display renderer ─────────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#include "abilities/display/display_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/ssd1306_display.h"
#include "services/display_port.h"

#ifndef CFG_DISPLAY_FPS
#define CFG_DISPLAY_FPS 25
#endif
#ifndef CFG_DISPLAY_FPS_DIM
#define CFG_DISPLAY_FPS_DIM 5
#endif
#ifndef CFG_DISPLAY_STACK_WORDS
#define CFG_DISPLAY_STACK_WORDS 4096
#endif
#ifndef CFG_DISPLAY_PRIORITY_OFFSET
#define CFG_DISPLAY_PRIORITY_OFFSET 2
#endif
#ifndef CFG_DISPLAY_CORE
#define CFG_DISPLAY_CORE 0
#endif

class DisplayRenderer : public IDisplayPort {
 public:
  explicit DisplayRenderer(Ssd1306Display& panel) : panel_(panel) {}

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
      showEyes(FACE_BOOT);
      xTaskCreatePinnedToCore(&DisplayRenderer::renderTask, "display", CFG_DISPLAY_STACK_WORDS, this,
                              tskIDLE_PRIORITY + CFG_DISPLAY_PRIORITY_OFFSET, nullptr, CFG_DISPLAY_CORE);
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
  void showEyes(uint8_t id) {
    DisplayTransition t;
    mgr_.showEyes(id, t);
    if (panel_.present() && enabled_) {
      drawEyes(mgr_.eyesId());
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

  uint8_t eyesId() const { return mgr_.eyesId(); }

  // Renders the 63B staging buffer until a FACE command or timeout.
  void showQr(const uint8_t qr63[qrdisplay::kQrLen]) {
    DisplayTransition t;
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
    DisplayTransition t;
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
    DisplayTransition t;
    if (!mgr_.clearQr(t)) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawEyes(mgr_.eyesId());
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
    DisplayTransition t;
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
      lastPairSec_ = mgr_.remainingSecs(millis());
      wasPairActive_ = true;
    }
    LOG_I("FACE", "pairing shown valid=%us", static_cast<unsigned>(validSecs));
  }

  // Clears pairing + demo QR modes. No-op when idle; repaints when live.
  void clearPairing(const char* why = "cli") {
    DisplayTransition t;
    if (!mgr_.clearPairing(t)) {
      return;
    }
    if (panel_.present() && enabled_) {
      drawEyes(mgr_.eyesId());
    }
    wasPairActive_ = false;
    lastPairSec_ = kNoPairSec;
    LOG_I("FACE", "pairing cleared (%s)", why != nullptr ? why : "cli");
  }

  bool pairActive() const { return mgr_.pairActive(); }

  // Single overlay home: the pairing policy attaches to this same instance.
  DisplayManager& overlays() { return mgr_; }
  const DisplayManager& overlays() const { return mgr_; }

  // Minimal bar, no text. Updater drives this (show/fail/hide).
  void showOta(uint8_t pct) {
    DisplayTransition t;
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
      drawEyes(mgr_.eyesId());
    }
    LOG_I("FACE", "ota done");
  }

  void failOta() {
    mgr_.failOta(millis());
    if (panel_.present() && enabled_) {
      drawEyes(FACE_ALERT);
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
      drawEyes(mgr_.eyesId());
    }
    LOG_I("FACE", "ota cleared (%s)", why != nullptr ? why : "cli");
  }

  // Repaints the live mode (overlay survives ON/DIM; FACE clears it).
  void repaint() {
    if (!panel_.present() || !enabled_) {
      return;
    }
    switch (mgr_.liveMode()) {
      case DisplayLiveMode::kOtaAlert:
        drawEyes(FACE_ALERT);
        break;
      case DisplayLiveMode::kOtaBar:
        drawOtaBar();
        break;
      case DisplayLiveMode::kQr:
        drawQr();
        break;
      case DisplayLiveMode::kEyes:
      default:
        drawEyes(mgr_.eyesId());
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
    auto* self = static_cast<DisplayRenderer*>(arg);
    for (;;) {
      // Never WDT-subscribed: no feed needed.
      if (self->enabled_ && self->panel_.present()) {
        if (self->mgr_.otaExpired(millis())) {
          self->clearOta("timeout");
        } else if (self->mgr_.otaActive()) {
          if (self->mgr_.otaFailed()) {
            self->drawEyes(FACE_ALERT);
          } else {
            self->drawOtaBar();
          }
        } else if (self->mgr_.pairExpired(millis())) {
          self->clearPairing("timeout");
        } else if (self->mgr_.pairActive()) {
          // Full-frame I2C push per repaint: hold the pair screen to ~1Hz.
          const uint32_t nowMs = millis();
          if (!self->wasPairActive_ || self->mgr_.pairSecondTicked(nowMs, self->lastPairSec_)) {
            self->drawQr();
          }
          self->wasPairActive_ = true;
        } else {
          self->wasPairActive_ = false;
          self->lastPairSec_ = kNoPairSec;
          if (self->mgr_.qrExpired(millis())) {
            self->clearQr("timeout");
          } else if (self->mgr_.qrActive()) {
            self->drawQr();
          } else {
            self->drawEyes(self->mgr_.eyesId());
          }
        }
      }
      const uint32_t periodMs = 1000u / (self->mgr_.isDim() ? CFG_DISPLAY_FPS_DIM : CFG_DISPLAY_FPS);
      vTaskDelay(pdMS_TO_TICKS(periodMs));
    }
  }

  void drawEyes(uint8_t id) {
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

  // Fill bar, no text. Gated on present() like drawEyes.
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
      char countdown[pairdisplay::kCountdownBuf] = {};
      pairdisplay::formatCountdown(mgr_.remainingSecs(millis()), countdown, sizeof(countdown));
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(qrdisplay::kPinX, qrdisplay::kCodeY);
      display.print(F("CODE"));
      display.setCursor(qrdisplay::kPinX, qrdisplay::kPinY);
      display.print(pin);
      display.setCursor(qrdisplay::kPinX, qrdisplay::kCountdownY);
      display.print(countdown);
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
  DisplayManager mgr_;
  volatile bool enabled_ = false;
  static constexpr uint32_t kNoPairSec = 0xFFFFFFFFu;
  uint32_t lastPairSec_ = kNoPairSec;
  bool wasPairActive_ = false;
};

#endif  // ARDUINO
