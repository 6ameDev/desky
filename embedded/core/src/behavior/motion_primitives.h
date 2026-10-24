#pragma once
// Motion primitives for expression bundles — behavior/.
//
// Arduino-free key->params table over the shared catalog motion ids: the
// expression conductor maps a row's motion field to startWiggle/stop args
// through here. MotionController itself is untouched (existing startWiggle +
// stop only). NONE/unknown ids map to still.
#include <stdint.h>

#include "link/expression_catalog.h"

namespace motionprim {

struct Primitive {
  bool still = true;  // True: hold stop, never arm a wiggle.
  uint32_t durationMs = 0;
  float rateHz = 0.0f;
  float amplitude = 0.0f;
};

// Shake: fast short wiggle. Cower: still. Bounce: ~1200ms happy wiggle.
// Tilt: slow sway.
inline Primitive forId(uint8_t motionId) {
  switch (motionId) {
    case exprcat::kMotionShake:
      return {false, 600, 6.0f, 0.8f};
    case exprcat::kMotionCower:
      return {true, 0, 0.0f, 0.0f};
    case exprcat::kMotionBounce:
      return {false, 1200, 2.0f, 0.6f};
    case exprcat::kMotionTilt:
      return {false, 1200, 0.8f, 0.5f};
    default:
      return {true, 0, 0.0f, 0.0f};
  }
}

}  // namespace motionprim
