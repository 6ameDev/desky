#pragma once
// desky v2 MPU6500 driver (behind ISensor) — header-only HAL.
//
// Ports the PROVEN V1 probe path (main:src/main.cpp mpuProbeAddr/mpuProbe):
// dual-address probe (0x68 primary, 0x69 fallback, MCU defines only),
// then 4G accel / 500DPS gyro / 20Hz DLPF via the pinned Bolderflight lib.
// V1-vs-new deltas:
// - WHO_AM_I (reg 0x75) must read 0x70 or the address is rejected, and
//   init() DESKY_ASSERTs the ID after a successful probe (AGENTS.md
//   silkscreen rule — V1's "MPU6050" is a 6500; the lib's Begin() checks
//   the ID internally too, this is the explicit silicon assert).
// - Orientation compensation (V1's -Z flip, FWD/RIGHT mapping, pitch/roll)
//   is NOT done here: the driver reports sensor-frame raw. Tilt math owns
//   to fusion. Units are documented on the struct: accel in g, gyro in dps.
// - V1's mpuEnabled/reprobe gating lives with the power manager / sensor
//   task later; update() here only skips when the bus is contended.
//
// Power: enable-flag only (setPowerState gates update()). Hardware
// sleep-mode is deferred to the power-manager work (documented, not wired).
//
// Rules: no String/heap in update(), no hardcoded addresses, mutex never
// held across delays (init retries delay OUTSIDE the lock).

#include <Arduino.h>
#include <mpu6500.h>

#include "config.h"
#include "hal/isensor.h"
#include "services/fault_manager.h"
#include "services/i2c_manager.h"
#include "services/logger.h"

// Raw sensor-frame reading. Units: ax/ay/az in g, gx/gy/gz in dps.
struct MpuReading {
  float ax;
  float ay;
  float az;
  float gx;
  float gy;
  float gz;
};

class Mpu6500Driver : public ISensor {
 public:
  static constexpr uint8_t kWhoAmIReg = 0x75;
  static constexpr uint8_t kWhoAmI6500 = 0x70;
  static constexpr int kInitAttempts = 3;         // V1 setup retry budget
  static constexpr uint32_t kRetryDelayMs = 100;  // V1 setup retry gap

  explicit Mpu6500Driver(I2CManager& bus) : bus_(bus), addr_(0), enabled_(false), healthy_(false) {}

  // Dual-addr probe with V1's 3-attempt budget. Returns false (no halt here)
  // so setup() applies the motor-task policy: DESKY_ASSERT on failure.
  bool init() override {
    for (int attempt = 0; attempt < kInitAttempts && addr_ == 0; ++attempt) {
      if (attempt > 0) {
        delay(kRetryDelayMs);
      }
      probe();
    }
    if (addr_ == 0) {
      LOG_E("MPU", "MPU6500 not found at 0x%02X/0x%02X", MCU_ADDR_MPU_PRIMARY, MCU_ADDR_MPU_ALT);
      return false;
    }
    // Explicit silicon assert (AGENTS.md silkscreen rule).
    DESKY_ASSERT(readWhoAmI(addr_) == kWhoAmI6500);
    enabled_ = true;
    LOG_I("MPU", "MPU6500 ready at 0x%02X (4G/500DPS/20Hz DLPF)", addr_);
    return true;
  }

  // Single non-blocking poll: skip the cycle if the bus is held (OLED
  // ~23ms), keep the last reading. Lock is released before any conversion.
  void update() override {
    if (!enabled_) {
      return;
    }
    if (!bus_.acquire()) {
      return;
    }
    const bool ok = mpu_.Read();
    bus_.release();
    if (!ok) {
      healthy_ = false;
      return;
    }
    static constexpr float kMps2PerG = 9.80665f;
    static constexpr float kRadToDeg = 57.29578f;
    reading_.ax = mpu_.accel_x_mps2() / kMps2PerG;
    reading_.ay = mpu_.accel_y_mps2() / kMps2PerG;
    reading_.az = mpu_.accel_z_mps2() / kMps2PerG;
    reading_.gx = mpu_.gyro_x_radps() * kRadToDeg;
    reading_.gy = mpu_.gyro_y_radps() * kRadToDeg;
    reading_.gz = mpu_.gyro_z_radps() * kRadToDeg;
    healthy_ = true;
  }

  // Enable-flag power only; hardware sleep-mode is power-manager work.
  void setPowerState(bool enable) override {
    enabled_ = enable;
    if (!enabled_) {
      healthy_ = false;
    }
  }

  bool isEnabled() const override { return enabled_; }
  uint32_t getTargetIntervalMs() const override { return CFG_MPU_TARGET_INTERVAL_MS; }

  const MpuReading& reading() const { return reading_; }
  bool isHealthy() const { return healthy_; }
  uint8_t address() const { return addr_; }

 private:
  bool probe() { return probeAddr(MCU_ADDR_MPU_PRIMARY) || probeAddr(MCU_ADDR_MPU_ALT); }

  bool probeAddr(uint8_t addr) {
    const uint8_t who = readWhoAmI(addr);
    LOG_D("MPU", "probe 0x%02X WHO_AM_I=0x%02X", addr, who);
    if (who != kWhoAmI6500) {
      return false;
    }
    // V1 order: Config, Begin, then range/DLPF programming.
    mpu_.Config(&bus_.wire(), addr == MCU_ADDR_MPU_ALT ? bfs::Mpu6500::I2C_ADDR_SEC : bfs::Mpu6500::I2C_ADDR_PRIM);
    if (!mpu_.Begin()) {
      return false;
    }
    if (!mpu_.ConfigAccelRange(bfs::Mpu6500::ACCEL_RANGE_4G)) {
      return false;
    }
    if (!mpu_.ConfigGyroRange(bfs::Mpu6500::GYRO_RANGE_500DPS)) {
      return false;
    }
    if (!mpu_.ConfigDlpfBandwidth(bfs::Mpu6500::DLPF_BANDWIDTH_20HZ)) {
      return false;
    }
    addr_ = addr;
    LOG_I("MPU", "probe found MPU6500 at 0x%02X", addr_);
    return true;
  }

  uint8_t readWhoAmI(uint8_t addr) {
    if (!bus_.acquire()) {
      return 0xFF;
    }
    TwoWire& wire = bus_.wire();
    uint8_t who = 0xFF;
    wire.beginTransmission(addr);
    wire.write(kWhoAmIReg);
    if (wire.endTransmission(false) == 0 && wire.requestFrom(addr, static_cast<uint8_t>(1)) == 1) {
      who = wire.read();
    }
    bus_.release();
    return who;
  }

  I2CManager& bus_;
  bfs::Mpu6500 mpu_;
  MpuReading reading_{};
  uint8_t addr_;
  bool enabled_;
  bool healthy_;
};
