#include <Arduino.h>

#include "behavior/motion_controller.h"
#include "config.h"
#include "hal/motor_driver.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "services/config_store.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/i2c_manager.h"
#include "services/logger.h"

namespace {
MotorDriver g_motorDriver(MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3, MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
MotionController g_motion;
I2CManager g_i2c;
Mpu6500Driver g_mpu(g_i2c);
Vl53l0xDriver g_tof(g_i2c);
}  // namespace

void setup() {
  Serial.begin(115200);
  Logger::begin();
  EventBus::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  ConfigStore::begin();
  const bool motorOk = g_motorDriver.init();
  DESKY_ASSERT(motorOk);
  const bool motionOk = g_motion.begin(&g_motorDriver);
  DESKY_ASSERT(motionOk);
  const bool i2cOk = g_i2c.begin();
  DESKY_ASSERT(i2cOk);
  g_i2c.scanBus();
  const bool tofOk = g_tof.init();
  DESKY_ASSERT(tofOk);
  const bool mpuOk = g_mpu.init();
  DESKY_ASSERT(mpuOk);
  LOG_I("BOOT", "sensors ready tof=%dmm mpu=0x%02X", g_tof.distanceMm(), g_mpu.address());
  LOG_I("BOOT", "motor L0+L1 ready pins=%d,%d,%d,%d fault=%d", MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3,
        MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  LOG_I("BOOT", "cpu=%dMHz flash_chip=%dB", ESP.getCpuFreqMHz(), ESP.getFlashChipSize());
}

void loop() {
  FaultManager::watchdogFeed();
  LOG_I("MAIN", "desky v2 skeleton alive");
  Diagnostics::logWatermarks();
  delay(2000);
}
