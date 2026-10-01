#include <Arduino.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "services/poc_manager.h"

// desky-head UART POC sender (poc-comm-link spike, replaces the face-unit shell).
// Setup order (core convention): logger -> fault -> config -> banner — nothing
// that can fail runs before logging exists.
//
// Stripped to transport verification only: NO I2C/scan/OLED/WiFi, NO camera
// init at boot (lazy on first mode-3 tick). Link UART2 TX=GPIO12/RX=GPIO13.
//
// v2 shell recoverable via `git checkout <v2> -- embedded/head/src/main.cpp`.

#ifndef CFG_CLI_BAUD
#define CFG_CLI_BAUD 115200
#endif

namespace {
PocManager g_poc;
}  // namespace

void setup() {
  Serial.begin(CFG_CLI_BAUD);
  Logger::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  g_poc.begin();  // Config defaults + Serial2 + INTERNAL buffers + generator task.
  LOG_I("BOOT", "desky-head uart-poc | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "pins link_tx=%d link_rx=%d (CAM-TX output keeps GPIO12 strapping LOW)", MCU_LINK_UART_TX,
        MCU_LINK_UART_RX);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  LOG_I("BOOT", "usb: SET k v | GET k | GET all | STATS | START | STOP | HELP");
}

void loop() {
  FaultManager::watchdogFeed();
  g_poc.poll();  // USB CLI + link CMD RX + HB tick + baud state machine.
}
