#pragma once
// SSD1306 panel driver (behind IActuator), header-only HAL: probe, on/off,
// clear, contrast, push, gfx accessor. No face vocabulary (see display_task.h).
// OLED can never attach during USB runs: init() scans {0x3C, 0x3D} and a
// "not found" result never blocks boot.

#include "head_context.h"

#ifdef ARDUINO
// ── Firmware: SSD1306 128x64 panel ──────────────────────────────────────────

#include <Arduino.h>
#include <U8g2lib.h>

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

  explicit Ssd1306Display(I2CManager& bus) : bus_(bus), u8g2_(U8G2_R0, U8X8_PIN_NONE, CFG_I2C_SCL, CFG_I2C_SDA) {}

  // Probe-then-claim. Never asserts: absent OLED is expected. Always returns true.
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
      // Expected on-MB: gate everything, boot continues.
      LOG_W("OLED", "not found at 0x3C/0x3D (expected on-MB) — display gated");
      present_ = false;
      return true;
    }
    u8g2_.setI2CAddress(static_cast<uint8_t>(found << 1));
    if (!u8g2_.begin()) {
      LOG_W("OLED", "begin failed at 0x%02X — display gated", found);
      present_ = false;
      return true;
    }
    // u8g2::begin re-attaches Wire on the ctor pins: re-assert the manager-owned clock/timeout.
    bus_.reinit();
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
    u8g2_.setPowerSave(enable ? 0 : 1);
  }

  bool isEnabled() const override { return enabled_; }

  bool present() const { return present_; }
  uint8_t address() const { return addr_; }

  void clear() {
    if (present_) {
      u8g2_.clearBuffer();
    }
  }

  void setContrast(uint8_t value) {
    if (present_) {
      u8g2_.setContrast(value);
    }
  }

  // Full-frame push under the shared-bus mutex: skip the frame when busy, panel keeps last-good.
  void push() {
    if (!present_) {
      return;
    }
    if (!bus_.acquire()) {
      return;
    }
    u8g2_.sendBuffer();
    bus_.release();
  }

  // Draw target for the renderer (display_task.h), the only caller.
  U8G2& gfx() { return u8g2_; }

 private:
  I2CManager& bus_;
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_;
  bool present_ = false;
  uint8_t addr_ = 0;
  volatile bool enabled_ = false;
};

#endif  // ARDUINO
