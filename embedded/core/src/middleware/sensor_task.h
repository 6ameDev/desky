#pragma once
// desky v2 sensor task — header-only, owns sensor polling + ground/obstacle publish.
//
// Polls the MPU (20ms), ToF (100ms), and both TCRT rails (20ms) on one 15ms
// Core 1 tick via wrap-safe per-sensor elapsed scheduling; driver update()
// calls skip non-blocking on bus contention and hold last-good inside the
// drivers. Fusion runs on live ticks only: a ground tick needs the MPU plus
// at least one enabled TCRT rail (dead ticks hold the last published level
// in silence so a dead source can never clear a latched ground bit); a ToF
// tick needs a valid ranging (enabled && != 9999 sentinel). Transitions
// publish EVENT_GROUND_CHANGED (packed gndFwd bit0 + gndRev bit1, on either
// rail's flip only, silence while held); the level boots with ground present
// so boot-into-void publishes once. The forward ToF is obstacle distance
// only (re-aimed forward, out of the ground path): its assert edge publishes
// EVENT_OBSTACLE_DETECTED (payload = distance mm, +/-10mm hysteresis,
// assert-edge only) which the coordinator explicitly ignores (no behavior
// action yet). isPickedUp is never touched (no pickup rule).
//
// Structure mirrors coordinator.h: the top half (namespace sensortask) is
// Arduino-free pure logic (stdint only) so host Unity tests include this
// header directly. The bottom half (class SensorTask) is firmware-only
// (#ifdef ARDUINO): a Core 1 publisher task.
//
// Rules: WDT fed every tick, Diagnostics::logWatermarks("SENSOR") every ~5s,
// task never halts, no heap/String in the loop.

#include <stdint.h>

#include "config.h"
#include "system_context.h"

namespace sensortask {

// ToF sentinel for "no reading yet" (mirrors Vl53l0xDriver::kNoReadingMm;
// duplicated so this namespace stays driver-free for host tests).
constexpr uint16_t kNoReadingMm = 9999;

// Obstacle hysteresis band around CFG_OBSTACLE_MM: the assert edge fires
// below (OBSTACLE - HYST), the latch silently re-arms above
// (OBSTACLE + HYST), so edge flicker never spams the bus.
constexpr int16_t kObstacleHystMm = 10;

// Threshold fallback keeps this header compilable standalone (host g++
// without -I include). In-project, include/config.h always wins.
#ifndef CFG_OBSTACLE_MM
#define CFG_OBSTACLE_MM 150
#endif

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
// Legacy single-rail helper (kept for host tests; firmware uses the unified
// groundTransition below).
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

// Transitions-only unified ground edge: dead ticks never evaluate and never
// publish (lastFwd/lastRev hold in silence); live ticks publish the packed
// ground payload once per flip on EITHER rail. Boots with ground present
// (true,true), so boot-into-void fires once.
inline bool groundTransition(bool liveTick, bool gndFwdNow, bool gndRevNow, bool& lastFwd, bool& lastRev,
                             uint32_t& payloadOut) {
  if (!liveTick) {
    return false;
  }
  if (gndFwdNow == lastFwd && gndRevNow == lastRev) {
    return false;
  }
  lastFwd = gndFwdNow;
  lastRev = gndRevNow;
  payloadOut = packGround(gndFwdNow, gndRevNow);
  return true;
}

// Edge-triggered obstacle assert: dead ticks never evaluate and never touch
// the latch; a clear latch fires ONCE when a live ToF reads below
// (CFG_OBSTACLE_MM - kObstacleHystMm) with payload = distance mm (see the
// EVENT_OBSTACLE_DETECTED contract in system_context.h). The latch silently
// re-arms above (CFG_OBSTACLE_MM + kObstacleHystMm); held-obstacle ticks
// stay silent. The clear edge never publishes (a future
// EVENT_OBSTACLE_CLEARED can add it); the coordinator ignores this event.
inline bool obstacleTransition(bool liveTick, uint16_t distMm, bool& asserted, uint32_t& payloadOut) {
  if (!liveTick) {
    return false;
  }
  const int32_t near = static_cast<int32_t>(CFG_OBSTACLE_MM) - static_cast<int32_t>(kObstacleHystMm);
  const int32_t far = static_cast<int32_t>(CFG_OBSTACLE_MM) + static_cast<int32_t>(kObstacleHystMm);
  const int32_t d = static_cast<int32_t>(distMm);
  if (!asserted && d < near) {
    asserted = true;
    payloadOut = distMm;
    return true;
  }
  if (asserted && d > far) {
    asserted = false;
  }
  return false;
}

}  // namespace sensortask

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
#include "middleware/sensor_fusion.h"
#include "services/event_bus.h"

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
#ifndef CFG_TCRT_TARGET_INTERVAL_MS
#define CFG_TCRT_TARGET_INTERVAL_MS 20
#endif
#ifndef CFG_OBSTACLE_MM
#define CFG_OBSTACLE_MM 150
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

  // Pins the sensor task to Core 1. Call once in setup(), after
  // Coordinator::begin (ground events have a subscriber from the first
  // publish; obstacle events are explicitly ignored by the coordinator — no
  // behavior action yet) and before UdpServer::begin, with logging already up.
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
      if (sensortask::due(nowMs, lastTcrtMs_, kTcrtMs)) {
        tcrtFwd_->update();
        tcrtRev_->update();
      }

      // Live gates: a ground tick needs the MPU plus at least one enabled
      // TCRT rail; a ToF tick needs a valid ranging. Either live rail makes
      // the fusion run; dead ticks hold in silence.
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
        // rails must hold (never clear), so a one-sided dead rail can never
        // synthesize a transition against its default.
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
      // Obstacle assert edge (forward ToF only): plumbed now, the
      // coordinator explicitly ignores it — no behavior action yet.
      uint32_t obsPayload = 0;
      if (sensortask::obstacleTransition(tofLive, distMm, lastObstacle_, obsPayload)) {
        EventBus::publish(EVENT_OBSTACLE_DETECTED, obsPayload);
        LOG_I("SENSOR", "OBSTACLE dist=%umm thr=%umm", obsPayload, static_cast<unsigned>(CFG_OBSTACLE_MM));
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
