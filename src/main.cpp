#include <Arduino.h>

#include "behavior/motion_controller.h"
#include "config.h"
#include "hal/motor_driver.h"
#include "services/config_store.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/logger.h"

namespace {
MotorDriver g_motorDriver(MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3, MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
MotionController g_motion;
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
