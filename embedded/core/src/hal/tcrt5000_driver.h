#pragma once
// desky v2 TCRT5000 driver (behind ISensor) — header-only HAL.
//
// Two reflectance modules wired by A0 only (FRONT -> MCU_TCRT_FWD, REAR ->
// MCU_TCRT_REV, module VCC on 3V3, D0 unconnected): the analog A0 path with
// a firmware threshold + hysteresis, NOT the D0 comparator, so a future
// bare-LED swap (GND/VCC/analog) needs no firmware change.
//
// Polarity (BENCH-MEASURED 2026-10-05, do not re-guess): the module A0
// SINKS with reflection, so ground present <=> LOW counts. Measured bands
// (12-bit @11dB): close white ~167, far white / black desk ~210-220, open
// void ~1100. CFG_TCRT_THRESHOLD / CFG_TCRT_HYSTERESIS below encode that
// split (assert <= thr-hyst, clear >= thr+hyst); re-pin them if the floor
// material or ride height changes (log rawCount() over floor vs void).
//
// Structure mirrors sensor_task.h: the top half (namespace tcrt) is
// Arduino-free pure logic (stdint only) so host Unity tests include this
// header directly. The bottom half (class Tcrt5000Driver) is firmware-only
// (#ifdef ARDUINO): analogRead at 11dB attenuation (full ~3V3 swing on the
// 12-bit ADC), enable-flag power model (setPowerState gates update(), like
// the MPU driver — no hardware sleep pin exists).
//
// Rules: no String/heap in update(), no hardcoded pins (MCU_TCRT_* via
// config), init() failure DESKY_ASSERTs at the setup() call site.

// ── Arduino-free: hysteresis rail ─────────────────────────────────────
#include <stdint.h>

#include "common/isensor.h"
#include "config.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as sensor_fusion.h).
#ifndef CFG_TCRT_THRESHOLD
#define CFG_TCRT_THRESHOLD 1500
#endif
#ifndef CFG_TCRT_HYSTERESIS
#define CFG_TCRT_HYSTERESIS 200
#endif
#ifndef CFG_TCRT_TARGET_INTERVAL_MS
#define CFG_TCRT_TARGET_INTERVAL_MS 20
#endif
#ifndef CFG_TCRT_ASSERT_HIGH
#define CFG_TCRT_ASSERT_HIGH 0
#endif

namespace tcrt {

// Symmetric hysteresis rail around threshold (ADC counts). assertHigh=true
// (default): a cleared rail asserts ground at >= threshold+hysteresis, an
// asserted rail clears at <= threshold-hysteresis. assertHigh=false (our
// modules — A0 sinks with reflection): assert at <= threshold-hysteresis,
// clear at >= threshold+hysteresis. Everything between holds either way.
// Pure function — the driver feeds its latched level back in; host tests
// drive it directly.
inline bool railUpdate(bool level, uint16_t counts, uint16_t threshold, uint16_t hyst, bool assertHigh = true) {
  // int32 arithmetic: a hysteresis wider than the threshold must saturate,
  // never wrap the uint16 band edges (which would freeze the rail).
  const int32_t hi = static_cast<int32_t>(threshold) + static_cast<int32_t>(hyst);
  const int32_t lo = static_cast<int32_t>(threshold) - static_cast<int32_t>(hyst);
  const int32_t c = static_cast<int32_t>(counts);
  if (assertHigh) {
    if (!level && c >= hi) {
      return true;
    }
    if (level && c <= lo) {
      return false;
    }
    return level;
  }
  if (!level && c <= lo) {
    return true;
  }
  if (level && c >= hi) {
    return false;
  }
  return level;
}

}  // namespace tcrt

#ifdef ARDUINO
// ── Firmware: TCRT5000 reflectance driver (analog A0, Core 1 poll) ──────────

#include <Arduino.h>

#include "common/fault_manager.h"
#include "common/logger.h"

class Tcrt5000Driver : public ISensor {
 public:
  // MotorDriver-style constructor with CFG defaults: production passes the
  // MCU pin + CFG values explicitly (see main.cpp); tests/bench can pin
  // ad-hoc thresholds without touching config.h. assertHigh=false matches
  // our modules (A0 sinks with reflection: ground = LOW counts).
  Tcrt5000Driver(int adcPin, uint16_t thresholdCounts = CFG_TCRT_THRESHOLD,
                 uint16_t hysteresisCounts = CFG_TCRT_HYSTERESIS, bool assertHigh = CFG_TCRT_ASSERT_HIGH)
      : adcPin_(adcPin),
        threshold_(thresholdCounts),
        hysteresis_(hysteresisCounts),
        assertHigh_(assertHigh),
        enabled_(false),
        raw_(0),
        ground_(true) {}

  // No probe exists for a bare analog rail: arm 11dB attenuation, seed one
  // reading so the first fusion tick is live (not default), and report. The
  // level boots with ground present so boot-into-void publishes once (same
  // convention as the sensor task latch).
  bool init() override {
    analogSetPinAttenuation(static_cast<uint8_t>(adcPin_), ADC_11db);
    enabled_ = true;
    update();
    LOG_I("TCRT", "TCRT ready pin=%d thr=%u hyst=%u raw=%u gnd=%u", adcPin_, threshold_, hysteresis_, raw_,
          ground_ ? 1u : 0u);
    return true;
  }

  // Single non-blocking poll: analogRead the A0 rail and run the firmware
  // threshold + hysteresis. Skipped while power-gated (holds last level).
  void update() override {
    if (!enabled_) {
      return;
    }
    raw_ = static_cast<uint16_t>(analogRead(static_cast<uint8_t>(adcPin_)));
    ground_ = tcrt::railUpdate(ground_, raw_, threshold_, hysteresis_, assertHigh_);
  }

  // Enable-flag power only (no sleep pin on the module); gating update()
  // freezes the rail, and fusion treats a disabled driver as a dead rail
  // (holds last state, never clears).
  void setPowerState(bool enable) override { enabled_ = enable; }

  bool isEnabled() const override { return enabled_; }
  uint32_t getTargetIntervalMs() const override { return CFG_TCRT_TARGET_INTERVAL_MS; }

  // Firmware-threshold rail verdict (feeds fusion) + raw ADC counts (feeds
  // the SENSOR log lines for bench calibration).
  bool groundPresent() const { return ground_; }
  uint16_t rawCount() const { return raw_; }

 private:
  int adcPin_;
  uint16_t threshold_;
  uint16_t hysteresis_;
  bool assertHigh_;
  bool enabled_;
  uint16_t raw_;
  bool ground_;
};

#endif  // ARDUINO
