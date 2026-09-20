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
//   - Cliff (ground drop): (tofMm > CFG_CLIFF_MM) && (|az| < gate). The accel
//     gate rejects false positives when level (|az| ~ 1g at rest).
//   - Health gating: an unhealthy source skips its own updates and retains
//     last state — MPU unhealthy freezes pitch/roll, ToF invalid freezes
//     distanceMM, and the fused cliffDetected freezes unless BOTH sources
//     are live. A dead sensor never clears flags.
//   - distanceMM retains its last value on invalid ToF (stale readings must
//     neither report nor clear cliff).
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
};

// Threshold fallbacks keep this header compilable standalone (host g++
// without -I include). In-project, include/config.h always wins.
#ifndef CFG_CLIFF_MM
#define CFG_CLIFF_MM 100
#endif
#ifndef CFG_CLIFF_ACCEL_Z_GATE_G
#define CFG_CLIFF_ACCEL_Z_GATE_G 0.5f
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
    if (snap.mpuHealthy && snap.tofValid) {
      const bool far = snap.tofMm > CFG_CLIFF_MM;
      const bool unloaded = fabsf(snap.az) < CFG_CLIFF_ACCEL_Z_GATE_G;
      state.cliffDetected = far && unloaded;
    }
  }

 private:
  FusionMounting mounting_;
};

}  // namespace fusion
