#pragma once
// Abstract actuator interface.

class IActuator {
 public:
  virtual ~IActuator() = default;
  virtual bool init() = 0;
  virtual void setPowerState(bool enable) = 0;
  virtual bool isEnabled() const = 0;
};
