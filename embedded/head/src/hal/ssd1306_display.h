#pragma once
// desky-head SSD1306 panel driver (behind IActuator) — header-only HAL.
//
// Panel primitives ONLY: probe, on/off, clear, contrast, push, and a gfx()
// accessor for drawing. No face vocabulary, no render task, no usage
// knowledge — faces live one layer up in middleware/face_renderer.h. Named
// for the chip because it IS chip-bound (Adafruit_SSD1306 API,
// SSD1306_* commands); an SH1106 panel would land in a sibling file.
//
// MB-board rule: the OLED can NEVER be attached during USB runs, so init()
// SCANS {0x3C, 0x3D} (never hardcodes one) and a "not found" result logs
// cleanly and returns true WITHOUT blocking boot. Full panel validation is
// deferred to the field harness.

#include "head_context.h"

#ifdef ARDUINO
// ── Firmware: SSD1306 128x64 panel ──────────────────────────────────────────

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "common/i2c_manager.h"
#include "common/iactuator.h"
#include "common/logger.h"
#include "config.h"

class Ssd1306Display : public IActuator {
 public:
  static constexpr int kWidth = 128;
  static constexpr int kHeight = 64;
  static constexpr uint8_t kContrastOn = 0x80;
  static constexpr uint8_t kContrastDim = 0x10;

  explicit Ssd1306Display(I2CManager& bus) : bus_(bus), display_(kWidth, kHeight, &bus.wire()) {}

  // Probe-then-claim. NEVER asserts: absent OLED (every on-MB run) is an
  // expected configuration, not a fault. Returns true always; present()
  // reports whether the panel is live.
  bool init() override {
    const uint8_t candidates[2] = {MCU_ADDR_OLED_PRIMARY, MCU_ADDR_OLED_ALT};
    uint8_t found = 0;
    if (bus_.acquire(100)) {
      for (uint8_t i = 0; i < 2; ++i) {
        bus_.wire().beginTransmission(candidates[i]);
        if (bus_.wire().endTransmission() == 0) {
          found = candidates[i];
          break;
        }
      }
      bus_.release();
    } else {
      LOG_W("OLED", "scan skipped, bus busy");
    }
    if (found == 0) {
      // Expected on the MB USB board (it occupies all pins): log cleanly,
      // gate everything, boot continues.
      LOG_W("OLED", "not found at 0x3C/0x3D (expected on-MB) — display gated");
      present_ = false;
      return true;
    }
    if (!display_.begin(SSD1306_SWITCHCAPVCC, found)) {
      LOG_W("OLED", "begin failed at 0x%02X — display gated", found);
      present_ = false;
      return true;
    }
    addr_ = found;
    present_ = true;
    enabled_ = true;
    clear();
    push();
    LOG_I("OLED", "up at 0x%02X 128x64", addr_);
    return true;
  }

  void setPowerState(bool enable) override {
    if (!present_) {
      enabled_ = enable;  // Remembered; applies if a display ever appears.
      return;
    }
    enabled_ = enable;
    display_.ssd1306_command(enable ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
  }

  bool isEnabled() const override { return enabled_; }

  bool present() const { return present_; }
  uint8_t address() const { return addr_; }

  void clear() {
    if (present_) {
      display_.clearDisplay();
    }
  }

  void setContrast(uint8_t value) {
    if (present_) {
      display_.ssd1306_command(SSD1306_SETCONTRAST);
      display_.ssd1306_command(value);
    }
  }

  void push() {
    if (present_) {
      display_.display();
    }
  }

  // Draw target for the renderer (middleware/face_renderer.h) — the ONLY
  // caller. Raw GFX access stays inside this project (pinned lib_dep).
  Adafruit_SSD1306& gfx() { return display_; }

 private:
  I2CManager& bus_;
  Adafruit_SSD1306 display_;
  bool present_ = false;
  uint8_t addr_ = 0;
  volatile bool enabled_ = false;
};

#endif  // ARDUINO
