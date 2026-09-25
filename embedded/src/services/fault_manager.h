#pragma once
// desky v2 fault manager — header-only, owns failure policy.
//
// Layering: logger formats/ships text (no side effects); this module
// decides halt vs reboot, watchdog lifecycle, and OOM response. It calls
// the logger, never the reverse.
//
// Usage:
//   FaultManager::watchdogInit();   // setup(), subscribes caller (loopTask)
//   FaultManager::watchdogFeed();   // loop() / each task body
//   DESKY_ASSERT(ptr != nullptr);   // dev: halt (WDT released first so the
//                                   // halt is a halt, not a reboot loop)
//                                   // release: flush + ESP.restart()

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <rom/ets_sys.h>

#include "services/logger.h"

class FaultManager {
 public:
  static constexpr uint32_t kWatchdogTimeoutSec = 5;

  static void watchdogInit(uint32_t timeoutSec = kWatchdogTimeoutSec) {
    esp_task_wdt_config_t config = {
        .timeout_ms = timeoutSec * 1000,
        .idle_core_mask = 0,  // We subscribe tasks explicitly; idle tasks stay out.
        .trigger_panic = true,
    };
    esp_task_wdt_init(&config);
    esp_task_wdt_add(nullptr);
  }

  static void watchdogFeed() { esp_task_wdt_reset(); }

  static void registerAllocFailureHook() { heap_caps_register_failed_alloc_callback(&onAllocFailed); }

  // Short reset-cause string for the boot banner (IDF idiom).
  static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
      case ESP_RST_UNKNOWN:
        return "UNKNOWN";
      case ESP_RST_POWERON:
        return "POWERON";
      case ESP_RST_EXT:
        return "EXT";
      case ESP_RST_SW:
        return "SW";
      case ESP_RST_PANIC:
        return "PANIC";
      case ESP_RST_INT_WDT:
        return "INT_WDT";
      case ESP_RST_TASK_WDT:
        return "TASK_WDT";
      case ESP_RST_WDT:
        return "WDT";
      case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
      case ESP_RST_BROWNOUT:
        return "BROWNOUT";
      case ESP_RST_SDIO:
        return "SDIO";
      case ESP_RST_USB:
        return "USB";
      default:
        return "UNKNOWN_N";
    }
  }

  [[noreturn]] static void fail(const char* file, int line, const char* expr) {
    Logger::log(LOG_LEVEL_ERROR, "ASSERT", "%s:%d %s", file, line, expr);
#if defined(DESKY_RELEASE)
    Serial.flush();
    ESP.restart();
    while (true) {
    }  // Unreachable; silences noreturn warnings on some toolchains.
#else
    // Dev: preserve the corpse for the debugger. Release the watchdog
    // first — a halted task stops feeding it, otherwise "halt" degrades
    // into a reboot loop and the evidence scrolls past.
    esp_task_wdt_delete(nullptr);
    while (true) {
      delay(1000);
    }
#endif
  }

 private:
  // Runs in allocation-failure context: ROM-safe print only (no heap,
  // no mutex), then restart. Logging here could deadlock on the log mutex.
  static void onAllocFailed(size_t requestedSize, uint32_t caps, const char* functionName) {
    ets_printf("OOM: alloc %u caps %lu in %s — restarting\n", (unsigned)requestedSize, (unsigned long)caps,
               functionName != nullptr ? functionName : "?");
    esp_restart();
  }
};

// Active in all envs — policy (halt vs reboot) differs, not existence.
#define DESKY_ASSERT(cond)                           \
  do {                                               \
    if (!(cond)) {                                   \
      FaultManager::fail(__FILE__, __LINE__, #cond); \
    }                                                \
  } while (0)
