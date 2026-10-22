#pragma once
// Owns the shared I2C bus lifecycle. Drivers hold a reference, never the Wire global.
// acquire() times out fast: skip the cycle, keep last-good, never block. Never hold across delays.

#include <Arduino.h>
#include <Wire.h>

#include "common/logger.h"
#include "config.h"  // Per-firmware defaults: resolves to the including project's config.h (core or head).

class I2CManager {
 public:
  I2CManager(TwoWire& wire = Wire, int sda = CFG_I2C_SDA, int scl = CFG_I2C_SCL, uint32_t freqHz = CFG_I2C_FREQ_HZ,
             uint32_t timeoutMs = CFG_I2C_TIMEOUT_MS)
      : wire_(wire), sda_(sda), scl_(scl), freqHz_(freqHz), timeoutMs_(timeoutMs), mutex_(nullptr) {}

  // Call once in setup() before driver init.
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

  // (Re)attach the peripheral. No delays, no locking.
  void busInit() {
    wire_.end();
    wire_.begin(sda_, scl_);
    wire_.setTimeOut(timeoutMs_);
    wire_.setClock(freqHz_);
  }

  void reinit() { busInit(); }

  // False on timeout: skip the cycle, keep last-good.
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

  // 9 SCL pulses free a stuck subordinate. Leaves pins in GPIO mode: follow with reinit().
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

  // Setup-time only: holds the mutex across the whole sweep.
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
