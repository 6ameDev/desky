#pragma once
// Stack/heap watermark logger: call in each task body and loop().
// Largest-block vs total-free divergence is the fragmentation canary.

#include <Arduino.h>

#include "common/logger.h"

class Diagnostics {
 public:
  // Caller-task stack watermark + heap free + largest free block.
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
