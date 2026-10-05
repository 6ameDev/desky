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

void test_ground_fwd_loss_publishes_packed_bits() {
  bool lastFwd = true;
  bool lastRev = true;  // boots with ground present
  uint32_t payload = 0;
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, true, true, lastFwd, lastRev, payload));
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, false, true, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(false, true), payload);
  TEST_ASSERT_FALSE(lastFwd);
  TEST_ASSERT_TRUE(lastRev);
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, false, true, lastFwd, lastRev, payload));
}

void test_ground_rev_loss_publishes_packed_bits() {
  bool lastFwd = true;
  bool lastRev = true;
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, true, false, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(true, false), payload);
  bool fwd = false;
  bool rev = false;
  unpackGround(payload, fwd, rev);
  TEST_ASSERT_TRUE(fwd);
  TEST_ASSERT_FALSE(rev);
}

void test_ground_either_rail_transition_only() {
  bool lastFwd = true;
  bool lastRev = true;
  uint32_t payload = 0xDEAD;
  // Held levels silent on both rails.
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, true, true, lastFwd, lastRev, payload));
  // Rev-only flip fires; fwd-only clear edge fires with both bits packed.
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, true, false, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(true, false), payload);
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, false, false, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(false, false), payload);
  // Held void silent.
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, false, false, lastFwd, lastRev, payload));
  // Clear edge fires.
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, true, true, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(true, true), payload);
}

void test_ground_dead_ticks_hold_silence() {
  bool lastFwd = false;  // latched void
  bool lastRev = true;
  uint32_t payload = 0xDEAD;
  TEST_ASSERT_FALSE(sensortask::groundTransition(false, true, true, lastFwd, lastRev, payload));
  TEST_ASSERT_FALSE(lastFwd);  // dead tick never moves the latch
  TEST_ASSERT_TRUE(lastRev);
  TEST_ASSERT_FALSE(sensortask::groundTransition(false, false, false, lastFwd, lastRev, payload));
  // Live tick on the held level stays silent; only a real flip fires.
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, false, true, lastFwd, lastRev, payload));
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, true, true, lastFwd, lastRev, payload));
  TEST_ASSERT_EQUAL_UINT32(packGround(true, true), payload);
}

void test_ground_boot_into_void_publishes_once() {
  bool lastFwd = true;
  bool lastRev = true;  // boot state is always ground present
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(sensortask::groundTransition(true, false, true, lastFwd, lastRev, payload));
  TEST_ASSERT_FALSE(sensortask::groundTransition(true, false, true, lastFwd, lastRev, payload));
}

void test_obstacle_assert_fires_once_below_band() {
  // Assert edge fires once below (OBSTACLE - HYST) = 140 with payload = mm.
  bool asserted = false;
  uint32_t payload = 0;
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 140, asserted, payload));  // boundary: strict <
  TEST_ASSERT_FALSE(asserted);
  TEST_ASSERT_TRUE(sensortask::obstacleTransition(true, 139, asserted, payload));
  TEST_ASSERT_EQUAL_UINT32(139, payload);
  TEST_ASSERT_TRUE(asserted);
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 100, asserted, payload));  // held: silent
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 50, asserted, payload));
}

void test_obstacle_rearm_needs_clear_above_band() {
  // The latch re-arms only above (OBSTACLE + HYST) = 160; the clear edge
  // never publishes and mid-band readings hold the latch either way.
  bool asserted = false;
  uint32_t payload = 0;
  TEST_ASSERT_TRUE(sensortask::obstacleTransition(true, 120, asserted, payload));
  TEST_ASSERT_EQUAL_UINT32(120, payload);
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 150, asserted, payload));  // mid-band: still armed
  TEST_ASSERT_TRUE(asserted);
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 160, asserted, payload));  // boundary: strict >
  TEST_ASSERT_TRUE(asserted);
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 161, asserted, payload));  // silent re-arm
  TEST_ASSERT_FALSE(asserted);
  TEST_ASSERT_TRUE(sensortask::obstacleTransition(true, 139, asserted, payload));  // fires again
  TEST_ASSERT_EQUAL_UINT32(139, payload);
}

void test_obstacle_far_readings_never_fire() {
  bool asserted = false;
  uint32_t payload = 0xDEAD;
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 150, asserted, payload));
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 200, asserted, payload));
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 4000, asserted, payload));
  TEST_ASSERT_FALSE(asserted);
}

void test_obstacle_dead_ticks_hold_silence() {
  bool asserted = false;
  uint32_t payload = 0xDEAD;
  // Dead ticks never evaluate and never move the latch, even at assert range.
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(false, 50, asserted, payload));
  TEST_ASSERT_FALSE(asserted);
  // Live tick on a clear latch at assert range fires normally afterwards.
  TEST_ASSERT_TRUE(sensortask::obstacleTransition(true, 50, asserted, payload));
  TEST_ASSERT_EQUAL_UINT32(50, payload);
  // A dead tick mid-latch cannot re-arm it: no clear edge is synthesized.
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(false, 3000, asserted, payload));
  TEST_ASSERT_TRUE(asserted);
  TEST_ASSERT_FALSE(sensortask::obstacleTransition(true, 3000, asserted, payload));
  TEST_ASSERT_FALSE(asserted);  // live far reading re-arms (silently)
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the sensortask tests are declared extern there.
