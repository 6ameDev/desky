#pragma once
// I2S duplex port owner — header-only, peripheral only, NO policy and NO part
// format (amp packing lives in speaker.h, mic unpacking in microphone.h;
// synth + meters live in abilities/audio).
//
// One I2S_NUM_0 port @CFG_AUDIO_RATE_HZ: TX feeds the amp, RX drains the
// mics, both on the one BCLK/WS clock. Wiring in the MCU header, never
// hardcoded here. ESP-IDF i2s_std driver directly (full-duplex); the legacy
// Arduino I2S library is NOT used.
//
// The single frame width is load-bearing: TX and RX share the port clock, so
// mixed widths would mis-clock one side (static_assert enforces the part
// headers agree; the slot literals in init() must match).
//
// Class I2sAudio is firmware-only: duplex init/deinit + blocking-with-timeout
// raw frame read/write + last-error accessor. init() returns bool for
// DESKY_ASSERT at the call site. No ISR work (the manager polls DMA).
//
// Rules: no String/heap in read/write, no hardcoded pins.

#include <stdint.h>

#include "config.h"
#include "microphone.h"
#include "speaker.h"

// Both directions share one BCLK: refuse to build if the part headers ever
// disagree on slot width (then update the slot literals in init() to match).
static_assert(speaker::kSlotBitWidth == microphone::kSlotBitWidth,
              "I2S duplex shares one BCLK: speaker/mic slot widths must match");

// Fallbacks if config keys drift; in-project include/config.h always wins.
#ifndef CFG_AUDIO_RATE_HZ
#define CFG_AUDIO_RATE_HZ 16000
#endif

#ifdef ARDUINO
// ── Firmware: duplex I2S peripheral (TX amp + RX mics, I2S_NUM_0) ────────────

#include <Arduino.h>
#include <driver/i2s_std.h>

#include "common/fault_manager.h"
#include "common/logger.h"

class I2sAudio {
 public:
  static constexpr uint32_t kRateHz = CFG_AUDIO_RATE_HZ;
  // Small DMA depth: a few 16ms-ish buffers in flight, not a buffer farm
  // (keeps beep latency + meter freshness tight, no heap anywhere). One
  // frame count is shared by both handles (i2s_chan_config_t carries a
  // single dma_frame_num); task transfer chunking is independent of it.
  static constexpr uint32_t kDmaFrames = 256;  // 16ms @16kHz per buffer
  static constexpr uint32_t kDmaDescNum = 4;

  I2sAudio() : tx_(nullptr), rx_(nullptr), lastErr_(ESP_OK), ready_(false) {}

  // Duplex init: one BCLK/WS clock, TX 16-bit stereo slots on DOUT, RX 32-bit
  // stereo slots on DIN. GPIOs from the MCU header via config. Returns false
  // (never halts) on any ESP-IDF error.
  bool init() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = kDmaDescNum;
    chanCfg.dma_frame_num = kDmaFrames;
    esp_err_t err = i2s_new_channel(&chanCfg, &tx_, &rx_);
    if (err != ESP_OK || tx_ == nullptr || rx_ == nullptr) {
      lastErr_ = err;
      LOG_E("AUDIO", "i2s_new_channel failed err=%d", static_cast<int>(lastErr_));
      return false;
    }

    i2s_std_clk_config_t txClk = I2S_STD_CLK_DEFAULT_CONFIG(kRateHz);
    i2s_std_slot_config_t txSlot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t txGpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = static_cast<gpio_num_t>(MCU_I2S_BCLK),
        .ws = static_cast<gpio_num_t>(MCU_I2S_WS),
        .dout = static_cast<gpio_num_t>(MCU_I2S_DOUT),
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    };
    i2s_std_config_t txCfg = {.clk_cfg = txClk, .slot_cfg = txSlot, .gpio_cfg = txGpio};
    err = i2s_channel_init_std_mode(tx_, &txCfg);
    if (err != ESP_OK) {
      lastErr_ = err;
      LOG_E("AUDIO", "tx std mode failed err=%d", static_cast<int>(lastErr_));
      teardown();
      return false;
    }

    // Same BCLK/WS clock, 32-bit stereo slots for the mic pair (24-bit
    // samples top-justified; see microphone.h).
    i2s_std_clk_config_t rxClk = I2S_STD_CLK_DEFAULT_CONFIG(kRateHz);
    i2s_std_slot_config_t rxSlot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t rxGpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = static_cast<gpio_num_t>(MCU_I2S_BCLK),
        .ws = static_cast<gpio_num_t>(MCU_I2S_WS),
        .dout = I2S_GPIO_UNUSED,
        .din = static_cast<gpio_num_t>(MCU_I2S_DIN),
        .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    };
    i2s_std_config_t rxCfg = {.clk_cfg = rxClk, .slot_cfg = rxSlot, .gpio_cfg = rxGpio};
    err = i2s_channel_init_std_mode(rx_, &rxCfg);
    if (err != ESP_OK) {
      lastErr_ = err;
      LOG_E("AUDIO", "rx std mode failed err=%d", static_cast<int>(lastErr_));
      teardown();
      return false;
    }

    err = i2s_channel_enable(tx_);
    if (err != ESP_OK) {
      lastErr_ = err;
      LOG_E("AUDIO", "tx enable failed err=%d", static_cast<int>(lastErr_));
      teardown();
      return false;
    }
    err = i2s_channel_enable(rx_);
    if (err != ESP_OK) {
      lastErr_ = err;
      LOG_E("AUDIO", "rx enable failed err=%d", static_cast<int>(lastErr_));
      teardown();
      return false;
    }
    ready_ = true;
    lastErr_ = ESP_OK;
    return true;
  }

  void deinit() { teardown(); }

  // Blocking-with-timeout write of raw TX frames (32-bit stereo interleaved).
  // Returns bytes actually written (short on timeout).
  size_t writeTx(const void* frames, size_t bytes, uint32_t timeoutMs) {
    if (!ready_ || frames == nullptr || bytes == 0) {
      return 0;
    }
    size_t written = 0;
    lastErr_ = i2s_channel_write(tx_, frames, bytes, &written, pdMS_TO_TICKS(timeoutMs));
    return written;
  }

  // Blocking-with-timeout read of raw RX frames (32-bit stereo interleaved).
  // Returns bytes actually read (short on timeout).
  size_t readRx(void* frames, size_t bytes, uint32_t timeoutMs) {
    if (!ready_ || frames == nullptr || bytes == 0) {
      return 0;
    }
    size_t bytesRead = 0;
    lastErr_ = i2s_channel_read(rx_, frames, bytes, &bytesRead, pdMS_TO_TICKS(timeoutMs));
    return bytesRead;
  }

  esp_err_t lastError() const { return lastErr_; }
  bool isReady() const { return ready_; }

 private:
  // Best-effort unwind for partial-init failures (never halts, never asserts:
  // init() already reported the load-bearing error).
  void teardown() {
    if (tx_ != nullptr) {
      i2s_channel_disable(tx_);
      i2s_del_channel(tx_);
      tx_ = nullptr;
    }
    if (rx_ != nullptr) {
      i2s_channel_disable(rx_);
      i2s_del_channel(rx_);
      rx_ = nullptr;
    }
    ready_ = false;
  }

  i2s_chan_handle_t tx_;
  i2s_chan_handle_t rx_;
  esp_err_t lastErr_;
  bool ready_;
};

#endif  // ARDUINO
