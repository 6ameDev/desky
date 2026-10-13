#pragma once
// desky-testbench audio POC (mic stages 1-2 + speaker stage S2) —
// TEMPORARY POC file: prove the parts, then DELETE so the rig returns to the
// lean baseline (see src/main.cpp workflow note).
//
// Duplex I2S0 @CFG_AUDIO_RATE_HZ (16kHz), 32-bit stereo slots: RX on DIN
// (mics), TX on DOUT (amp). Slot format mirrors core's hal/microphone.h +
// hal/speaker.h (Voltros INMP441: 24-bit samples left-justified in 32-bit
// slots; MAX98357A: int16 synth top-justified, dual-mono).
//
// DSP below is Arduino-free (stdint only) so host Unity tests reach it
// directly; class MicRx is firmware-only (#ifdef ARDUINO): duplex init +
// blocking-with-timeout raw frame read/write. RX init is required; TX init
// is best-effort (an unwired amp must never break the mic meter).
// init() returns bool, never halts — a dead part is a VALID POC result.

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace micpoc {

// I2S slot contract: 32-bit stereo, interleaved L/R frames.
constexpr uint32_t kSlotBitWidth = 32;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kLeftSlot = 0;   // L/R→GND mic by hardware tie.
constexpr uint32_t kRightSlot = 1;  // L/R→3V3 mic by hardware tie.

// 32-bit slot -> signed sample: left-justified 24-bit sample in the top 24
// bits, so an arithmetic shift right by 8 sign-extends int24 into int32.
// HARDWARE ASSUMPTION (same flag as core): confirm on the bench — a clap
// near the mic must read a large RMS; constant digital zero = dead board.
inline int32_t unpackSample(uint32_t rawSlot) { return static_cast<int32_t>(rawSlot) >> 8; }

// RMS of one slot over interleaved stereo frames (no heap, no second
// buffer). slot is kLeftSlot/kRightSlot; returns 0 on empty input.
inline uint32_t slotRms(const uint32_t* frames, size_t frameCount, uint32_t slot) {
  if (frames == nullptr || frameCount == 0 || slot >= kChannels) {
    return 0;
  }
  uint64_t acc = 0;
  for (size_t i = 0; i < frameCount; ++i) {
    const int64_t s = static_cast<int64_t>(unpackSample(frames[kChannels * i + slot]));
    acc += static_cast<uint64_t>(s * s);
  }
  const double mean = static_cast<double>(acc) / static_cast<double>(frameCount);
  return static_cast<uint32_t>(sqrt(mean));
}

}  // namespace micpoc

// ── Demo presentation (TEMPORARY video-shoot block) ─────────────────────────
// Bar-graph meter + LIVE/SILENT verdict. Arduino-free so host tests pin it.
namespace demopoc {

constexpr uint32_t kBarWidth = 16;
// RMS per bar char: room floor (~2-9k) reads 1-2 chars, rub/clap pegs 16.
constexpr uint32_t kRmsPerChar = 4000;
// A slot is LIVE once its peak-hold crosses this (well above any silence).
constexpr uint32_t kLiveFloor = 20000;

inline uint32_t barChars(uint32_t rms) {
  uint32_t chars = rms / kRmsPerChar;
  return chars > kBarWidth ? kBarWidth : chars;
}

inline bool slotLive(uint32_t peakHold) { return peakHold >= kLiveFloor; }

}  // namespace demopoc

namespace spkpoc {

// One sine-beep sample with a short linear attack/release ramp (~2ms at the
// given rate) to kill edge clicks. freqHz 0 = rest silence.
inline int16_t synthSine(uint32_t idx, uint32_t total, uint16_t freqHz, uint32_t rateHz, int16_t peak) {
  if (freqHz == 0 || total == 0 || idx >= total || rateHz == 0) {
    return 0;
  }
  const uint32_t ramp = (rateHz * 2u) / 1000u;
  float env = 1.0f;
  if (ramp > 0) {
    if (idx < ramp) {
      env = static_cast<float>(idx + 1u) / static_cast<float>(ramp + 1u);
    }
    const uint32_t tail = total - idx - 1u;  // samples after idx; safe: idx < total
    if (tail < ramp) {
      const float rel = static_cast<float>(tail + 1u) / static_cast<float>(ramp + 1u);
      if (rel < env) {
        env = rel;
      }
    }
  }
  const float ph =
      6.283185307179586f * static_cast<float>(freqHz) * static_cast<float>(idx) / static_cast<float>(rateHz);
  return static_cast<int16_t>(sinf(ph) * static_cast<float>(peak) * env);
}

// Mono int16 synth -> interleaved 32-bit stereo frames, identical L+R
// (dual-mono, same contract as core's speaker::packMono32: top-justified
// int16 plays at full scale on 16/24/32-bit amps, and every SD_MODE pick
// reproduces the same sound).
inline void packMono32(const int16_t* mono, int32_t* stereoLR, uint32_t frames) {
  if (mono == nullptr || stereoLR == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    const int32_t s = static_cast<int32_t>(mono[i]) << 16;
    stereoLR[2 * i] = s;
    stereoLR[2 * i + 1] = s;
  }
}

}  // namespace spkpoc
#ifdef ARDUINO
// ── Firmware: duplex I2S peripheral (RX mics on DIN + TX amp on DOUT) ─────────

#include <Arduino.h>
#include <driver/i2s_std.h>

class MicRx {
 public:
  MicRx(int wsPin, int sckPin, int sdPin, int doutPin, uint32_t rateHz)
      : ws_(wsPin),
        sck_(sckPin),
        sd_(sdPin),
        dout_(doutPin),
        rateHz_(rateHz),
        tx_(nullptr),
        rx_(nullptr),
        ready_(false),
        txReady_(false) {}

  // Duplex init: one 16kHz BCLK/WS clock, RX 32-bit stereo slots on DIN
  // (required — failure returns false), TX 32-bit stereo slots on DOUT
  // (best-effort — an unwired amp logs tx-off and the mic meter carries
  // on). Returns false (never halts) if the RX side fails.
  bool init() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 4;
    chanCfg.dma_frame_num = 256;  // 16ms @16kHz per buffer
    esp_err_t err = i2s_new_channel(&chanCfg, &tx_, &rx_);
    if (err != ESP_OK || tx_ == nullptr || rx_ == nullptr) {
      teardown();
      return false;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rateHz_);
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t rxGpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = static_cast<gpio_num_t>(sck_),
        .ws = static_cast<gpio_num_t>(ws_),
        .dout = I2S_GPIO_UNUSED,
        .din = static_cast<gpio_num_t>(sd_),
        .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    };
    i2s_std_config_t rxCfg = {.clk_cfg = clk, .slot_cfg = slot, .gpio_cfg = rxGpio};
    err = i2s_channel_init_std_mode(rx_, &rxCfg);
    if (err != ESP_OK) {
      teardown();
      return false;
    }
    // TX side: best-effort (amp may not be wired yet).
    i2s_std_gpio_config_t txGpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = static_cast<gpio_num_t>(sck_),
        .ws = static_cast<gpio_num_t>(ws_),
        .dout = static_cast<gpio_num_t>(dout_),
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    };
    i2s_std_config_t txCfg = {.clk_cfg = clk, .slot_cfg = slot, .gpio_cfg = txGpio};
    txReady_ = (i2s_channel_init_std_mode(tx_, &txCfg) == ESP_OK);
    err = i2s_channel_enable(rx_);
    if (err != ESP_OK) {
      teardown();
      return false;
    }
    if (txReady_) {
      if (i2s_channel_enable(tx_) != ESP_OK) {
        teardownTx();
      }
    }
    ready_ = true;
    return true;
  }

  void deinit() { teardown(); }

  // Blocking-with-timeout read of raw RX frames (32-bit stereo interleaved,
  // bytes = frames * 8). Returns frames actually read (short on timeout).
  size_t readFrames(uint32_t* frames, size_t frameCount, uint32_t timeoutMs) {
    if (!ready_ || frames == nullptr || frameCount == 0) {
      return 0;
    }
    size_t bytesRead = 0;
    (void)i2s_channel_read(rx_, frames, frameCount * 8u, &bytesRead, pdMS_TO_TICKS(timeoutMs));
    return bytesRead / 8u;
  }

  // Blocking-with-timeout write of raw TX frames (32-bit stereo interleaved,
  // bytes = frames * 8). No-op (returns 0) while the amp side is down.
  size_t writeFrames(const void* frames, size_t frameCount, uint32_t timeoutMs) {
    if (!txReady_ || frames == nullptr || frameCount == 0) {
      return 0;
    }
    size_t written = 0;
    (void)i2s_channel_write(tx_, frames, frameCount * 8u, &written, pdMS_TO_TICKS(timeoutMs));
    return written / 8u;
  }

  bool isReady() const { return ready_; }
  bool txReady() const { return txReady_; }

 private:
  void teardownTx() {
    if (tx_ != nullptr) {
      (void)i2s_channel_disable(tx_);
      (void)i2s_del_channel(tx_);
      tx_ = nullptr;
    }
    txReady_ = false;
  }

  void teardown() {
    teardownTx();
    if (rx_ != nullptr) {
      i2s_channel_disable(rx_);
      i2s_del_channel(rx_);
      rx_ = nullptr;
    }
    ready_ = false;
  }

  int ws_;
  int sck_;
  int sd_;
  int dout_;
  uint32_t rateHz_;
  i2s_chan_handle_t tx_;
  i2s_chan_handle_t rx_;
  bool ready_;
  bool txReady_;
};

#endif  // ARDUINO
