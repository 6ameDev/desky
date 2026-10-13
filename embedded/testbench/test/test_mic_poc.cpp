// Host unit tests for the mic POC DSP (no hardware).
// The DSP is Arduino-free (stdint only); the I2S driver class is firmware-
// only (#ifdef ARDUINO) and is exercised on the bench, not here.
// Run: pio test -e native-test

#include <unity.h>

#include "../src/hal/mic_poc.h"
#include "../src/hal/tcrt_poc.h"
#include "config.h"

void test_mic_unpack_top24_in_32() {
  // INMP441 left-justifies its 24-bit sample (bottom 8 zero): arithmetic
  // >>8 sign-extends into int32. Mirrors core's vectors — the POC copy must
  // agree with the production format header.
  TEST_ASSERT_EQUAL_INT32(0, micpoc::unpackSample(0x00000000));
  TEST_ASSERT_EQUAL_INT32(0x123456, micpoc::unpackSample(0x12345600));
  TEST_ASSERT_EQUAL_INT32(8388607, micpoc::unpackSample(0x7FFFFF00));
  TEST_ASSERT_EQUAL_INT32(-8388608, micpoc::unpackSample(0x80000000));
  TEST_ASSERT_EQUAL_INT32(-1, micpoc::unpackSample(0xFFFFFF00));
}

void test_mic_slot_rms_reads_one_slot() {
  // Interleaved stereo: tone in LEFT only -> rmsL reads amplitude, rmsR 0.
  // (Stage 1 bench expectation for a single L/R→GND board.)
  uint32_t frames[8];
  for (uint32_t i = 0; i < 4; ++i) {
    frames[2 * i] = 0x00040000;  // +1024 int24 in the top 24 bits
    frames[2 * i + 1] = 0x00000000;
  }
  TEST_ASSERT_EQUAL_UINT32(1024, micpoc::slotRms(frames, 4, micpoc::kLeftSlot));
  TEST_ASSERT_EQUAL_UINT32(0, micpoc::slotRms(frames, 4, micpoc::kRightSlot));
}

void test_mic_slot_rms_rejects_bad_input() {
  uint32_t frames[2] = {0x00040000, 0x00040000};
  TEST_ASSERT_EQUAL_UINT32(0, micpoc::slotRms(nullptr, 1, micpoc::kLeftSlot));
  TEST_ASSERT_EQUAL_UINT32(0, micpoc::slotRms(frames, 0, micpoc::kLeftSlot));
  TEST_ASSERT_EQUAL_UINT32(0, micpoc::slotRms(frames, 1, micpoc::kChannels));  // no such slot
}

void test_spk_sine_shape_and_click_ramp() {
  // 200ms beep @440Hz/16kHz: 3200 samples, ~88 cycles — halves contain many
  // peaks, and the envelope is flat in the middle, so both halves read loud.
  const int16_t peak = 1500;
  const uint32_t total = 3200;
  int32_t maxFirst = 0, maxSecond = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const int16_t s = spkpoc::synthSine(i, total, 440, 16000, peak);
    TEST_ASSERT_TRUE(s <= peak && s >= -peak);  // never clips past peak
    const int32_t a = s < 0 ? -s : s;
    if (i < total / 2) {
      if (a > maxFirst) maxFirst = a;
    } else {
      if (a > maxSecond) maxSecond = a;
    }
  }
  TEST_ASSERT_TRUE(maxFirst > peak / 2);                                                 // the beep actually sounds
  TEST_ASSERT_TRUE(maxSecond > peak / 2);                                                // ...and sustains (no decay)
  TEST_ASSERT_INT16_WITHIN(peak / 4, 0, spkpoc::synthSine(0, total, 440, 16000, peak));  // attack starts ~0
  TEST_ASSERT_EQUAL_INT16(0, spkpoc::synthSine(100, total, 0, 16000, peak));             // rest note
  TEST_ASSERT_EQUAL_INT16(0, spkpoc::synthSine(0, 0, 440, 16000, peak));                 // empty note
}

void test_spk_pack_dual_mono32() {  // int16 synth lands top-justified in 32-bit slots, identical L+R (same
  // contract as core's speaker::packMono32).
  const int16_t mono[4] = {1, -2, 1500, 0};
  int32_t stereo[8] = {};
  spkpoc::packMono32(mono, stereo, 4);
  for (uint32_t i = 0; i < 4; ++i) {
    const int32_t expect = static_cast<int32_t>(mono[i]) << 16;
    TEST_ASSERT_EQUAL_INT32(expect, stereo[2 * i]);
    TEST_ASSERT_EQUAL_INT32(expect, stereo[2 * i + 1]);
  }
  spkpoc::packMono32(nullptr, stereo, 4);  // null-safe no-op
  spkpoc::packMono32(mono, nullptr, 4);
}

void test_demo_bar_mapping() {
  // Layout contract for the in-place dashboard: 16-char bars, silence
  // empty, room floor a stub or two, rub/clap pegs full.
  TEST_ASSERT_EQUAL_UINT32(16, demopoc::kBarWidth);
  TEST_ASSERT_EQUAL_UINT32(0, demopoc::barChars(0));
  TEST_ASSERT_EQUAL_UINT32(0, demopoc::barChars(3999));
  TEST_ASSERT_EQUAL_UINT32(1, demopoc::barChars(4000));
  TEST_ASSERT_EQUAL_UINT32(2, demopoc::barChars(9000));
  TEST_ASSERT_EQUAL_UINT32(16, demopoc::barChars(64000));
  TEST_ASSERT_EQUAL_UINT32(16, demopoc::barChars(1500000));  // clamped, never overflows
}

void test_demo_verdict_latches_on_peak() {
  TEST_ASSERT_FALSE(demopoc::slotLive(0));
  TEST_ASSERT_FALSE(demopoc::slotLive(19999));
  TEST_ASSERT_TRUE(demopoc::slotLive(20000));
  TEST_ASSERT_TRUE(demopoc::slotLive(1500000));
}

void test_tcrt_tap_is_adc1_wifi_safe() {
  // GPIO33: ADC1 (analog reads never fight WiFi), clear of I2C/audio/LED.
  TEST_ASSERT_EQUAL_INT(33, CFG_TCRT_PIN);
  TEST_ASSERT_GREATER_THAN_INT(0, CFG_TCRT_READ_MS);
}

void test_tcrt_rail_assert_low_bands() {  // Measured bands (white ~20-170, void ~4095) vs rail 600/200 assert-low:
  // assert at <= 400, clear at >= 800, hold between.
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(false, 170, 600, 200, false));   // white asserts
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(false, 400, 600, 200, false));   // edge asserts
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(false, 401, 600, 200, false));  // just above holds clear
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(true, 4095, 600, 200, false));  // void clears
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(true, 800, 600, 200, false));   // edge clears
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(true, 799, 600, 200, false));    // just below holds asserted
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(true, 600, 600, 200, false));    // middle holds either way
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(false, 600, 600, 200, false));
}

void test_tcrt_subtract_clamps_at_zero() {
  // Tap sinks with reflection: emitter-on reads LOWER, signal = off - on.
  TEST_ASSERT_EQUAL_UINT16(3900, tcrtpoc::subtractSignal(195, 4095));  // white: strong reflection
  TEST_ASSERT_EQUAL_UINT16(0, tcrtpoc::subtractSignal(4095, 4095));    // void: nothing to subtract
  TEST_ASSERT_EQUAL_UINT16(0, tcrtpoc::subtractSignal(3000, 100));     // inverted (noise) clamps, never wraps
  TEST_ASSERT_EQUAL_UINT16(0, tcrtpoc::subtractSignal(0, 0));
}

void test_tcrt_rail_assert_high_signal_space() {
  // Signal-space rail 2000/500 assert-high: assert at >= 2500, clear at <=
  // 1500, hold between. Predicted bands: white ~4000, void ~0.
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(false, 3900, 2000, 500, true));   // white asserts
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(false, 2500, 2000, 500, true));   // edge asserts
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(false, 2499, 2000, 500, true));  // just below holds clear
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(true, 50, 2000, 500, true));     // void clears
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(true, 1500, 2000, 500, true));   // edge clears
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(true, 1501, 2000, 500, true));    // just above holds asserted
  TEST_ASSERT_TRUE(tcrtpoc::railUpdate(true, 2000, 2000, 500, true));    // middle holds either way
  TEST_ASSERT_FALSE(tcrtpoc::railUpdate(false, 2000, 2000, 500, true));
}
