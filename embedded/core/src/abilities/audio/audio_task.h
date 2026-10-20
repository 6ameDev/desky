#pragma once
// desky v2 audio task binder — abilities/audio (ability layer).
//
// Firmware-only RTOS binding for the audio ability: owns the task-facing
// constants (stack/priority/core, aliased from AudioManager so there is one
// source of truth) and forwards the public API (begin/beep/playTune/
// bootJingle/playPcm/snapshot) to the owned AudioManager. main.cpp owns an
// AudioTask, never an AudioManager, so a future slice can migrate the loop
// down here (pairing Slice 6b pattern) without touching call sites.
//
// Seam note (Slice 4): AudioManager fuses queue/synth policy with its Core 0
// loop — mutex-interleaved member access throughout (enqueue/dequeueNext/
// drainCheck/updateMeter share queue_/level_/pcm state with the loop under
// one mutex), task-owned playback registers interleaved with shared state,
// and the same object inits the HAL port it polls. No pure-motion seam
// exists: extracting the loop would mean friendship + member migration or a
// new accessor surface, i.e. logic changes — so the manager stays whole in
// audio_manager.h and this binder stays thin. Pure DSP lives in
// audio_codec.h. Zero socket dependencies anywhere in this ability — the
// sole bus interaction is the EventBus EVENT_BEEP_DONE publish, like
// SensorTask.

#include "audio_manager.h"

#ifdef ARDUINO
// ── Firmware: audio task binder (forwards to AudioManager, Core 0) ─────────

#include <Arduino.h>

class AudioTask {
 public:
  static constexpr uint32_t kStackWords = AudioManager::kStackWords;
  static constexpr UBaseType_t kPriority = AudioManager::kPriority;
  static constexpr BaseType_t kCore = AudioManager::kCore;

  bool begin() { return mgr_.begin(); }

  bool beep(uint16_t freqHz, uint16_t durMs, uint16_t gapMs = 0) { return mgr_.beep(freqHz, durMs, gapMs); }

  bool playTune(const AudioManager::Note* seq, size_t len) { return mgr_.playTune(seq, len); }

  bool bootJingle() { return mgr_.bootJingle(); }

  bool playPcm(const int16_t* samples, size_t frames) { return mgr_.playPcm(samples, frames); }

  audiometer::AudioLevel snapshot() const { return mgr_.snapshot(); }

  AudioManager& manager() { return mgr_; }

 private:
  AudioManager mgr_;
};

#endif  // ARDUINO
