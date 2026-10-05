#pragma once
// desky v2 audio manager — header-only, owns beeps + mic loudness metering.
//
// Owns the I2S HAL (hal/i2s_audio.h duplex port + hal/speaker.h amp format
// + hal/microphone.h mic format) + one Core 0 FreeRTOS task. Non-blocking API:
// beep() enqueues a tone, playTune() queues a short melody, bootJingle()
// queues the 4-bell ascending arpeggio (the jingle itself is the speaker
// self-test). playPcm() stages raw PCM for the server/hub streaming future.
// The RX meter (per-channel RMS over each DMA batch + leaky peak-hold +
// louderSide L/R/TIE with deadband) is snapshot + serial only —
// NO periodic level events (bus spam); the single EVENT_BEEP_DONE (payload
// 0) fires when the beep queue drains to empty, which the coordinator
// explicitly ignores (see system_context.h).
//
// TX synth: bell chimes (sine + octave partial, exponential decay —
// a struck bell, not a gated oscillator). Output is DUAL-MONO (identical
// L+R samples, so the amp's floating SD_MODE (L+R)/2 mix is moot). Quiet
// by construction: peak = CFG_AUDIO_VOLUME of int16 full-scale (~12%
// default — the 8Ω 2W cavity on 5V gets loud).
//
// Serial LOG_I lines: one boot line (rate/format/channels/pins/volume),
// one meter line at CFG_AUDIO_METER_MS cadence (rmsL/rmsR/peak/side), one
// beep-start line per note + one done line per queue drain.
//
// Structure mirrors sensor_task.h: the top half (namespace audiometer) is
// Arduino-free pure DSP (stdint + math only) so host Unity tests include
// this header directly. The bottom half (class AudioManager) is
// firmware-only (#ifdef ARDUINO): note queue + synth/meter task.
//
// Rules: no heap/String in the audio task paths (fixed 8-slot note queue,
// drop + LOG_W when full — matches the EventBus drop style), WDT fed every
// loop, Diagnostics::logWatermarks("AUDIO") every ~5s, task never halts,
// no ISR work (DMA polled in the task).

// ── Arduino-free: beep synth + meter DSP ────────────────────────────────
#include <math.h>
#include <stdint.h>

#include "config.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as sensor_task.h).
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

#ifdef ARDUINO
// ── Firmware: audio service (note queue + synth/meter task, Core 0) ──────────

#include <Arduino.h>

#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/i2s_audio.h"
#include "hal/microphone.h"
#include "hal/speaker.h"
#include "services/event_bus.h"
#include "system_context.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as udp_server.h).
#ifndef CFG_AUDIO_STACK_WORDS
#define CFG_AUDIO_STACK_WORDS 4096
#endif
#ifndef CFG_AUDIO_PRIORITY_OFFSET
#define CFG_AUDIO_PRIORITY_OFFSET 2
#endif
#ifndef CFG_AUDIO_CORE
#define CFG_AUDIO_CORE 0
#endif
#ifndef CFG_AUDIO_METER_MS
#define CFG_AUDIO_METER_MS 100
#endif
#ifndef CFG_AUDIO_VOLUME
#define CFG_AUDIO_VOLUME 0.12f
#endif

class AudioManager {
 public:
  struct Note {
    uint16_t freqHz = 0;  // 0 = rest (silence for durMs)
    uint16_t durMs = 0;
    uint16_t gapMs = 0;  // Trailing silence before the next note.
  };

  static constexpr uint8_t kQueueDepth = 8;  // Fixed slots, drop + LOG_W when full.
  static constexpr uint32_t kStackWords = CFG_AUDIO_STACK_WORDS;
  // Below UDP +3 and below the Core-1 control slot: audio must never preempt
  // sensing, motion, or comms.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_AUDIO_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_AUDIO_CORE;
  static constexpr uint32_t kMeterMs = CFG_AUDIO_METER_MS;
  // Per-loop transfer chunks (independent of the HAL DMA depth): 10ms TX +
  // 16ms RX @16kHz pace the loop without busy-spinning.
  static constexpr uint32_t kTxChunkFrames = 160;
  static constexpr uint32_t kRxBatchFrames = 256;

  // Boot jingle: soft bell arpeggio (E5 G5 B5 E6, ~220ms bells, legato).
  // Chime decay needs room to breathe, so notes run long with short gaps;
  // the tail of each bell melts into the next strike. Quiet by construction
  // (CFG_AUDIO_VOLUME peak). The jingle itself is the speaker self-test.
  static constexpr Note kBootJingle[] = {
      {659, 220, 30},
      {784, 220, 30},
      {988, 220, 30},
      {1319, 420, 0},
  };

  AudioManager()
      : task_(nullptr),
        mutex_(nullptr),
        head_(0),
        tail_(0),
        count_(0),
        busy_(false),
        noteActive_(false),
        noteFreqHz_(0),
        noteLeft_(0),
        notePos_(0),
        noteTotal_(0),
        gapLeft_(0),
        peakAmp_(0),
        lastMeterMs_(0),
        lastWmMs_(0) {}

  // Inits the duplex HAL, logs ONE boot line, and pins the audio task to
  // Core 0. Call once in setup(), after SensorTask::begin (audio never
  // touches sensors — the ordering just keeps fallible inits sequenced) in
  // ALL envs incl. desky-bench (no WiFi dependency), with logging up.
  bool begin() {
    peakAmp_ = static_cast<int16_t>(CFG_AUDIO_VOLUME * 32767.0f);
    if (!hal_.init()) {
      return false;
    }
    mutex_ = xSemaphoreCreateMutex();
    DESKY_ASSERT(mutex_ != nullptr);
    if (mutex_ == nullptr) {
      return false;
    }
    const int volPct = static_cast<int>(CFG_AUDIO_VOLUME * 100.0f);
    LOG_I("AUDIO", "audio ready rate=%uHz tx=32bit-stereo rx=32bit-stereo vol=%d%% ws=%d bclk=%d dout=%d din=%d",
          static_cast<unsigned>(I2sAudio::kRateHz), volPct, MCU_I2S_WS, MCU_I2S_BCLK, MCU_I2S_DOUT, MCU_I2S_DIN);
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&AudioManager::taskEntry, "audio", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // Non-blocking: enqueue one tone (gapMs trailing silence). False + LOG_W
  // when the 8-slot queue is full (drop style, matches EventBus) or dur 0.
  bool beep(uint16_t freqHz, uint16_t durMs, uint16_t gapMs = 0) {
    if (durMs == 0) {
      return false;
    }
    const Note note{freqHz, durMs, gapMs};
    return enqueue(note);
  }

  // Non-blocking: queue a short melody (truncated to free slots). True iff
  // every note fit; partial enqueue keeps the head notes (drop-tail style).
  bool playTune(const Note* seq, size_t len) {
    if (seq == nullptr || len == 0) {
      return false;
    }
    bool allFit = true;
    for (size_t i = 0; i < len; ++i) {
      if (!enqueue(seq[i])) {
        allFit = false;
      }
    }
    return allFit;
  }

  // Non-blocking: queue the boot jingle (call after the BOOT banner lines so
  // the chirp reads as the speaker self-test).
  bool bootJingle() { return playTune(kBootJingle, sizeof(kBootJingle) / sizeof(kBootJingle[0])); }

  // Non-blocking: stage raw int16 mono PCM for playout once the note
  // machinery is fully idle (notes always win over PCM). STUB for the
  // server/hub future: the network task will fill this buffer with streamed
  // chunks; the task drains it exactly like notes (dual-mono'd, done edge).
  // Truncates to the buffer cap. Call from setup/task context, never ISR.
  bool playPcm(const int16_t* samples, size_t frames) {
    if (mutex_ == nullptr || samples == nullptr || frames == 0) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const size_t n = frames < kPcmCapFrames ? frames : kPcmCapFrames;
    for (size_t i = 0; i < n; ++i) {
      pcmBuf_[i] = samples[i];
    }
    pcmLen_ = static_cast<uint32_t>(n);
    pcmPos_ = 0;
    pcmActive_ = true;
    xSemaphoreGive(mutex_);
    LOG_I("AUDIO", "pcm staged %ums", static_cast<unsigned>(pcmLen_ * 1000u / I2sAudio::kRateHz));
    return true;
  }

  // Mutex-guarded copy of the live meter (rmsL/rmsR/peak/side for logs).
  audiometer::AudioLevel snapshot() const {
    audiometer::AudioLevel level;
    if (mutex_ == nullptr) {
      return level;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    level = level_;
    xSemaphoreGive(mutex_);
    return level;
  }

 private:
  static void taskEntry(void* arg) { static_cast<AudioManager*>(arg)->loop(); }

  bool enqueue(const Note& note) {
    if (mutex_ == nullptr || note.durMs == 0) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = false;
    if (count_ < kQueueDepth) {
      queue_[tail_] = note;
      tail_ = static_cast<uint8_t>((tail_ + 1u) % kQueueDepth);
      ++count_;
      busy_ = true;
      ok = true;
    }
    const uint8_t depth = count_;
    xSemaphoreGive(mutex_);
    if (!ok) {
      LOG_W("AUDIO", "note queue full depth=%u dropping freq=%uHz", depth, note.freqHz);
    }
    return ok;
  }

  // Dequeue one note into the task-owned playback registers. Short lock,
  // task context only.
  bool dequeueNext(Note& out) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = false;
    if (count_ > 0) {
      out = queue_[head_];
      head_ = static_cast<uint8_t>((head_ + 1u) % kQueueDepth);
      --count_;
      ok = true;
    }
    xSemaphoreGive(mutex_);
    return ok;
  }

  void loop() {
    DESKY_ASSERT(mutex_ != nullptr);
    esp_task_wdt_add(nullptr);
    lastMeterMs_ = millis();
    lastWmMs_ = lastMeterMs_;
    // Never returns / never halts by design (see coordinator.h: a halted
    // WDT-subscribed task becomes a panic-reboot loop).
    for (;;) {
      // RX: one DMA batch per loop (16ms of stereo-32 @16kHz, 30ms cap).
      // Timeouts hold the last meter in silence — no stale-zero flicker.
      size_t got = hal_.readRx(rxStereo_, sizeof(rxStereo_), 30);
      const size_t frames = got / (2u * sizeof(uint32_t));
      if (frames > 0) {
        updateMeter(frames);
      }

      // TX: fill one 10ms dual-mono chunk (pcm -> note -> gap -> queued ->
      // idle silence; the idle silence write keeps the amp fed with zeros so
      // it never holds a stale half-cycle).
      uint32_t avail = kTxChunkFrames;
      uint32_t filled = 0;
      bool pcmDone = false;
      xSemaphoreTake(mutex_, portMAX_DELAY);
      // PCM yields to all note machinery (active note, gap, queued notes).
      bool pcm = pcmActive_ && !noteActive_ && gapLeft_ == 0 && count_ == 0;
      xSemaphoreGive(mutex_);
      while (avail > 0) {
        if (pcm && pcmPos_ < pcmLen_) {
          const uint32_t remain = pcmLen_ - pcmPos_;
          const uint32_t n = avail < remain ? avail : remain;
          for (uint32_t i = 0; i < n; ++i) {
            txMono_[filled + i] = pcmBuf_[pcmPos_ + i];
          }
          filled += n;
          avail -= n;
          pcmPos_ += n;
          if (pcmPos_ >= pcmLen_) {
            pcm = false;  // Exhausted: fall through to note/idle fill below.
            pcmDone = true;
          }
        } else if (gapLeft_ > 0) {
          const uint32_t n = avail < gapLeft_ ? avail : gapLeft_;
          for (uint32_t i = 0; i < n; ++i) {
            txMono_[filled + i] = 0;
          }
          filled += n;
          avail -= n;
          gapLeft_ -= n;
        } else if (!noteActive_) {
          Note next;
          if (!dequeueNext(next)) {
            for (uint32_t i = 0; i < avail; ++i) {
              txMono_[filled + i] = 0;
            }
            filled += avail;
            avail = 0;
            drainCheck();
          } else {
            noteFreqHz_ = next.freqHz;
            noteTotal_ = static_cast<uint32_t>(next.durMs) * (I2sAudio::kRateHz / 1000u);
            noteLeft_ = noteTotal_;
            notePos_ = 0;
            noteActive_ = noteTotal_ > 0;
            gapLeft_ = static_cast<uint32_t>(next.gapMs) * (I2sAudio::kRateHz / 1000u);
            LOG_I("AUDIO", "beep start freq=%uHz dur=%ums gap=%ums", next.freqHz, next.durMs, next.gapMs);
            if (!noteActive_) {
              drainCheck();
            }
          }
        } else {
          const uint32_t n = avail < noteLeft_ ? avail : noteLeft_;
          for (uint32_t i = 0; i < n; ++i) {
            txMono_[filled + i] =
                audiometer::synthChime(notePos_ + i, noteTotal_, noteFreqHz_, I2sAudio::kRateHz, peakAmp_);
          }
          filled += n;
          avail -= n;
          notePos_ += n;
          noteLeft_ -= n;
          if (noteLeft_ == 0) {
            noteActive_ = false;
          }
        }
      }
      if (pcmDone) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        pcmActive_ = false;
        xSemaphoreGive(mutex_);
        LOG_I("AUDIO", "pcm done");
        EventBus::publish(EVENT_BEEP_DONE, 0);
      }
      speaker::packMono32(txMono_, txStereo_, kTxChunkFrames);
      hal_.writeTx(txStereo_, sizeof(txStereo_), 50);

      // Meter serial line at the CFG cadence (the ONLY periodic audio log).
      const uint32_t nowMs = millis();
      if (nowMs - lastMeterMs_ >= kMeterMs) {
        lastMeterMs_ = nowMs;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        const audiometer::AudioLevel level = level_;
        xSemaphoreGive(mutex_);
        LOG_I("AUDIO", "meter rmsL=%lu rmsR=%lu peak=%lu side=%s", static_cast<unsigned long>(level.rmsL),
              static_cast<unsigned long>(level.rmsR), static_cast<unsigned long>(level.peak),
              audiometer::sideName(level.side));
      }

      FaultManager::watchdogFeed();
      if (nowMs - lastWmMs_ >= 5000u) {
        lastWmMs_ = nowMs;
        Diagnostics::logWatermarks("AUDIO");
      }
    }
  }

  // Queue-drained edge (task context, call with no lock held): exactly one
  // EVENT_BEEP_DONE per busy epoch + the done serial line.
  void drainCheck() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool drained = (count_ == 0) && busy_;
    if (drained) {
      busy_ = false;
    }
    xSemaphoreGive(mutex_);
    if (drained) {
      LOG_I("AUDIO", "beep done queue drained");
      EventBus::publish(EVENT_BEEP_DONE, 0);
    }
  }

  // Per-channel RMS over one DMA batch + leaky peak-hold + louderSide.
  // Unpacks int24 on the fly (no second buffer, no heap).
  void updateMeter(size_t frames) {
    uint64_t accL = 0;
    uint64_t accR = 0;
    for (size_t i = 0; i < frames; ++i) {
      const int64_t l = static_cast<int64_t>(microphone::unpackSample(rxStereo_[2 * i]));
      const int64_t r = static_cast<int64_t>(microphone::unpackSample(rxStereo_[2 * i + 1]));
      accL += static_cast<uint64_t>(l * l);
      accR += static_cast<uint64_t>(r * r);
    }
    const double n = static_cast<double>(frames);
    const uint32_t rmsL = static_cast<uint32_t>(sqrt(static_cast<double>(accL) / n));
    const uint32_t rmsR = static_cast<uint32_t>(sqrt(static_cast<double>(accR) / n));
    const uint32_t curMax = rmsL > rmsR ? rmsL : rmsR;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    level_.rmsL = rmsL;
    level_.rmsR = rmsR;
    // Leaky peak-hold: attacks instantly, decays ~0.8%/batch (~16ms) so a
    // clap spike stays visible across several meter lines, then settles.
    level_.peak = (level_.peak * 127u) / 128u;
    if (curMax > level_.peak) {
      level_.peak = curMax;
    }
    level_.side = audiometer::louderSide(rmsL, rmsR);
    xSemaphoreGive(mutex_);
  }

  I2sAudio hal_;
  TaskHandle_t task_;
  SemaphoreHandle_t mutex_;
  Note queue_[kQueueDepth];
  uint8_t head_;
  uint8_t tail_;
  uint8_t count_;
  bool busy_;  // Set on enqueue, cleared on the drain edge (drives EVENT_BEEP_DONE).
  // Task-owned playback registers (no lock: only the task touches them).
  bool noteActive_;
  uint16_t noteFreqHz_;
  uint32_t noteLeft_;
  uint32_t notePos_;
  uint32_t noteTotal_;
  uint32_t gapLeft_;
  int16_t peakAmp_;
  audiometer::AudioLevel level_;  // Task writes / snapshot() reads under mutex_.
  uint32_t lastMeterMs_;
  uint32_t lastWmMs_;
  // Staged-PCM playout buffer (playPcm): 1.024s cap @16kHz, int16 mono —
  // the server/hub streaming stub. Dual-mono'd on the way out like notes.
  static constexpr uint32_t kPcmCapFrames = 16384;
  int16_t pcmBuf_[kPcmCapFrames];
  uint32_t pcmLen_ = 0;
  uint32_t pcmPos_ = 0;
  bool pcmActive_ = false;
  // Task transfer buffers (members, not stack; no heap anywhere).
  int16_t txMono_[kTxChunkFrames];
  int32_t txStereo_[2 * kTxChunkFrames];  // 32-bit slots (see speaker::packMono32).
  uint32_t rxStereo_[2 * kRxBatchFrames];
};

#endif  // ARDUINO
