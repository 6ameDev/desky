#pragma once
// desky v2 I2C manager — header-only, owns the shared bus lifecycle.
//
// Bus-agnostic by construction: drivers hold an I2CManager reference and
// reach Wire only through it (bus().wire()), never a global directly.
// A second bus later (e.g. OLED on Wire1 via MCU_I2C1_SDA/SCL) is a second
// instance — no driver changes.
//
// Locking model (mirrors proven V1 i2cMutex use): one FreeRTOS mutex per
// bus. acquire() with a short wait (default 5ms, V1's budget — OLED holds
// ~23ms); on failure the caller SKIPS the cycle and keeps last-good data,
// never blocks the control loop. The mutex is NEVER held across delays:
// device boot/XSHUT waits happen outside acquire/release pairs.
//
// Recovery primitives port V1 tofPulseClock/tofReinit (main:src/main.cpp):
// pulseClock() frees a subordinate stuck holding SDA low (9 SCL pulses),
// reinit() re-attaches the peripheral. Device re-begin after reinit is the
// DRIVER's job (reinit is bus-level only).

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "services/logger.h"

class I2CManager {
 public:
  I2CManager(TwoWire& wire = Wire, int sda = CFG_I2C_SDA, int scl = CFG_I2C_SCL, uint32_t freqHz = CFG_I2C_FREQ_HZ,
             uint32_t timeoutMs = CFG_I2C_TIMEOUT_MS)
      : wire_(wire), sda_(sda), scl_(scl), freqHz_(freqHz), timeoutMs_(timeoutMs), mutex_(nullptr) {}

  // Creates the bus mutex and brings the peripheral up. Call once in setup()
  // before any driver init. Returns false only if the mutex cannot be made.
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      LOG_E("I2C", "mutex create failed");
      return false;
    }
    busInit();
    LOG_I("I2C", "bus0 up sda=%d scl=%d freq=%luHz timeout=%lums", sda_, scl_, freqHz_, timeoutMs_);
    return true;
  }

  // (Re)attach the peripheral: end + begin + timeout + clock.
  // No delays, no locking — safe to call from recovery paths.
  void busInit() {
    wire_.end();
    wire_.begin(sda_, scl_);
    wire_.setTimeOut(timeoutMs_);
    wire_.setClock(freqHz_);
  }

  // Bus-level reinit after pulseClock() (V1 tofReinit's Wire half).
  void reinit() { busInit(); }

  // Take the bus. Returns false on timeout — skip the cycle, keep last-good.
  bool acquire(uint32_t waitMs = 5) {
    if (mutex_ == nullptr) {
      return true;
    }
    return xSemaphoreTake(mutex_, pdMS_TO_TICKS(waitMs)) == pdTRUE;
  }

  void release() {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  TwoWire& wire() { return wire_; }
  int sda() const { return sda_; }
  int scl() const { return scl_; }

  // 9 SCL pulses to release a stuck subordinate (V1 tofPulseClock,
  // generalized to this bus's pins). Leaves pins in GPIO mode — the caller
  // must follow with reinit() to hand them back to the peripheral.
  void pulseClock() {
    pinMode(scl_, OUTPUT);
    pinMode(sda_, INPUT_PULLUP);
    digitalWrite(scl_, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9; ++i) {
      digitalWrite(scl_, LOW);
      delayMicroseconds(5);
      digitalWrite(scl_, HIGH);
      delayMicroseconds(5);
    }
  }

  // Setup-time visibility: logs every ACKing address. Not for the poll path
  // (holds the mutex across the whole sweep, no delays inside).
  void scanBus() {
    if (!acquire(100)) {
      LOG_W("I2C", "scan skipped, bus busy");
      return;
    }
    LOG_I("I2C", "scanning bus...");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
      wire_.beginTransmission(addr);
      if (wire_.endTransmission() == 0) {
        LOG_I("I2C", "found device at 0x%02X", addr);
        ++found;
      }
    }
    if (found == 0) {
      LOG_W("I2C", "no devices found");
    }
    release();
  }

 private:
  TwoWire& wire_;
  int sda_;
  int scl_;
  uint32_t freqHz_;
  uint32_t timeoutMs_;
  SemaphoreHandle_t mutex_;
};
