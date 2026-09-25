#pragma once
// Abstract sensor interface (v2 HAL). Phase 1 fills in drivers.
#include <stdint.h>

class ISensor {
 public:
  virtual ~ISensor() = default;
  virtual bool init() = 0;
  virtual void update() = 0;
  virtual bool isEnabled() const = 0;
  virtual void setPowerState(bool enable) = 0;
  virtual uint32_t getTargetIntervalMs() const = 0;
};
