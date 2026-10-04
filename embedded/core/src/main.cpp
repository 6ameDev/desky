#include <Arduino.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "middleware/poc_link.h"

// desky-core UART POC receiver (poc-comm-link spike, replaces the v2 brain shell).
// Setup order (core convention): logger -> fault -> config -> banner — nothing
// that can fail runs before logging exists.
//
// Stripped to transport verification only: Serial + UART1 link ONLY
// (RX=GPIO18, TX=GPIO17 tristated until the first valid Head frame, which
// protects CAM GPIO12 strapping). NO motor/I2C/ToF/MPU/UDP/WiFi.
//
// v2 brain recoverable via `git checkout <v2> -- embedded/core/src/main.cpp`.

namespace {
PocLink g_poc;
}  // namespace

void setup() {
  Serial.begin(115200);
  Logger::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  g_poc.begin();  // Config defaults + RX-only Serial1 + INTERNAL buffers.
  // fw= marker: manual firmware identity (bump on every firmware change so a
  // console banner proves which image runs — hash verifies bytes-written, not
  // source-identity). Pinned in host tests (test_uart_frame.cpp fwmarker).
  LOG_I("BOOT", "desky-core uart-poc fw=s3-006-ring16+split | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME,
        MCU_NUM_CORES, MCU_FLASH_SIZE_MB, MCU_HAS_PSRAM);
  LOG_I("BOOT", "pins link_rx=%d link_tx=%d(tristated until first head frame)", MCU_LINK_UART_RX, MCU_LINK_UART_TX);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  LOG_I("BOOT", "usb: STATS | RESET | SET k v | GET k | GET all | HEAD SET k v | HEAD GET k | HEAD STATS | HELP");
}

void loop() {
  FaultManager::watchdogFeed();
  g_poc.poll();  // Link RX pump + USB CLI.
  delay(1);
}
