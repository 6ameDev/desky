#pragma once
// desky v2 audio codec — abilities/audio (ability layer).
//
// Arduino-free beep synth + mic loudness DSP (namespace audiometer), 100%
// native-testable (stdint + math only) so host Unity tests include this
// header directly. Moved verbatim from the top half of the former
// services/audio_manager.h (Slice 4); zero behavior change. Siblings:
// audio_manager.h owns the note queue + synth/meter policy (AudioManager,
// firmware-only); audio_task.h is the thin firmware-only RTOS binder.

// ── Arduino-free: beep synth + meter DSP ────────────────────────────────
#include <math.h>
#include <stdint.h>

#include "config.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as sensors_task.h).
#ifndef CFG_AUDIO_RATE_HZ
#define CFG_AUDIO_RATE_HZ 16000
#endif
#ifndef CFG_AUDIO_VOLUME
#define CFG_AUDIO_VOLUME 0.12f
#endif
#ifndef CFG_AUDIO_METER_MS
#define CFG_AUDIO_METER_MS 100
#endif

namespace audiometer {

// Which mic is louder (GND-mic = left slot, 3V3-mic = right slot by
// hardware tie). TIE covers silence + near-equal levels so noise never
// flaps the verdict.
enum Side : uint8_t { SIDE_LEFT, SIDE_RIGHT, SIDE_TIE };

inline const char* sideName(Side side) { return side == SIDE_LEFT ? "L" : (side == SIDE_RIGHT ? "R" : "TIE"); }

// Deadband: absolute silence floor (counts RMS) + relative margin — levels
// within ~1/8 of each other (or both near zero) read TIE.
constexpr uint32_t kSideFloor = 120;
constexpr uint32_t kSideDeadbandDiv = 8;

inline Side louderSide(uint32_t rmsL, uint32_t rmsR) {
  const uint32_t mx = rmsL > rmsR ? rmsL : rmsR;
  const uint32_t diff = rmsL > rmsR ? rmsL - rmsR : rmsR - rmsL;
  if (mx < kSideFloor) {
    return SIDE_TIE;
  }
  if (diff <= mx / kSideDeadbandDiv) {
    return SIDE_TIE;
  }
  return rmsL > rmsR ? SIDE_LEFT : SIDE_RIGHT;
}

// ~2ms click-suppression ramp in samples at the given rate (32 @16kHz).
inline uint32_t rampSamples(uint32_t rateHz) { return (rateHz * 2u) / 1000u; }

// One bell-chime sample (freq 0 = rest silence): sine fundamental + a soft
// octave partial (0.35x) under an exponential decay (tau = total/3) with a
// short linear attack (~2ms, kills the edge click without a cos table).
// The decay is what sells "chime" vs "buzzer": a struck bell, not a gated
// oscillator. Partial sum is normalized by 1.35 so output never exceeds
// peak. Needs sinf/expf (S3 FPU eats it at 16kHz; host tests link libm).
inline int16_t synthChime(uint32_t idx, uint32_t total, uint16_t freqHz, uint32_t rateHz, int16_t peak) {
  if (freqHz == 0 || total == 0 || idx >= total || rateHz == 0) {
    return 0;
  }
  if (static_cast<float>(freqHz) * 2.0f >= static_cast<float>(rateHz)) {
    return 0;  // Octave partial would alias: silence rather than mush.
  }
  constexpr float kTwoPi = 6.283185307179586f;
  const float t = static_cast<float>(idx) / static_cast<float>(rateHz);
  const float ph = kTwoPi * static_cast<float>(freqHz) * t;
  const float partial = sinf(ph) + 0.35f * sinf(2.0f * ph);
  const float tau = static_cast<float>(total) / 3.0f;
  float env = expf(-static_cast<float>(idx) / tau);
  const uint32_t attack = rampSamples(rateHz);
  if (attack > 0 && idx < attack) {
    env *= static_cast<float>(idx + 1u) / static_cast<float>(attack + 1u);
  }
  return static_cast<int16_t>(partial * (static_cast<float>(peak) / 1.35f) * env);
}

// RMS of unpacked int24 samples (uint64 accumulator: 256 samples of
// (2^23)^2 sum to ~2^54, far from u64 overflow; double mean for precision).
inline uint32_t rmsInt24(const int32_t* samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return 0;
  }
  uint64_t acc = 0;
  for (size_t i = 0; i < count; ++i) {
    const int64_t s = static_cast<int64_t>(samples[i]);
    acc += static_cast<uint64_t>(s * s);
  }
  const double mean = static_cast<double>(acc) / static_cast<double>(count);
  return static_cast<uint32_t>(sqrt(mean));
}

// Meter snapshot for logs/telemetry-side readers (no bus events — the task
// owns the live copy, readers take a mutex-guarded copy).
struct AudioLevel {
  uint32_t rmsL = 0;
  uint32_t rmsR = 0;
  uint32_t peak = 0;  // Leaky peak-hold of max(rmsL, rmsR): attacks instantly.
  Side side = SIDE_TIE;
};

}  // namespace audiometer
