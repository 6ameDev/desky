#pragma once
// Audio task binder — abilities/audio.
//
// Firmware-only RTOS binding for the audio ability: owns the task-facing
// constants (stack/priority/core, aliased from AudioManager) and forwards the
// public API to the owned AudioManager. main.cpp owns an AudioTask, never an
// AudioManager. Pure DSP lives in audio_codec.h. Zero socket dependencies
// anywhere in this ability — the sole bus interaction is the EVENT_BEEP_DONE
// publish.

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

  void cancelAll() { mgr_.cancelAll(); }

  bool playPcm(const int16_t* samples, size_t frames) { return mgr_.playPcm(samples, frames); }

  audiometer::AudioLevel snapshot() const { return mgr_.snapshot(); }

  AudioManager& manager() { return mgr_; }

 private:
  AudioManager mgr_;
};

#endif  // ARDUINO
