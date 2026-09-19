#include <Arduino.h>

#include "config.h"
#include "services/config_store.h"
#include "services/diagnostics.h"
#include "services/fault_manager.h"
#include "services/logger.h"

void setup() {
  Serial.begin(115200);
  Logger::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  ConfigStore::begin();
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
