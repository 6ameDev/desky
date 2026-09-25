#pragma once
// desky v2 diagnostics — header-only, ESP-IDF primitives only.
//
// Memory discipline rule (see also logger.h): no String/new/malloc in
// tasks/loops. Stack buffers + vsnprintf only. This helper makes the
// habit measurable from day 1: call logWatermarks() in each task body
// (and loop()) so stack/heap pressure is visible long before Phase 2.

#include <Arduino.h>

#include "services/logger.h"

class Diagnostics {
 public:
  // Logs caller-task stack watermark + heap free + largest free block.
  // Largest-block < total-free divergence is the fragmentation canary.
  static void logWatermarks(const char* tag = "MEM") {
#if defined(portTICK_PERIOD_MS)
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
#else
    UBaseType_t watermark = 0;
#endif
    size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    LOG_D(tag, "stack_wm=%u heap_free=%u largest=%u", (unsigned)watermark, (unsigned)freeBytes, (unsigned)largestBlock);
  }
};
