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

void test_cliff_far_and_unloaded() {
  fusion::SensorSnapshot s;
  s.ax = 0.7f;
  s.ay = 0.0f;
  s.az = 0.1f;  // |az| < 0.5g gate: not level, not resting on ground
  s.tofMm = 150;
  s.tofValid = true;
  s.mpuHealthy = true;
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

  // Boundary: exactly CFG_CLIFF_MM (100) is NOT a cliff (strict >).
  fusion::SensorSnapshot s;
  s.az = 0.1f;
  s.tofMm = 100;
  s.tofValid = true;
  s.mpuHealthy = true;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.cliffDetected);
}

void test_cliff_gated_when_level() {
  // Far ToF but level (|az| ~ 1g at rest): gate blocks the cliff flag.
  fusion::SensorSnapshot s = restSnapshot();
  s.tofMm = 150;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(150, st.distanceMM);  // ToF path still live
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

// Single-program runner lives in test_udp_codec.cpp (pio test links all
// test/*.cpp into one binary): the fusion tests are declared extern there.
