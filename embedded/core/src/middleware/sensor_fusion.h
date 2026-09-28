#pragma once
// desky v2 sensor fusion — Arduino-independent pure logic (stdint/math only,
// like src/middleware/udp_codec.h). Shared by firmware (invoked on-demand by
// the future coordinator task — there is NO sensor polling task) and host
// Unity tests. No heap, no Arduino headers.
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
//   - Ground/cliff per beam (fwd + mirrored rev): the ToF looks 30deg down,
//     so a beam's flat-ground distance scales as 1/sin(depression); the
//     threshold scales by that ratio (see cliffThresholdMm). The tilt gate is
//     pose-explicit total-tilt-magnitude from freshly fused pitch/roll:
//     level ⟺ (pitch²+roll²) < 35², evaluated only when mpuHealthy.
//     Past 35° total tilt the gate abstains (no cliff verdict — ground holds
//     true); past ~25° pitch the 10/80° clamp bounds the threshold.
//   - Health gating: an unhealthy source skips its own updates and retains
//     last state — MPU unhealthy freezes pitch/roll, ToF invalid freezes
//     distanceMM, and each ground bit freezes unless its own rail (MPU + its
//     ToF) is live. A dead sensor never clears flags.
//   - distanceMM tracks the forward ToF only and retains its last value on
//     invalid ToF (stale readings must neither report nor clear cliff).
//   - gndFwd is the compensated forward cliff derivation; gndRev is the
//     mirrored rear derivation (fail-open: no rear sensor exists yet, so the
//     invalid-hold pins it at default-true). cliffDetected stays as the raw
//     forward flag (telemetry/snapshot/wire untouched).
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
  uint16_t tofRevMm = 0;
  bool tofRevValid = false;  // default invalid: no rear sensor yet, invalid-hold pins gndRev true
};

// Threshold fallbacks keep this header compilable standalone (host g++
// without -I include). In-project, include/config.h always wins.
#ifndef CFG_CLIFF_MM
#define CFG_CLIFF_MM 100
#endif
#ifndef CFG_CLIFF_MM_REV
#define CFG_CLIFF_MM_REV 100
#endif
#ifndef CFG_BEAM_DEPRESSION_DEG
#define CFG_BEAM_DEPRESSION_DEG 30
#endif
#ifndef CFG_LEVEL_MAX_TILT_DEG
#define CFG_LEVEL_MAX_TILT_DEG 35.0f
#endif
#ifndef CFG_FUSION_SWAP_AXAY
#define CFG_FUSION_SWAP_AXAY 0
#endif

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

// Pitch-compensated cliff threshold (plain floats, no state): on flat terrain
// a beam's ground distance scales as 1/sin(depression), so the threshold
// scales by that ratio: thr = base * sin(30°) / sin(dep_eff),
// dep_eff = clamp(30° − s·pitchDeg, 10°, 80°), s = +1 fwd / −1 rev (the rear
// beam mirrors: nose-up steepens it). Clamp rationale: below 10° the 1/sin
// blow-up is unphysical (near-grazing beam, noise dominates); above 80° the
// beam points near-straight-down. Worked example (base 100): level → 100mm,
// nose-up +15° fwd → ~193mm (a 120mm climb reading passes), nose-down −15°
// fwd → ~71mm (stricter descending).
inline float cliffThresholdMm(float baseMm, float pitchDeg, bool fwdSide) {
  const float base = static_cast<float>(CFG_BEAM_DEPRESSION_DEG);
  const float s = fwdSide ? 1.0f : -1.0f;
  float dep = base - s * pitchDeg;
  if (dep < 10.0f) {
    dep = 10.0f;
  }
  if (dep > 80.0f) {
    dep = 80.0f;
  }
  return baseMm * sinf(base * kDeg2Rad) / sinf(dep * kDeg2Rad);
}

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
    // NOTE: ToF obstacle (proximity) use is future work; this derivation is ground-drop only.
    if (snap.mpuHealthy && snap.tofValid) {
      const float thrFwd = cliffThresholdMm(static_cast<float>(CFG_CLIFF_MM), state.pitch, true);
      const bool far = static_cast<float>(snap.tofMm) > thrFwd;
      const float tilt2 = state.pitch * state.pitch + state.roll * state.roll;
      const float gate = static_cast<float>(CFG_LEVEL_MAX_TILT_DEG);
      const bool level = tilt2 < gate * gate;
      const bool cliff = far && level;
      state.gndFwd = !cliff;
      state.cliffDetected = cliff;  // raw fwd flag: telemetry/snapshot/wire untouched
    }
    if (snap.mpuHealthy && snap.tofRevValid) {
      const float thrRev = cliffThresholdMm(static_cast<float>(CFG_CLIFF_MM_REV), state.pitch, false);
      const bool far = static_cast<float>(snap.tofRevMm) > thrRev;
      const float tilt2 = state.pitch * state.pitch + state.roll * state.roll;
      const float gate = static_cast<float>(CFG_LEVEL_MAX_TILT_DEG);
      const bool level = tilt2 < gate * gate;
      state.gndRev = !(far && level);
    }
    // Dead rails hold their ground bits (never clear); isPickedUp untouched.
  }

 private:
  FusionMounting mounting_;
};

}  // namespace fusion
