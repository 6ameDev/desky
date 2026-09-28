// Host unit tests for the sensor fusion engine (no hardware).
// Run: pio test -e native-test
//
// Orientation is an explicit FusionMounting input, never ambient config:
// the same vectors run under identity mounting (stable forever) and the
// swapped variant proves the remap independently. Changing the production
// mount (kBoardMounting) never rewrites these tests — it adds one
// (test_board_mounting_matches_bench).
//
// Reference frame: measured level rest is ax=-0.026g, ay=+0.039g, az=-1.044g.
// Post-remap convention: +pitch = nose-up, +roll = right-side-down.

#include <math.h>
#include <unity.h>

#include "../src/middleware/sensor_fusion.h"

namespace {

constexpr float kSin45 = 0.70710678f;
constexpr float kCos45 = 0.70710678f;

const fusion::Fusion kIdentity{{false}};
const fusion::Fusion kSwapped{{true}};

fusion::SensorSnapshot restSnapshot() {
  fusion::SensorSnapshot s;
  s.ax = -0.026f;
  s.ay = 0.039f;
  s.az = -1.044f;
  s.tofMm = 50;
  s.tofValid = true;
  s.mpuHealthy = true;
  return s;
}

}  // namespace

void test_flat_rest_no_cliff_near_zero_tilt() {
  SystemState st;
  kIdentity.evaluate(restSnapshot(), st);
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 0.0f, st.pitch);
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 0.0f, st.roll);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(50, st.distanceMM);
  TEST_ASSERT_FALSE(st.isPickedUp);
}

void test_nose_up_45_pitch_tracks_identity() {
  fusion::SensorSnapshot s;
  s.ax = kSin45;
  s.ay = 0.0f;
  s.az = -kCos45;
  s.tofMm = 50;
  s.tofValid = true;
  s.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 45.0f, st.pitch);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 0.0f, st.roll);
}

void test_nose_up_45_pitch_tracks_swapped() {
  // Same physical pose as above, sensor-frame rotated: tilt on +Y.
  fusion::SensorSnapshot s;
  s.ax = 0.0f;
  s.ay = kSin45;
  s.az = -kCos45;
  s.tofMm = 50;
  s.tofValid = true;
  s.mpuHealthy = true;
  SystemState st;
  kSwapped.evaluate(s, st);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 45.0f, st.pitch);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 0.0f, st.roll);
}

void test_sign_convention_identity() {
  // Nose-up 90 deg (+X at +1g) reads +90 pitch; nose-down reads -90.
  fusion::SensorSnapshot up;
  up.ax = 1.0f;
  up.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(up, st);
  TEST_ASSERT_GREATER_THAN_FLOAT(85.0f, st.pitch);

  fusion::SensorSnapshot down;
  down.ax = -1.0f;
  down.mpuHealthy = true;
  kIdentity.evaluate(down, st);
  TEST_ASSERT_LESS_THAN_FLOAT(-85.0f, st.pitch);

  // Right-side-down 90 deg (+Y at +1g) reads +90 roll.
  fusion::SensorSnapshot right;
  right.ay = 1.0f;
  right.mpuHealthy = true;
  kIdentity.evaluate(right, st);
  TEST_ASSERT_GREATER_THAN_FLOAT(85.0f, st.roll);
}

void test_sign_convention_swapped() {
  // Bench mapping: nose-up puts +g on sensor +Y, right-side-down on +X.
  fusion::SensorSnapshot up;
  up.ay = 1.0f;
  up.mpuHealthy = true;
  SystemState st;
  kSwapped.evaluate(up, st);
  TEST_ASSERT_GREATER_THAN_FLOAT(85.0f, st.pitch);

  fusion::SensorSnapshot down;
  down.ay = -1.0f;
  down.mpuHealthy = true;
  kSwapped.evaluate(down, st);
  TEST_ASSERT_LESS_THAN_FLOAT(-85.0f, st.pitch);

  fusion::SensorSnapshot right;
  right.ax = 1.0f;
  right.mpuHealthy = true;
  kSwapped.evaluate(right, st);
  TEST_ASSERT_GREATER_THAN_FLOAT(85.0f, st.roll);
}

void test_board_mounting_matches_bench() {
  // Production mounting (swap=1 per bench 2026-09-20): sensor-+Y tilt must
  // read as nose-up pitch. If the mount changes, THIS test gets replaced.
  fusion::SensorSnapshot s;
  s.ax = 0.0f;
  s.ay = kSin45;
  s.az = -kCos45;
  s.mpuHealthy = true;
  SystemState st;
  fusion::Fusion(fusion::kBoardMounting).evaluate(s, st);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 45.0f, st.pitch);
}

void test_cliff_far_and_level_fires() {
  // ToF looks 30deg down: the catch moment is far + level (|az| > 0.8 gate).
  fusion::SensorSnapshot s = restSnapshot();  // level, az=-1.044g
  s.tofMm = 150;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(150, st.distanceMM);
}

void test_cliff_near_ground_false() {
  SystemState st;
  kIdentity.evaluate(restSnapshot(), st);  // tof 50mm, level
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(50, st.distanceMM);

  // Boundary: exactly CFG_CLIFF_MM (100) is NOT a cliff (strict >), even level.
  // Pinned at exact level (pitch 0 → thr exactly 100); the rest pose carries
  // ~-1.4° pitch so its compensated threshold sits just under 100.
  fusion::SensorSnapshot s;
  s.ax = 0.0f;
  s.ay = 0.0f;
  s.az = -1.0f;
  s.tofMm = 100;
  s.tofValid = true;
  s.mpuHealthy = true;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndFwd);
}

void test_cliff_tilted_far_holds() {
  // Far ToF but tipped (|az| ~ 0.1g): not level, so no cliff.
  fusion::SensorSnapshot s;
  s.ax = 0.7f;
  s.ay = 0.0f;
  s.az = 0.1f;
  s.tofMm = 150;
  s.tofValid = true;
  s.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(150, st.distanceMM);  // ToF path still live
}

void test_cliff_30deg_fires_45deg_holds() {
  // Tilt gate is total-tilt-magnitude < 35°: 30° single-axis (roll, so the
  // pitch-compensated fwd threshold stays ~100) fires, 45° holds.
  fusion::SensorSnapshot rolled;
  rolled.ay = 0.5f;  // sin30
  rolled.az = -0.8660254f;
  rolled.tofMm = 150;
  rolled.tofValid = true;
  rolled.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(rolled, st);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_FALSE(st.gndFwd);

  fusion::SensorSnapshot tipped45;
  tipped45.ax = kSin45;
  tipped45.az = -kCos45;
  tipped45.tofMm = 150;
  tipped45.tofValid = true;
  tipped45.mpuHealthy = true;
  kIdentity.evaluate(tipped45, st);
  TEST_ASSERT_FALSE(st.cliffDetected);
}

void test_unhealthy_mpu_freezes_tilt_and_flags() {
  SystemState st;
  st.pitch = 10.0f;
  st.roll = -7.0f;
  st.distanceMM = 60;
  st.cliffDetected = true;
  st.isPickedUp = true;

  fusion::SensorSnapshot s;
  s.ax = kSin45;
  s.az = -kCos45;
  s.tofMm = 150;
  s.tofValid = true;
  s.mpuHealthy = false;
  kIdentity.evaluate(s, st);

  TEST_ASSERT_EQUAL_FLOAT(10.0f, st.pitch);
  TEST_ASSERT_EQUAL_FLOAT(-7.0f, st.roll);
  TEST_ASSERT_TRUE(st.cliffDetected);            // dead sensor never clears flags
  TEST_ASSERT_TRUE(st.isPickedUp);               // fusion never touches pickup
  TEST_ASSERT_EQUAL_UINT16(150, st.distanceMM);  // healthy ToF path stays live
}

void test_invalid_tof_retains_distance_no_cliff_from_stale() {
  SystemState st;
  st.distanceMM = 80;
  st.cliffDetected = false;

  fusion::SensorSnapshot s = restSnapshot();
  s.tofMm = 999;  // stale garbage: must be ignored
  s.tofValid = false;
  kIdentity.evaluate(s, st);

  TEST_ASSERT_EQUAL_UINT16(80, st.distanceMM);
  TEST_ASSERT_FALSE(st.cliffDetected);
  // Healthy MPU path stays live even while ToF is invalid.
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 0.0f, st.pitch);
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 0.0f, st.roll);
}

void test_threshold_level_is_base() {
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, fusion::cliffThresholdMm(100.0f, 0.0f, true));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, fusion::cliffThresholdMm(100.0f, 0.0f, false));
}

void test_threshold_nose_up_15_fwd_193() {
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 193.0f, fusion::cliffThresholdMm(100.0f, 15.0f, true));
}

void test_threshold_nose_down_15_fwd_71() {
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 71.0f, fusion::cliffThresholdMm(100.0f, -15.0f, true));
}

void test_threshold_clamp_bounds() {
  // Nose-up +40° fwd: dep = 30−40 = −10 → clamped to 10° → ~288mm.
  TEST_ASSERT_FLOAT_WITHIN(5.0f, 288.0f, fusion::cliffThresholdMm(100.0f, 40.0f, true));
  // Nose-down −60° fwd: dep = 30+60 = 90 → clamped to 80° → ~51mm.
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 51.0f, fusion::cliffThresholdMm(100.0f, -60.0f, true));
}

void test_threshold_rear_mirrors_fwd() {
  // Rear beam mirrors: fwd(+15) == rev(−15), fwd(−15) == rev(+15).
  TEST_ASSERT_FLOAT_WITHIN(0.01f, fusion::cliffThresholdMm(100.0f, 15.0f, true),
                           fusion::cliffThresholdMm(100.0f, -15.0f, false));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, fusion::cliffThresholdMm(100.0f, -15.0f, true),
                           fusion::cliffThresholdMm(100.0f, 15.0f, false));
}

void test_bump_climb_120_passes_at_plus15() {
  // Bump scenario: nose-up +15° while climbing, beam reads 120mm. The old
  // fixed 100mm rule would fire; the compensated ~193mm threshold passes.
  fusion::SensorSnapshot s;
  s.ax = 0.2588190f;  // sin15
  s.ay = 0.0f;
  s.az = -0.9659258f;  // -cos15
  s.tofMm = 120;
  s.tofValid = true;
  s.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 15.0f, st.pitch);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_EQUAL_UINT16(120, st.distanceMM);
}

void test_level_2deg_fires() {
  // Bench verdict pin: ~2° total tilt is level enough to fire on far+level.
  fusion::SensorSnapshot s;
  s.ax = 0.0348995f;  // sin2
  s.ay = 0.0f;
  s.az = -0.9993908f;  // -cos2
  s.tofMm = 150;
  s.tofValid = true;
  s.mpuHealthy = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_FALSE(st.gndFwd);
}

void test_rev_far_level_drops_rev_only() {
  // Mirrored rev rail: rev far+level drops gndRev while fwd stays grounded
  // and cliffDetected (raw fwd flag) stays false.
  fusion::SensorSnapshot s = restSnapshot();  // fwd 50mm near, level
  s.tofRevMm = 150;
  s.tofRevValid = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_FALSE(st.gndRev);
}

void test_rev_invalid_holds_default_true() {
  // No rear sensor yet: invalid rev holds gndRev at default-true even when
  // the fwd rail drops.
  fusion::SensorSnapshot s = restSnapshot();
  s.tofMm = 500;  // far, level → fwd drops
  s.tofRevValid = false;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndRev);
}

void test_dead_sources_hold_both_bits() {
  // Dead rails never clear: preset dropped bits survive a dead tick ...
  SystemState st;
  st.gndFwd = false;
  st.gndRev = false;
  st.cliffDetected = true;
  st.distanceMM = 60;
  fusion::SensorSnapshot dead = restSnapshot();
  dead.tofValid = false;
  dead.tofRevValid = false;
  dead.mpuHealthy = false;
  kIdentity.evaluate(dead, st);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_FALSE(st.gndRev);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(60, st.distanceMM);
  // ... and preset present bits survive a one-sided dead tick.
  SystemState st2;
  st2.gndFwd = true;
  st2.gndRev = true;
  st2.cliffDetected = false;
  fusion::SensorSnapshot half = restSnapshot();
  half.tofValid = false;  // fwd dead, rev invalid too
  half.tofRevValid = false;
  half.mpuHealthy = true;  // tilt still live
  kIdentity.evaluate(half, st2);
  TEST_ASSERT_TRUE(st2.gndFwd);
  TEST_ASSERT_TRUE(st2.gndRev);
  TEST_ASSERT_FALSE(st2.cliffDetected);
}

// Single-program runner lives in test_udp_codec.cpp (pio test links all
// test/*.cpp into one binary): the fusion tests are declared extern there.
