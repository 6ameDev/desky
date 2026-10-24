#pragma once
// Audio codec — abilities/audio.
//
// Arduino-free beep synth + mic loudness DSP (namespace audiometer), 100%
// native-testable (stdint + math only) so host tests include it directly.
// Siblings: audio_manager.h owns the note queue + synth/meter policy;
// audio_task.h is the thin RTOS binder.

// ── Arduino-free: beep synth + meter DSP ────────────────────────────────
#include <math.h>
#include <stdint.h>

#include "config.h"

// Shared expression catalog ids (V2 kSound* vocabularies + NONE sentinels).
#include "link/expression_catalog.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
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

// Which mic is louder (GND-mic = left slot, 3V3-mic = right slot). TIE
// covers silence + near-equal levels so noise never flaps the verdict.
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
// short linear attack (~2ms, kills the edge click). Partial sum is normalized
// by 1.35 so output never exceeds peak. Needs sinf/expf.
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

// RMS of unpacked int24 samples (uint64 accumulator).
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

// Expression sound cues (namespace exprcue) — the single keyed table the
// expression conductor plays from: cue id (catalog kSound*) to short note
// sequences. Plain POD (no AudioManager::Note dependency); the conductor's
// audio sink converts one beep() call per note. Every cue fits the 8-slot
// note queue with room to spare. NONE/unknown ids yield no notes (silent).
namespace exprcue {

struct CueNote {
  uint16_t freqHz = 0;  // 0 = rest (silence for durMs)
  uint16_t durMs = 0;
  uint16_t gapMs = 0;  // Trailing silence before the next note.
};

// Angry: low buzz. Scared: tremolo. Excited: ascending fanfare. Confused:
// two-note wobble.
constexpr CueNote kCueAngry[] = {{130, 140, 15}, {98, 220, 0}};
constexpr CueNote kCueScared[] = {{880, 70, 20}, {880, 70, 20}, {660, 140, 0}};
constexpr CueNote kCueExcited[] = {{523, 110, 15}, {659, 110, 15}, {784, 110, 15}, {1047, 220, 0}};
constexpr CueNote kCueConfused[] = {{392, 150, 30}, {311, 220, 0}};

struct Cue {
  const CueNote* notes = nullptr;
  uint8_t len = 0;
};

inline Cue cueFor(uint8_t soundId) {
  switch (soundId) {
    case exprcat::kSoundAngry:
      return {kCueAngry, sizeof(kCueAngry) / sizeof(kCueAngry[0])};
    case exprcat::kSoundScared:
      return {kCueScared, sizeof(kCueScared) / sizeof(kCueScared[0])};
    case exprcat::kSoundExcited:
      return {kCueExcited, sizeof(kCueExcited) / sizeof(kCueExcited[0])};
    case exprcat::kSoundConfused:
      return {kCueConfused, sizeof(kCueConfused) / sizeof(kCueConfused[0])};
    default:
      return {nullptr, 0};
  }
}

}  // namespace exprcue
