#pragma once
// Sensors task — abilities/sensors.
//
// Firmware-only SensorTask binder: Core 1 RTOS task, HAL polling, EventBus
// publish — no sockets involved. Pure pieces live in sensors_codec.h, the
// shared production Fusion home in sensors_manager.h.

#include "sensors_manager.h"

#ifdef ARDUINO
// ── Firmware: sensor task (poll + fuse + publish, Core 1) ──────────────────

#include <Arduino.h>

#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/tcrt5000_driver.h"
#include "hal/vl53l0x_driver.h"
#include "services/event_bus.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
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
#ifndef CFG_TCRT_TARGET_INTERVAL_MS
#define CFG_TCRT_TARGET_INTERVAL_MS 20
#endif
#ifndef CFG_OBSTACLE_MM
#define CFG_OBSTACLE_MM 80
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
  static constexpr uint32_t kTcrtMs = CFG_TCRT_TARGET_INTERVAL_MS;

  SensorTask()
      : mpu_(nullptr),
        tof_(nullptr),
        tcrtFwd_(nullptr),
        tcrtRev_(nullptr),
        task_(nullptr),
        lastGndFwd_(true),
        lastGndRev_(true),
        lastObstacle_(false),
        lastMpuMs_(0),
        lastTofMs_(0),
        lastTcrtMs_(0) {}

  // Pins the sensor task to Core 1. Call once in setup(), with logging up.
  bool begin(Mpu6500Driver* mpu, Vl53l0xDriver* tof, Tcrt5000Driver* tcrtFwd, Tcrt5000Driver* tcrtRev) {
    DESKY_ASSERT(mpu != nullptr);
    DESKY_ASSERT(tof != nullptr);
    DESKY_ASSERT(tcrtFwd != nullptr);
    DESKY_ASSERT(tcrtRev != nullptr);
    if (mpu == nullptr || tof == nullptr || tcrtFwd == nullptr || tcrtRev == nullptr) {
      return false;
    }
    mpu_ = mpu;
    tof_ = tof;
    tcrtFwd_ = tcrtFwd;
    tcrtRev_ = tcrtRev;
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
    DESKY_ASSERT(tcrtFwd_ != nullptr);
    DESKY_ASSERT(tcrtRev_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts. (A halted WDT-subscribed task becomes a
    // panic-reboot loop.)
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();

      if (sensortask::due(nowMs, lastMpuMs_, kMpuMs)) {
        mpu_->update();
      }
      if (sensortask::due(nowMs, lastTofMs_, kTofMs)) {
        tof_->update();
      }
      if (sensortask::due(nowMs, lastTcrtMs_, kTcrtMs)) {
        tcrtFwd_->update();
        tcrtRev_->update();
      }

      // A ground tick needs the MPU plus at least one enabled TCRT rail; a
      // ToF tick needs a valid ranging. Either live rail runs the fusion;
      // dead ticks hold in silence.
      const uint16_t distMm = tof_->distanceMm();
      const bool tofLive = sensortask::tofReadingValid(tof_->isEnabled(), distMm);
      const bool groundLive = mpu_->isHealthy() && (tcrtFwd_->isEnabled() || tcrtRev_->isEnabled());
      bool gndFwdNow = lastGndFwd_;
      bool gndRevNow = lastGndRev_;
      float pitchDeg = 0.0f;
      if (groundLive || tofLive) {
        const MpuReading& r = mpu_->reading();
        fusion::SensorSnapshot snap;
        snap.ax = r.ax;
        snap.ay = r.ay;
        snap.az = r.az;
        snap.gx = r.gx;
        snap.gy = r.gy;
        snap.gz = r.gz;
        snap.tofMm = distMm;
        snap.tofValid = tofLive;
        snap.mpuHealthy = mpu_->isHealthy();
        snap.tcrtFwdGround = tcrtFwd_->groundPresent();
        snap.tcrtFwdValid = tcrtFwd_->isEnabled();
        snap.tcrtRevGround = tcrtRev_->groundPresent();
        snap.tcrtRevValid = tcrtRev_->isEnabled();
        // Seed with the last published levels: fusion is stateless and dead
        // rails hold (never clear).
        SystemState fused;
        fused.gndFwd = lastGndFwd_;
        fused.gndRev = lastGndRev_;
        fused.cliffDetected = !lastGndFwd_;
        fusion::Fusion{fusion::kBoardMounting}.evaluate(snap, fused);
        gndFwdNow = fused.gndFwd;
        gndRevNow = fused.gndRev;
        pitchDeg = fused.pitch;
      }
      uint32_t payload = 0;
      if (sensortask::groundTransition(groundLive, gndFwdNow, gndRevNow, lastGndFwd_, lastGndRev_, payload)) {
        EventBus::publish(EVENT_GROUND_CHANGED, payload);
        LOG_I("SENSOR", "CLIFF %s tcrtF=%u(%u) tcrtR=%u(%u) obs=%umm pitch=%+.1f gndFwd=%u gndRev=%u",
              gndFwdNow ? "cleared" : "asserted", tcrtFwd_->rawCount(), tcrtFwd_->groundPresent() ? 1u : 0u,
              tcrtRev_->rawCount(), tcrtRev_->groundPresent() ? 1u : 0u, distMm, pitchDeg, gndFwdNow ? 1u : 0u,
              gndRevNow ? 1u : 0u);
      }
      // Obstacle assert/clear edges (forward ToF only): the transition helper
      // fires once per edge; the pre-call latch tells assert from clear.
      uint32_t obsPayload = 0;
      const bool wasObstacle = lastObstacle_;
      if (sensortask::obstacleTransition(tofLive, distMm, lastObstacle_, obsPayload)) {
        if (wasObstacle) {
          EventBus::publish(EVENT_OBSTACLE_CLEARED, obsPayload);
          LOG_I("SENSOR", "OBSTACLE cleared dist=%umm thr=%umm", obsPayload, static_cast<unsigned>(CFG_OBSTACLE_MM));
        } else {
          EventBus::publish(EVENT_OBSTACLE_DETECTED, obsPayload);
          LOG_I("SENSOR", "OBSTACLE dist=%umm thr=%umm", obsPayload, static_cast<unsigned>(CFG_OBSTACLE_MM));
        }
      }

      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (60000 / kLoopMs) == 0) {
        Diagnostics::logWatermarks("SENSOR");
      }
    }
  }

  Mpu6500Driver* mpu_;
  Vl53l0xDriver* tof_;
  Tcrt5000Driver* tcrtFwd_;
  Tcrt5000Driver* tcrtRev_;
  TaskHandle_t task_;
  bool lastGndFwd_;
  bool lastGndRev_;
  bool lastObstacle_;
  uint32_t lastMpuMs_;
  uint32_t lastTofMs_;
  uint32_t lastTcrtMs_;
};

#endif  // ARDUINO
