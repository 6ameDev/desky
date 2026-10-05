// Host unit tests for I2S audio DSP + meter policy (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free part headers (hal/microphone.h: INMP441
// top-24-in-32 unpack; hal/speaker.h: 32-bit dual-mono pack for the amp's
// floating SD_MODE mix) and audiometer (src/services/audio_manager.h:
// bell-chime synth + per-channel RMS + louderSide deadband). The duplex I2S
// port (init/read/write), the AudioManager task, and the playPcm
// server-streaming stub are exercised on hardware.
// Runner lives in test_udp_codec.cpp.

#include <unity.h>

#include "../src/hal/microphone.h"
#include "../src/hal/speaker.h"
#include "../src/services/audio_manager.h"

void test_audio_int24_unpack_top24_in_32() {  // INMP441 left-justifies its 24-bit sample (bottom 8 zero): arithmetic
  // >>8 sign-extends into int32. HARDWARE ASSUMPTION — the clap test owns it.
  TEST_ASSERT_EQUAL_INT32(0, microphone::unpackSample(0x00000000));
  TEST_ASSERT_EQUAL_INT32(0x123456, microphone::unpackSample(0x12345600));
  TEST_ASSERT_EQUAL_INT32(8388607, microphone::unpackSample(0x7FFFFF00));   // max positive int24
  TEST_ASSERT_EQUAL_INT32(-8388608, microphone::unpackSample(0x80000000));  // min negative int24
  TEST_ASSERT_EQUAL_INT32(-1, microphone::unpackSample(0xFFFFFF00));        // -1 int24 stays -1
  TEST_ASSERT_EQUAL_INT32(-65536, microphone::unpackSample(0xFF000000));
}

void test_audio_dual_mono32_top_justified() {
  // Production TX path: int16 beeps land top-justified in 32-bit slots,
  // identical L+R (the amp decodes 16/24/32-bit words, so top-16 plays).
  const int16_t mono[4] = {1, -2, 3000, 0};
  int32_t stereo[8] = {};
  speaker::packMono32(mono, stereo, 4);
  for (uint32_t i = 0; i < 4; ++i) {
    const int32_t expect = static_cast<int32_t>(mono[i]) << 16;
    TEST_ASSERT_EQUAL_INT32(expect, stereo[2 * i]);
    TEST_ASSERT_EQUAL_INT32(expect, stereo[2 * i + 1]);
  }
  TEST_ASSERT_EQUAL_INT32(0x0BB80000, stereo[4]);  // 3000 << 16
}

void test_audio_chime_shape_and_decay() {
  // 220ms bell @659Hz/16kHz: 3520 samples, ~47 cycles — halves contain many
  // peaks, so the exponential decay guarantees a smaller second-half max.
  const int16_t peak = 10000;
  const uint32_t total = 3520;
  const int16_t first = audiometer::synthChime(0, total, 659, 16000, peak);
  TEST_ASSERT_INT16_WITHIN(peak / 16, 0, first);  // attack starts ~0 (no click)
  int32_t maxFirst = 0, maxSecond = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const int16_t s = audiometer::synthChime(i, total, 659, 16000, peak);
    TEST_ASSERT_TRUE(s <= peak && s >= -peak);  // partial sum never clips past peak
    const int32_t a = s < 0 ? -s : s;
    if (i < total / 2) {
      if (a > maxFirst) maxFirst = a;
    } else {
      if (a > maxSecond) maxSecond = a;
    }
  }
  TEST_ASSERT_TRUE(maxFirst > 0);                                                  // the bell actually rings
  TEST_ASSERT_TRUE(maxSecond < maxFirst);                                          // ...and audibly decays
  TEST_ASSERT_EQUAL_INT16(0, audiometer::synthChime(100, total, 0, 16000, peak));  // rest note
  TEST_ASSERT_EQUAL_INT16(0, audiometer::synthChime(0, 0, 659, 16000, peak));      // empty note
}

void test_audio_rms_and_louder_side() {
  // RMS: constant tone reads its amplitude, silence reads 0, empty reads 0.
  const int32_t tone[4] = {1000, 1000, 1000, 1000};
  TEST_ASSERT_EQUAL_UINT32(1000, audiometer::rmsInt24(tone, 4));
  const int32_t silence[4] = {0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT32(0, audiometer::rmsInt24(silence, 4));
  TEST_ASSERT_EQUAL_UINT32(0, audiometer::rmsInt24(nullptr, 0));
  // Direction: clear winners pick a side, ties (equal / silent / within the
  // ~1/8 deadband) hold TIE so noise never flaps the verdict.
  TEST_ASSERT_EQUAL(audiometer::SIDE_LEFT, audiometer::louderSide(8000, 1000));
  TEST_ASSERT_EQUAL(audiometer::SIDE_RIGHT, audiometer::louderSide(1000, 8000));
  TEST_ASSERT_EQUAL(audiometer::SIDE_TIE, audiometer::louderSide(4000, 4000));
  TEST_ASSERT_EQUAL(audiometer::SIDE_TIE, audiometer::louderSide(0, 0));
  TEST_ASSERT_EQUAL(audiometer::SIDE_TIE, audiometer::louderSide(10, 20));       // below the floor
  TEST_ASSERT_EQUAL(audiometer::SIDE_TIE, audiometer::louderSide(8000, 7500));   // inside deadband
  TEST_ASSERT_EQUAL(audiometer::SIDE_LEFT, audiometer::louderSide(8000, 6000));  // outside deadband
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the audio tests are declared extern there.
