#include <Arduino.h>

#include "config.h"
#include "services/diagnostics.h"
#include "services/fault_manager.h"
#include "services/logger.h"

void setup() {
  Serial.begin(115200);
  Logger::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
}

void loop() {
  FaultManager::watchdogFeed();
  LOG_I("MAIN", "desky v2 skeleton alive");
  Diagnostics::logWatermarks();
  delay(2000);
}
