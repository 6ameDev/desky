#pragma once
// desky v2 sensors codec — abilities/sensors (ability layer).
//
// Arduino-free pure pieces, 100% native-testable: fusion math (SensorSnapshot,
// FusionMounting, kBoardMounting, Fusion::evaluate) plus the sensor edge
// helpers (namespace sensortask: tofReadingValid, due, cliffTransition,
// groundTransition, obstacleTransition, kObstacleHystMM). Moved verbatim from
// the former middleware fusion header + the Arduino-free top half of the
// former middleware sensor-task header (Slice 3); zero behavior change.
// Siblings: sensors_manager.h owns the production Fusion instance + a thin
// evaluate() entry point; sensors_task.h is the firmware-only RTOS binder.

// desky v2 sensor fusion — Arduino-independent pure logic (stdint/math only,
// like src/transport/udp_codec.h). Shared by firmware (invoked on-demand by
// the sensor task) and host Unity tests. No heap, no Arduino headers.
//
// Sign convention (defined on the post-remap logical frame, zero at level
// rest where measured ax=-0.026g, ay=+0.039g, az=-1.044g):
//   +pitch = nose-up (logical +X rises as the nose rises),
//   +roll  = right-side-down (logical +Y rises as the right side drops).
// Orientation is an explicit input (FusionMounting), never ambient config:
// tests construct their own mountings, production owns one instance built
// from kBoardMounting below. Changing the production mount never rewrites
// tests; it adds one (see test_board_mounting_matches_bench).
//
// Formulas (accelerometer-only, degrees):
//   pitch = atan2(fax, hypot(fay, faz))
//   roll  = atan2(fay, hypot(fax, faz))
// where (fax, fay, faz) is raw accel (g) after the mounting remap
// (optional 90deg XY swap). hypot/|.| forms use only the AZ magnitude.
//
// Rules:
//   - Tilt always updates pitch/roll when the MPU is healthy (no thresholds,
//     no picked-up flag — pitch/roll are computed for telemetry/behavior only).
//   - Ground/cliff per TCRT rail (fwd + rev): each rail's firmware
//     threshold + hysteresis verdict (see tcrt::railUpdate) feeds its ground
//     bit, gated by the pose-explicit total-tilt-magnitude rule: level ⟺
//     (pitch²+roll²) < 35² from freshly fused pitch/roll, evaluated only
//     when mpuHealthy. Past 35° total tilt the gate abstains (no ground
//     verdict — the bits hold); a dead rail holds too (never clears).
//   - Health gating: an unhealthy source skips its own updates and retains
//     last state — MPU unhealthy freezes pitch/roll, ToF invalid freezes
//     distanceMM, and each ground bit freezes unless its own rail (MPU + its
//     TCRT) is live. A dead sensor never clears flags.
//   - distanceMM tracks the forward ToF only and retains its last value on
//     invalid ToF. The ToF is re-aimed forward for obstacle/collision
//     detection, so distanceMM is now obstacle distance — it plays NO part
//     in the ground/cliff derivation (the old ToF-as-cliff rule and its
//     1/sin beam-depression compensation are deleted).
//   - gndFwd is the gated forward-TCRT verdict; gndRev is the gated
//     rear-TCRT verdict. cliffDetected is the raw forward-TCRT flag
//     (telemetry/snapshot/wire untouched).
//   - isPickedUp is never touched here (no pickup rule by design).

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
// without -I include). In-project, include/config.h always wins.
#ifndef CFG_LEVEL_MAX_TILT_DEG
#define CFG_LEVEL_MAX_TILT_DEG 35.0f
#endif
#ifndef CFG_FUSION_SWAP_AXAY
#define CFG_FUSION_SWAP_AXAY 0
#endif

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

// Explicit mounting: 90deg XY swap applied to raw accel before tilt math.
// Passed by value into Fusion (see below) so orientation is always a
// deliberate input, never ambient build flags.
struct FusionMounting {
  bool swapXY = false;
};

// Canonical production mounting — single source is CFG_FUSION_SWAP_AXAY in
// include/config.h. Production owns one static Fusion built from this
// (wiring lands with the sensor task).
constexpr FusionMounting kBoardMounting{CFG_FUSION_SWAP_AXAY != 0};

// Stateless fusion engine: pure math over (snapshot, mounting) -> state.
// No heap, no Arduino headers, safe to call from any task context.
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
    // NOTE: the forward ToF is obstacle (proximity) distance only — it never
    // feeds the ground derivation. Ground comes from the TCRT rails below.
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

// desky v2 sensor task top half — Arduino-free pure logic (moved verbatim
// from the former middleware task header; the firmware bottom half now lives in
// sensors_task.h). Host Unity tests include this header (via
// sensors_codec.h or sensors_task.h) directly.
//
// ToF sentinel for "no reading yet" (mirrors Vl53l0xDriver::kNoReadingMm;
// duplicated so this namespace stays driver-free for host tests). Obstacle
// hysteresis band + wrap-safe elapsed scheduling + transitions-only
// ground/obstacle edges: see the per-function contracts below.

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
