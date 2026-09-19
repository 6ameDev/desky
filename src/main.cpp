#include <Arduino.h>

#include "config.h"
#include "services/logger.h"

void setup() {
  Serial.begin(115200);
  Logger::begin();
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME,
        MCU_NUM_CORES, MCU_FLASH_SIZE_MB, MCU_HAS_PSRAM);
}

void loop() {
  LOG_I("MAIN", "desky v2 skeleton alive");
  delay(2000);
}
