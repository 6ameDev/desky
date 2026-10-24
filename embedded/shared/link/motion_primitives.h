#pragma once
// Single keyed table: catalog motion id -> wiggle params; NONE/unknown ids map to still.
#include <stdint.h>

#include "link/expression_catalog.h"

namespace motionprim {

struct Primitive {
  bool still = true;
  uint32_t durationMs = 0;
  float rateHz = 0.0f;
  float amplitude = 0.0f;
};

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
