#pragma once
// Speaker HAL (generic I2S DAC amp; reference part MAX98357A) — header-only,
// Arduino-free (stdint only). No peripheral code here: the shared duplex port
// lives in i2s_audio.h; this file owns the amp-side FORMAT (slot packing +
// channel strategy).
//
// Reference wiring: amp DIN on MCU_I2S_DOUT, VIN on 5V (keep levels quiet,
// see CFG_AUDIO_VOLUME). The amp decodes 16/24/32-bit words; the duplex port
// runs 32-bit slots (shared BCLK with the mics), so int16 synth lands
// top-justified.
//
// Channel strategy (floating SD_MODE defaults to the (L+R)/2 mix):
// packMono32 writes IDENTICAL L+R, so left / right / mix picks all reproduce
// the same sound and the strapping is moot. If the speaker ever goes silent,
// strap SD_MODE to 3V3 (left) first. Set loudness via CFG_AUDIO_VOLUME.

#include <stdint.h>

namespace speaker {

// I2S slot contract for the duplex port: 32-bit stereo. Keep equal to
// microphone::kSlotBitWidth or the port refuses to init.
constexpr uint32_t kSlotBitWidth = 32;
constexpr uint32_t kChannels = 2;

// Mono int16 synth -> interleaved 32-bit stereo frames, identical L+R
// (dual-mono: top-justified int16 plays at full scale).
inline void packMono32(const int16_t* mono, int32_t* stereoLR, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    const int32_t s = static_cast<int32_t>(mono[i]) << 16;
    stereoLR[2 * i] = s;
    stereoLR[2 * i + 1] = s;
  }
}

}  // namespace speaker
