#pragma once
// desky v2 VL53L0X driver (behind ISensor) — header-only HAL.
//
// Ports the PROVEN V1 ToF path (main:src/main.cpp) via the pinned Adafruit
// lib: XSHUT-gated init (MCU_TOF_XSHUT, plain OUTPUT HIGH + boot delay),
// polling rangingTest, V1 validity gate (err NONE + RangeStatus != 4 +
// range <= CFG_TOF_VALID_MAX_MM), hold-last-good on bad reads, and BOTH
// recovery tiers (soft: SCL-pulse + reinit, no XSHUT; hard: XSHUT power
// cycle + pulse + reinit).
// V1-vs-new deltas:
// - Recovery used to be a manual web request (takeTofRecoveryRequest);
//   with no coordinator wired yet, update() self-recovers when the fault
//   trips (soft, then hard), then holds last-good and re-tries on the next
//   trip instead of hammering XSHUT every cycle.
// - Fault threshold is CFG_TOF_MAX_CONSEC_ERRORS (5, V1's value).
//
// Power: REAL, via XSHUT (LOW = shutdown, HIGH + boot delay + re-begin =
// on). Units: distanceMm() returns mm.
//
// Rules: no String/heap in update(), no hardcoded pins/addresses, mutex
// never held across delays (XSHUT waits run lock-free; init runs in setup
// before tasks start, so it takes no lock at all).

#include <Arduino.h>

#include "Adafruit_VL53L0X.h"
#include "config.h"
#include "hal/isensor.h"
#include "services/i2c_manager.h"
#include "services/logger.h"

class Vl53l0xDriver : public ISensor {
 public:
  static constexpr uint16_t kNoReadingMm = 9999;  // V1 lastGoodDist sentinel

  Vl53l0xDriver(I2CManager& bus, int xshutPin = MCU_TOF_XSHUT)
      : bus_(bus), xshutPin_(xshutPin), enabled_(false), lastGoodMm_(kNoReadingMm), consecErrors_(0) {}

  // XSHUT HIGH -> boot wait -> begin -> trial ranging must validate.
  // Returns false (no halt here); setup() applies DESKY_ASSERT policy.
  bool init() override {
    pinMode(xshutPin_, OUTPUT);
    digitalWrite(xshutPin_, HIGH);
    LOG_I("TOF", "XSHUT HIGH on pin %d, booting", xshutPin_);
    delay(CFG_TOF_XSHUT_BOOT_MS);
    if (!beginSensor()) {
      LOG_E("TOF", "VL53L0X not found at 0x%02X", MCU_ADDR_TOF);
      return false;
    }
    VL53L0X_RangingMeasurementData_t trial{};
    if (!readingValid(trial, lox_.rangingTest(&trial, false))) {
      LOG_E("TOF", "begin ok but trial ranging invalid");
      return false;
    }
    lastGoodMm_ = trial.RangeMilliMeter;
    enabled_ = true;
    LOG_I("TOF", "VL53L0X ready, trial=%dmm", lastGoodMm_);
    return true;
  }

  // Polling read at the caller's cadence (target: 10Hz). Bad reads hold
  // last-good; the fault trip triggers soft-then-hard recovery inline.
  void update() override {
    if (!enabled_) {
      return;
    }
    VL53L0X_RangingMeasurementData_t measure{};
    if (!bus_.acquire()) {
      return;  // Bus busy (OLED holds ~23ms) — skip, keep last-good.
    }
    const VL53L0X_Error err = lox_.rangingTest(&measure, false);
    bus_.release();
    if (readingValid(measure, err)) {
      consecErrors_ = 0;
      lastGoodMm_ = measure.RangeMilliMeter;
      return;
    }
    ++consecErrors_;
    if (consecErrors_ >= CFG_TOF_MAX_CONSEC_ERRORS) {
      consecErrors_ = 0;  // Re-arm: next streak re-tries, no XSHUT hammering.
      if (!recoverSoft()) {
        recoverHard();
      }
    }
  }

  // Real power via XSHUT. Re-enable re-boots and re-begins the sensor.
  void setPowerState(bool enable) override {
    if (enable == enabled_) {
      return;
    }
    if (!enable) {
      digitalWrite(xshutPin_, LOW);
      enabled_ = false;
      LOG_I("TOF", "XSHUT LOW (shutdown)");
      return;
    }
    digitalWrite(xshutPin_, HIGH);
    delay(CFG_TOF_XSHUT_BOOT_MS);
    enabled_ = beginSensor();
    LOG_I("TOF", "XSHUT HIGH (wake %s)", enabled_ ? "ok" : "FAILED");
  }

  bool isEnabled() const override { return enabled_; }
  uint32_t getTargetIntervalMs() const override { return CFG_TOF_TARGET_INTERVAL_MS; }

  uint16_t distanceMm() const { return lastGoodMm_; }
  bool fault() const { return consecErrors_ >= CFG_TOF_MAX_CONSEC_ERRORS; }

  // Tier 1 (V1 tofRecoverSoft): SCL pulse + bus reinit, no XSHUT.
  bool recoverSoft() {
    LOG_W("TOF", "soft recovery (SCL pulse + reinit, no XSHUT)");
    bus_.pulseClock();
    bus_.reinit();
    return beginSensor() && trialUpdate();
  }

  // Tier 2 (V1 tofRecoverHard): XSHUT power cycle + pulse + reinit.
  bool recoverHard() {
    LOG_W("TOF", "hard recovery (XSHUT cycle)");
    digitalWrite(xshutPin_, LOW);
    delay(CFG_TOF_XSHUT_SHUTDOWN_MS);
    bus_.pulseClock();
    digitalWrite(xshutPin_, HIGH);
    delay(CFG_TOF_XSHUT_BOOT_MS);
    bus_.reinit();
    return beginSensor() && trialUpdate();
  }

 private:
  // V1 tofReadingValid, unchanged semantics.
  static bool readingValid(const VL53L0X_RangingMeasurementData_t& measure, VL53L0X_Error err) {
    return err == VL53L0X_ERROR_NONE && measure.RangeStatus != 4 && measure.RangeMilliMeter <= CFG_TOF_VALID_MAX_MM;
  }

  bool beginSensor() { return lox_.begin(MCU_ADDR_TOF, false, &bus_.wire()); }

  // V1 tofReinit's trial gate: re-begin is not enough, ranging must work.
  bool trialUpdate() {
    VL53L0X_RangingMeasurementData_t trial{};
    if (!readingValid(trial, lox_.rangingTest(&trial, false))) {
      LOG_W("TOF", "reinit ok but trial reading invalid");
      return false;
    }
    lastGoodMm_ = trial.RangeMilliMeter;
    LOG_I("TOF", "sensor back online (%dmm)", lastGoodMm_);
    return true;
  }

  I2CManager& bus_;
  Adafruit_VL53L0X lox_;
  int xshutPin_;
  bool enabled_;
  uint16_t lastGoodMm_;
  int consecErrors_;
};
