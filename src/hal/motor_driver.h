#pragma once
// desky v2 L0 motor driver (DRV8833 dual-H-bridge) — header-only HAL.
//
// Ports the PROVEN V1 logic (main:src/drivers/MotorDriver.{h,cpp} +
// main:include/Config.h): IN1=25, IN2=26, IN3=18, IN4=19 (LEDC 20kHz
// 8-bit, Arduino-3.x ledcAttach(pin,freq,res)+ledcWrite API),
// FAULT=27 INPUT_PULLUP active-low, MIN_PWM=65 floor, brake=all-255.
// Pins here are cross-checked against include/mcu/esp32_devkit_v1_30pin.h
// (MCU_MOTOR_IN1/IN2/IN3/IN4/FAULT, MCU_PWM_*): they match, no conflict.
//
// Implements IActuator (init/setPowerState/isEnabled) plus motor verbs:
// setDuty(left,right) signed -255..255, brake(), coast(), fault().
//
// Power model: the board wires no SLEEP/EEP pin, so setPowerState(false)
// is logical: it coasts the bridge (high-Z, same as V1 zero-drive) and
// gates setDuty(); brake()/coast() still actuate so an e-stop can always
// force a stop state. init() failure DESKY_ASSERTs (fault_manager.h).

#include <Arduino.h>
#include <stdint.h>

#include "config.h"
#include "hal/iactuator.h"
#include "services/fault_manager.h"

class MotorDriver : public IActuator {
 public:
  MotorDriver(int in1, int in2, int in3, int in4, int faultPin, uint32_t freqHz = CFG_PWM_FREQ_HZ,
              uint8_t resBits = CFG_PWM_RES_BITS, int minDuty = CFG_PWM_MIN_DUTY)
      : in1_(in1),
        in2_(in2),
        in3_(in3),
        in4_(in4),
        faultPin_(faultPin),
        freqHz_(freqHz),
        resBits_(resBits),
        minDuty_(minDuty),
        enabled_(false) {}

  bool init() override {
    pinMode(faultPin_, INPUT_PULLUP);
    const bool ok = ledcAttach(in1_, freqHz_, resBits_) && ledcAttach(in2_, freqHz_, resBits_) &&
                    ledcAttach(in3_, freqHz_, resBits_) && ledcAttach(in4_, freqHz_, resBits_);
    DESKY_ASSERT(ok);
    if (!ok) {
      return false;
    }
    enabled_ = true;
    coast();
    return true;
  }

  void setPowerState(bool enable) override {
    if (enable == enabled_) {
      return;
    }
    enabled_ = enable;
    // Both transitions land in a safe stop state (coast == V1 zero-drive).
    coast();
  }

  bool isEnabled() const override { return enabled_; }

  // Signed open-loop duty per side, -255..255 (clamped). Zero (after the
  // V1 deadband) coasts that side; use brake() for an active short-stop.
  void setDuty(int left, int right) {
    if (!enabled_) {
      return;
    }
    left = applyFloor(clampDuty(left));
    right = applyFloor(clampDuty(right));
    driveSide(in1_, in2_, left);
    driveSide(in3_, in4_, right);
  }

  void brake() {
    ledcWrite(in1_, 255);
    ledcWrite(in2_, 255);
    ledcWrite(in3_, 255);
    ledcWrite(in4_, 255);
  }

  void coast() {
    ledcWrite(in1_, 0);
    ledcWrite(in2_, 0);
    ledcWrite(in3_, 0);
    ledcWrite(in4_, 0);
  }

  bool fault() const { return digitalRead(faultPin_) == LOW; }

 private:
  static int clampDuty(int d) {
    if (d > 255) {
      return 255;
    }
    if (d < -255) {
      return -255;
    }
    return d;
  }

  // V1 applyFloor, integer-identical: |d|<10 -> 0, else linear
  // map 10..255 -> minDuty..255 preserving sign.
  int applyFloor(int speed) const {
    int mag = speed >= 0 ? speed : -speed;
    if (mag < 10) {
      return 0;
    }
    const int span = 255 - 10;
    int scaled = minDuty_ + (mag - 10) * (255 - minDuty_) / span;
    if (scaled > 255) {
      scaled = 255;
    }
    return speed > 0 ? scaled : -scaled;
  }

  static void driveSide(int pinFwd, int pinRev, int duty) {
    if (duty > 0) {
      ledcWrite(pinFwd, static_cast<uint32_t>(duty));
      ledcWrite(pinRev, 0);
    } else if (duty < 0) {
      ledcWrite(pinFwd, 0);
      ledcWrite(pinRev, static_cast<uint32_t>(-duty));
    } else {
      ledcWrite(pinFwd, 0);
      ledcWrite(pinRev, 0);
    }
  }

  int in1_;
  int in2_;
  int in3_;
  int in4_;
  int faultPin_;
  uint32_t freqHz_;
  uint8_t resBits_;
  int minDuty_;
  bool enabled_;
};
