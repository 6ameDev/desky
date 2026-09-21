// Host unit tests for the sensor task edge logic (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace sensortask (edge + hold + scheduling in
// src/middleware/sensor_task.h): transitions-only EVENT_CLIFF_DETECTED
// publish, frozen-input silence, and wrap-safe elapsed scheduling. The
// SensorTask FreeRTOS task (drivers/WDT) is firmware-only (#ifdef ARDUINO)
// and exercised on hardware. Runner lives in test_udp_codec.cpp.

#include <unity.h>

#include "../src/middleware/sensor_task.h"

namespace {

bool tryEdge(bool live, bool cliffNow, bool& latched, uint32_t& payload) {
  return sensortask::cliffTransition(live, cliffNow, latched, payload);
}

}  // namespace

void test_sensor_rising_publishes_once() {
  bool latched = false;  // boots clear
  uint32_t payload = 0;
  TEST_ASSERT_FALSE(tryEdge(true, false, latched, payload));  // held clear: silent
  TEST_ASSERT_TRUE(tryEdge(true, true, latched, payload));    // rising: fires nonzero
  TEST_ASSERT_EQUAL_UINT32(1, payload);
  TEST_ASSERT_TRUE(latched);
  TEST_ASSERT_FALSE(tryEdge(true, true, latched, payload));  // held cliff: silent
}

void test_sensor_held_levels_silent() {
  bool latched = false;
  uint32_t payload = 0;
  TEST_ASSERT_FALSE(tryEdge(true, false, latched, payload));
  TEST_ASSERT_FALSE(tryEdge(true, false, latched, payload));
  latched = true;
  TEST_ASSERT_FALSE(tryEdge(true, true, latched, payload));
  TEST_ASSERT_FALSE(tryEdge(true, true, latched, payload));
}

void test_sensor_falling_publishes_once() {
  bool latched = true;
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(tryEdge(true, false, latched, payload));  // falling: fires zero
  TEST_ASSERT_EQUAL_UINT32(0, payload);
  TEST_ASSERT_FALSE(latched);
  TEST_ASSERT_FALSE(tryEdge(true, false, latched, payload));  // held clear: silent
}

void test_sensor_reassert_after_clear() {
  bool latched = false;
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(tryEdge(true, true, latched, payload));
  TEST_ASSERT_EQUAL_UINT32(1, payload);
  TEST_ASSERT_TRUE(tryEdge(true, false, latched, payload));
  TEST_ASSERT_EQUAL_UINT32(0, payload);
  TEST_ASSERT_TRUE(tryEdge(true, true, latched, payload));  // reassert fires again
  TEST_ASSERT_EQUAL_UINT32(1, payload);
}

void test_sensor_boot_into_cliff_publishes_once() {
  bool latched = false;  // boot state is always clear
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(tryEdge(true, true, latched, payload));  // first live tick fires
  TEST_ASSERT_EQUAL_UINT32(1, payload);
  TEST_ASSERT_FALSE(tryEdge(true, true, latched, payload));
}

void test_sensor_frozen_inputs_silent() {
  bool latched = false;
  uint32_t payload = 0xDEAD;
  // Dead ticks never publish and never move the latch, even as inputs flip.
  TEST_ASSERT_FALSE(tryEdge(false, true, latched, payload));
  TEST_ASSERT_FALSE(latched);
  TEST_ASSERT_FALSE(tryEdge(false, false, latched, payload));
  TEST_ASSERT_FALSE(tryEdge(false, true, latched, payload));
  TEST_ASSERT_FALSE(latched);
  // Live tick on the held level stays silent; only a real live flip fires.
  TEST_ASSERT_FALSE(tryEdge(true, false, latched, payload));
  TEST_ASSERT_TRUE(tryEdge(true, true, latched, payload));
  TEST_ASSERT_EQUAL_UINT32(1, payload);
  // A dead tick mid-latch cannot clear it: no falling edge is synthesized.
  TEST_ASSERT_FALSE(tryEdge(false, false, latched, payload));
  TEST_ASSERT_TRUE(latched);
  TEST_ASSERT_TRUE(tryEdge(true, false, latched, payload));
  TEST_ASSERT_EQUAL_UINT32(0, payload);
}

void test_sensor_tof_sentinel_invalid() {
  TEST_ASSERT_TRUE(sensortask::tofReadingValid(true, 50));
  TEST_ASSERT_FALSE(sensortask::tofReadingValid(true, sensortask::kNoReadingMm));
  TEST_ASSERT_FALSE(sensortask::tofReadingValid(false, 50));
}

void test_sensor_due_wrap_safe() {
  uint32_t last = 1000;
  TEST_ASSERT_FALSE(sensortask::due(1010, last, 20));
  TEST_ASSERT_EQUAL_UINT32(1000, last);
  TEST_ASSERT_TRUE(sensortask::due(1020, last, 20));
  TEST_ASSERT_EQUAL_UINT32(1020, last);
  // Rollover: now wrapped past zero, elapsed still measured unsigned.
  last = 0xFFFFFFF0u;
  TEST_ASSERT_FALSE(sensortask::due(0xFFFFFFF9u, last, 20));  // 9ms elapsed
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFF0u, last);
  TEST_ASSERT_TRUE(sensortask::due(0x00000005u, last, 20));  // 21ms elapsed
  TEST_ASSERT_EQUAL_UINT32(0x00000005u, last);
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the sensortask tests are declared extern there.
