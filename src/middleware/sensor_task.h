#pragma once
// desky v2 sensor task — header-only, owns sensor polling + cliff publish.
//
// Polls the MPU (20ms) and ToF (100ms) on one 15ms Core 1 tick via wrap-safe
// per-sensor elapsed scheduling; driver update() calls skip non-blocking on
// bus contention and hold last-good inside the drivers. Fusion runs on live
// ticks only (mpuHealthy && tofValid, tofValid = enabled && != 9999
// sentinel); dead ticks hold the last published level in silence so a dead
// source can never clear a latched cliff. Transitions publish
// EVENT_CLIFF_DETECTED (nonzero rising, zero falling, silence while held);
// the level boots clear so boot-into-cliff publishes once. isPickedUp is
// never touched (no pickup rule).
//
// Structure mirrors coordinator.h: the top half (namespace sensortask) is
// Arduino-free pure logic (stdint only) so host Unity tests include this
// header directly. The bottom half (class SensorTask) is firmware-only
// (#ifdef ARDUINO): a Core 1 publisher task.
//
// Rules: WDT fed every tick, Diagnostics::logWatermarks("SENSOR") every ~5s,
// task never halts, no heap/String in the loop.

#include <stdint.h>

namespace sensortask {

// ToF sentinel for "no reading yet" (mirrors Vl53l0xDriver::kNoReadingMm;
// duplicated so this namespace stays driver-free for host tests).
constexpr uint16_t kNoReadingMm = 9999;

// ToF sample is usable only while powered and holding a real ranging.
inline bool tofReadingValid(bool enabled, uint16_t distMm) { return enabled && distMm != kNoReadingMm; }

// Wrap-safe elapsed gate: fires (and re-arms) once per intervalMs using
// unsigned subtraction, so the ~49-day millis() rollover stays correct.
inline bool due(uint32_t nowMs, uint32_t& lastMs, uint32_t intervalMs) {
  if (nowMs - lastMs >= intervalMs) {
    lastMs = nowMs;
    return true;
  }
  return false;
}

// Transitions-only cliff edge: dead ticks never evaluate and never publish
// (lastPublished holds in silence); live ticks publish once per level flip
// (nonzero rising / zero falling). Boots clear, so boot-into-cliff fires.
inline bool cliffTransition(bool liveTick, bool cliffNow, bool& lastPublished, uint32_t& payloadOut) {
  if (!liveTick) {
    return false;
  }
  if (cliffNow == lastPublished) {
    return false;
  }
  lastPublished = cliffNow;
  payloadOut = cliffNow ? 1u : 0u;
  return true;
}

}  // namespace sensortask

#ifdef ARDUINO
// ── Firmware: sensor task (poll + fuse + publish, Core 1) ──────────────────

#include <Arduino.h>

#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "middleware/sensor_fusion.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/logger.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as coordinator.h).
#ifndef CFG_SENSOR_STACK_WORDS
#define CFG_SENSOR_STACK_WORDS 4096
#endif
#ifndef CFG_SENSOR_PRIORITY_OFFSET
#define CFG_SENSOR_PRIORITY_OFFSET 3
#endif
#ifndef CFG_SENSOR_CORE
#define CFG_SENSOR_CORE 1
#endif
#ifndef CFG_SENSOR_LOOP_MS
#define CFG_SENSOR_LOOP_MS 15
#endif
#ifndef CFG_MPU_TARGET_INTERVAL_MS
#define CFG_MPU_TARGET_INTERVAL_MS 20
#endif
#ifndef CFG_TOF_TARGET_INTERVAL_MS
#define CFG_TOF_TARGET_INTERVAL_MS 100
#endif

class SensorTask {
 public:
  static constexpr uint32_t kStackWords = CFG_SENSOR_STACK_WORDS;
  // Below Coordinator +4 / Motion +5 so sensing never preempts control.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_SENSOR_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_SENSOR_CORE;
  static constexpr uint32_t kLoopMs = CFG_SENSOR_LOOP_MS;
  static constexpr uint32_t kMpuMs = CFG_MPU_TARGET_INTERVAL_MS;
  static constexpr uint32_t kTofMs = CFG_TOF_TARGET_INTERVAL_MS;

  SensorTask() : mpu_(nullptr), tof_(nullptr), task_(nullptr), lastPublished_(false), lastMpuMs_(0), lastTofMs_(0) {}

  // Pins the sensor task to Core 1. Call once in setup(), after
  // Coordinator::begin (cliff events have a subscriber from the first
  // publish) and before UdpServer::begin, with logging already up.
  bool begin(Mpu6500Driver* mpu, Vl53l0xDriver* tof) {
    DESKY_ASSERT(mpu != nullptr);
    DESKY_ASSERT(tof != nullptr);
    if (mpu == nullptr || tof == nullptr) {
      return false;
    }
    mpu_ = mpu;
    tof_ = tof;
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&SensorTask::taskEntry, "sensor", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

 private:
  static void taskEntry(void* arg) { static_cast<SensorTask*>(arg)->loop(); }

  void loop() {
    DESKY_ASSERT(mpu_ != nullptr);
    DESKY_ASSERT(tof_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts by design: a halted task that stays
    // WDT-subscribed becomes a panic-reboot loop (see coordinator.h).
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();

      if (sensortask::due(nowMs, lastMpuMs_, kMpuMs)) {
        mpu_->update();
      }
      if (sensortask::due(nowMs, lastTofMs_, kTofMs)) {
        tof_->update();
      }

      // Live gate: both sources must be live or the tick holds in silence.
      const uint16_t distMm = tof_->distanceMm();
      const bool live = mpu_->isHealthy() && sensortask::tofReadingValid(tof_->isEnabled(), distMm);
      bool cliffNow = false;
      float azG = 0.0f;
      if (live) {
        const MpuReading& r = mpu_->reading();
        azG = r.az;
        fusion::SensorSnapshot snap;
        snap.ax = r.ax;
        snap.ay = r.ay;
        snap.az = r.az;
        snap.gx = r.gx;
        snap.gy = r.gy;
        snap.gz = r.gz;
        snap.tofMm = distMm;
        snap.tofValid = true;
        snap.mpuHealthy = true;
        SystemState fused;
        fusion::Fusion{fusion::kBoardMounting}.evaluate(snap, fused);
        cliffNow = fused.cliffDetected;
      }
      uint32_t payload = 0;
      if (sensortask::cliffTransition(live, cliffNow, lastPublished_, payload)) {
        EventBus::publish(EVENT_CLIFF_DETECTED, payload);
        LOG_I("SENSOR", "CLIFF %s dist=%umm az=%.2fg", cliffNow ? "asserted" : "cleared", distMm, azG);
      }

      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000 / kLoopMs) == 0) {
        Diagnostics::logWatermarks("SENSOR");
      }
    }
  }

  Mpu6500Driver* mpu_;
  Vl53l0xDriver* tof_;
  TaskHandle_t task_;
  bool lastPublished_;
  uint32_t lastMpuMs_;
  uint32_t lastTofMs_;
};

#endif  // ARDUINO
