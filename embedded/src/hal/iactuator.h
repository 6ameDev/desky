#pragma once
// Abstract actuator interface (v2 HAL). Phase 1 fills in drivers.

class IActuator {
 public:
  virtual ~IActuator() = default;
  virtual bool init() = 0;
  virtual void setPowerState(bool enable) = 0;
  virtual bool isEnabled() const = 0;
};
