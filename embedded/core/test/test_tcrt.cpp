// Host unit tests for the TCRT5000 hysteresis rail (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace tcrt (tcrt::railUpdate in
// src/hal/tcrt5000_driver.h): symmetric firmware threshold + hysteresis
// around CFG defaults (threshold 1500, hysteresis 200 — ground asserts at
// >= 1700, clears at <= 1300). The Tcrt5000Driver firmware class
// (analogRead/attenuation/WDT) is exercised on hardware. Runner lives in
// test_udp_codec.cpp.

#include <unity.h>

#include "../src/hal/tcrt5000_driver.h"

namespace {

constexpr uint16_t kThr = 1500;
constexpr uint16_t kHyst = 200;

}  // namespace

void test_tcrt_rail_asserts_above_threshold_plus_hyst() {
  // Cleared rail asserts at >= thr+hyst (1700), stays clear just below it.
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 1699, kThr, kHyst));
  TEST_ASSERT_TRUE(tcrt::railUpdate(false, 1700, kThr, kHyst));
  TEST_ASSERT_TRUE(tcrt::railUpdate(false, 4095, kThr, kHyst));
}

void test_tcrt_rail_clears_below_threshold_minus_hyst() {
  // Asserted rail clears at <= thr-hyst (1300), holds just above it.
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 1301, kThr, kHyst));
  TEST_ASSERT_FALSE(tcrt::railUpdate(true, 1300, kThr, kHyst));
  TEST_ASSERT_FALSE(tcrt::railUpdate(true, 0, kThr, kHyst));
}

void test_tcrt_rail_holds_inside_band() {
  // Inside (1300, 1700) the level holds either way — edge flicker never flips.
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 1500, kThr, kHyst));
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 1500, kThr, kHyst));
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 1699, kThr, kHyst));
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 1301, kThr, kHyst));
}

void test_tcrt_rail_full_sweep_latches() {
  // A void→floor→void sweep latches through the band (stateful via feedback).
  bool level = false;  // boot-into-void
  level = tcrt::railUpdate(level, 100, kThr, kHyst);
  TEST_ASSERT_FALSE(level);
  level = tcrt::railUpdate(level, 1500, kThr, kHyst);  // mid-band: still void
  TEST_ASSERT_FALSE(level);
  level = tcrt::railUpdate(level, 1800, kThr, kHyst);  // floor: asserts
  TEST_ASSERT_TRUE(level);
  level = tcrt::railUpdate(level, 1500, kThr, kHyst);  // mid-band: still floor
  TEST_ASSERT_TRUE(level);
  level = tcrt::railUpdate(level, 1200, kThr, kHyst);  // void: clears
  TEST_ASSERT_FALSE(level);
}

void test_tcrt_rail_custom_thresholds() {
  // Ad-hoc bench thresholds behave identically (constructor-with-CFG-defaults
  // pattern lets the bench pin these without touching config.h).
  TEST_ASSERT_TRUE(tcrt::railUpdate(false, 2200, 2000, 100));
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 2099, 2000, 100));
  TEST_ASSERT_FALSE(tcrt::railUpdate(true, 1900, 2000, 100));
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 1901, 2000, 100));
}

void test_tcrt_rail_cfg_defaults_match_header() {
  // The CFG fallbacks compiled into the header are the bench-measured values
  // (2026-10-05): threshold 600, hysteresis 200, assert-low polarity.
  TEST_ASSERT_EQUAL_UINT16(600, static_cast<uint16_t>(CFG_TCRT_THRESHOLD));
  TEST_ASSERT_EQUAL_UINT16(200, static_cast<uint16_t>(CFG_TCRT_HYSTERESIS));
  TEST_ASSERT_EQUAL(0, CFG_TCRT_ASSERT_HIGH);
  TEST_ASSERT_TRUE(tcrt::railUpdate(false, 400, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, false));
  TEST_ASSERT_FALSE(tcrt::railUpdate(true, 800, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, false));
}

void test_tcrt_rail_inverted_asserts_below_threshold_minus_hyst() {
  // Inverted rail (our modules — A0 sinks with reflection), thr 600 hyst
  // 200: cleared asserts at <= 400, stays clear just above it.
  TEST_ASSERT_TRUE(tcrt::railUpdate(false, 400, 600, 200, false));
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 401, 600, 200, false));
  TEST_ASSERT_FALSE(tcrt::railUpdate(false, 4095, 600, 200, false));
}

void test_tcrt_rail_inverted_clears_above_threshold_plus_hyst() {
  // Asserted rail clears at >= 800, holds just below it.
  TEST_ASSERT_FALSE(tcrt::railUpdate(true, 800, 600, 200, false));
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 799, 600, 200, false));
  TEST_ASSERT_TRUE(tcrt::railUpdate(true, 0, 600, 200, false));
}

void test_tcrt_rail_inverted_bench_bands() {
  // Measured bands (2026-10-05, thr 600 hyst 200): floor 167-220 asserts
  // and holds, void ~1100 clears and holds.
  bool level = true;  // boot-with-ground
  level = tcrt::railUpdate(level, 220, 600, 200, false);
  TEST_ASSERT_TRUE(level);
  level = tcrt::railUpdate(level, 1100, 600, 200, false);  // void: clears
  TEST_ASSERT_FALSE(level);
  level = tcrt::railUpdate(level, 600, 600, 200, false);  // mid-band: still void
  TEST_ASSERT_FALSE(level);
  level = tcrt::railUpdate(level, 167, 600, 200, false);  // floor: asserts
  TEST_ASSERT_TRUE(level);
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the tcrt tests are declared extern there.
