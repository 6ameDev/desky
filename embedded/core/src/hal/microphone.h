#pragma once
// Microphone HAL (generic I2S mic; reference part INMP441) — header-only,
// Arduino-free (stdint only). No peripheral code here: the shared duplex port
// lives in i2s_audio.h; this file owns the mic-side FORMAT (slot layout +
// channel map).
//
// Reference wiring: two mics on the one SD line (MCU_I2S_DIN), stereo by
// hardware tie — GND-tied mic drives the LEFT slot, 3V3-tied mic the RIGHT
// slot. 24-bit samples left-justified in 32-bit slots.
//
// HARDWARE ASSUMPTION (flagged for bench confirmation): the >>8 below.
// Confirm with the clap test (one loud clap per mic: expect a large
// asymmetric RMS spike on that side only). If the part turns out
// right-justified or 16-bit, this shift changes — not the callers.

#include <stdint.h>

namespace microphone {

// I2S slot contract for the duplex port: 32-bit stereo. Keep equal to
// speaker::kSlotBitWidth or the port refuses to init.
constexpr uint32_t kSlotBitWidth = 32;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kLeftSlot = 0;   // GND-tied mic by hardware tie.
constexpr uint32_t kRightSlot = 1;  // 3V3-tied mic by hardware tie.

// 32-bit slot -> signed sample: an arithmetic shift right by 8 sign-extends
// int24 into int32.
inline int32_t unpackSample(uint32_t rawSlot) { return static_cast<int32_t>(rawSlot) >> 8; }

}  // namespace microphone
