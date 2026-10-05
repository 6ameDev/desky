#pragma once
// desky v2 sensor fusion — Arduino-independent pure logic (stdint/math only,
// like src/middleware/udp_codec.h). Shared by firmware (invoked on-demand by
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
