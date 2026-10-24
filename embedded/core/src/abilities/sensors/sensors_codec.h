#pragma once
// Sensors codec — abilities/sensors.
//
// Arduino-free pure pieces, 100% native-testable: fusion math
// (SensorSnapshot, FusionMounting, kBoardMounting, Fusion::evaluate) plus the
// sensor edge helpers (namespace sensortask). Siblings: sensors_manager.h owns
// the production Fusion instance; sensors_task.h is the RTOS binder.

// Sensor fusion — Arduino-independent pure logic (stdint/math only).
// Shared by firmware (invoked on-demand by the sensor task) and host tests.
// No heap, no Arduino headers.
//
// Sign convention (defined on the post-remap logical frame):
//   +pitch = nose-up, +roll = right-side-down.
// Orientation is an explicit input (FusionMounting), never ambient config:
// tests construct their own mountings; production owns one instance built
// from kBoardMounting.
//
// Formulas (accelerometer-only, degrees):
//   pitch = atan2(fax, hypot(fay, faz))
//   roll  = atan2(fay, hypot(fax, faz))
// where (fax, fay, faz) is raw accel (g) after the mounting remap.
//
// Rules:
//   - Tilt always updates pitch/roll when the MPU is healthy (no thresholds;
//     computed for telemetry/behavior only).
//   - Ground/cliff per TCRT rail (fwd + rev): each rail's threshold +
//     hysteresis verdict feeds its ground bit, gated by the total-tilt rule:
//     level ⟺ (pitch²+roll²) < 35² from freshly fused pitch/roll, evaluated
//     only when mpuHealthy. Past 35° the gate abstains (bits hold); a dead
//     rail holds too (never clears).
//   - Health gating: an unhealthy source skips its own updates and retains
//     last state. A dead sensor never clears flags.
//   - distanceMM tracks the forward ToF only and retains its last value on
//     invalid ToF. It is obstacle distance and plays NO part in the
//     ground/cliff derivation.
//   - gndFwd/gndRev are the gated rail verdicts; cliffDetected is the raw
//     forward-TCRT flag. isPickedUp is never touched here.

#include <math.h>
#include <stdint.h>

#include "config.h"
#include "system_context.h"

namespace fusion {

// Raw sensor snapshot feeding evaluateFusion. Accel in g, gyro in dps
// (gyro reserved for a future complementary filter — carried, not yet fused).
struct SensorSnapshot {
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  uint16_t tofMm = 0;
  bool tofValid = false;
  bool mpuHealthy = false;
  bool tcrtFwdGround = false;
  bool tcrtFwdValid = false;  // default invalid: disabled/unpolled rail holds, never clears
  bool tcrtRevGround = false;
  bool tcrtRevValid = false;  // default invalid: disabled/unpolled rail holds, never clears
};

// Threshold fallbacks keep this header compilable standalone (host g++
// without -I include).
#ifndef CFG_LEVEL_MAX_TILT_DEG
#define CFG_LEVEL_MAX_TILT_DEG 35.0f
#endif
#ifndef CFG_FUSION_SWAP_AXAY
#define CFG_FUSION_SWAP_AXAY 0
#endif

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

// Explicit mounting: 90deg XY swap applied to raw accel before tilt math.
// Passed by value into Fusion so orientation is always a deliberate input.
struct FusionMounting {
  bool swapXY = false;
};

// Canonical production mounting — single source is CFG_FUSION_SWAP_AXAY.
constexpr FusionMounting kBoardMounting{CFG_FUSION_SWAP_AXAY != 0};

// Stateless fusion engine: pure math over (snapshot, mounting) -> state.
// No heap, safe to call from any task context.
class Fusion {
 public:
  explicit constexpr Fusion(FusionMounting mounting) : mounting_(mounting) {}

  void evaluate(const SensorSnapshot& snap, SystemState& state) const {
    if (snap.mpuHealthy) {
      // Mounting remap first: optional 90deg XY swap.
      const float fax = mounting_.swapXY ? snap.ay : snap.ax;
      const float fay = mounting_.swapXY ? snap.ax : snap.ay;
      const float faz = snap.az;
      state.pitch = atan2f(fax, sqrtf(fay * fay + faz * faz)) * kRad2Deg;
      state.roll = atan2f(fay, sqrtf(fax * fax + faz * faz)) * kRad2Deg;
    }
    if (snap.tofValid) {
      state.distanceMM = snap.tofMm;
    }
    // The forward ToF is obstacle distance only — it never feeds the ground
    // derivation.
    if (snap.mpuHealthy && (snap.tcrtFwdValid || snap.tcrtRevValid)) {
      const float tilt2 = state.pitch * state.pitch + state.roll * state.roll;
      const float gate = static_cast<float>(CFG_LEVEL_MAX_TILT_DEG);
      const bool level = tilt2 < gate * gate;
      if (level) {
        if (snap.tcrtFwdValid) {
          state.gndFwd = snap.tcrtFwdGround;
          state.cliffDetected = !snap.tcrtFwdGround;  // raw fwd flag: telemetry/snapshot/wire untouched
        }
        if (snap.tcrtRevValid) {
          state.gndRev = snap.tcrtRevGround;
        }
      }
      // Tilted (or MPU-dead, above): abstain — dead rails hold their ground
      // bits (never clear); isPickedUp untouched.
    }
  }

 private:
  FusionMounting mounting_;
};

}  // namespace fusion

// Sensor task top half — Arduino-free pure logic. Host tests include this
// header directly.
//
// ToF sentinel for "no reading yet" (mirrors the driver constant; duplicated
// so this namespace stays driver-free). Obstacle hysteresis band +
// wrap-safe elapsed scheduling + transitions-only edges: see per-function
// contracts below.

namespace sensortask {

// ToF sentinel for "no reading yet" (mirrors the driver constant).
constexpr uint16_t kNoReadingMm = 9999;

// Obstacle hysteresis band around CFG_OBSTACLE_MM: the assert edge fires
// below (OBSTACLE - HYST), the latch silently re-arms above
// (OBSTACLE + HYST).
constexpr int16_t kObstacleHystMm = 10;

// Threshold fallback keeps this header compilable standalone (host g++
// without -I include). In-project, include/config.h always wins.
#ifndef CFG_OBSTACLE_MM
#define CFG_OBSTACLE_MM 80
#endif

// ToF sample is usable only while powered and holding a real ranging.
inline bool tofReadingValid(bool enabled, uint16_t distMm) { return enabled && distMm != kNoReadingMm; }

// Wrap-safe elapsed gate: fires (and re-arms) once per intervalMs using
// unsigned subtraction.
inline bool due(uint32_t nowMs, uint32_t& lastMs, uint32_t intervalMs) {
  if (nowMs - lastMs >= intervalMs) {
    lastMs = nowMs;
    return true;
  }
  return false;
}

// Transitions-only cliff edge: dead ticks never evaluate and never publish;
// live ticks publish once per level flip. Boots clear, so boot-into-cliff
// fires. Legacy single-rail helper (firmware uses groundTransition).
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
// publish; live ticks publish the packed ground payload once per flip on
// EITHER rail. Boots with ground present (true,true).
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

// Edge-triggered obstacle assert/clear: dead ticks never evaluate; a clear
// latch fires ONCE when a live ToF reads below (CFG_OBSTACLE_MM - HYST) with
// payload = distance mm. The latch re-arms with ONE clear publication when
// the reading rises above (CFG_OBSTACLE_MM + HYST), payload = distance mm
// at the clear edge. Held ticks on either side stay silent.
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
    payloadOut = distMm;
    return true;
  }
  return false;
}

}  // namespace sensortask
