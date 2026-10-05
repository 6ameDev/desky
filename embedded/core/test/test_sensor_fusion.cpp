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

// Level pose (exact 0° tilt) with both TCRT rails live and grounded and the
// forward ToF holding an obstacle reading: the ground/obstacle split means
// distanceMM tracks the ToF while the ground bits track the rails.
fusion::SensorSnapshot levelRailsSnapshot() {
  fusion::SensorSnapshot s;
  s.ax = 0.0f;
  s.ay = 0.0f;
  s.az = -1.0f;
  s.tofMm = 200;
  s.tofValid = true;
  s.mpuHealthy = true;
  s.tcrtFwdGround = true;
  s.tcrtFwdValid = true;
  s.tcrtRevGround = true;
  s.tcrtRevValid = true;
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
  // No TCRT rail live in restSnapshot: ground bits hold their defaults.
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_TRUE(st.gndRev);
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

void test_tcrt_fwd_void_level_drops_fwd_and_fires_cliff() {
  // Forward TCRT rail reports void while level: gndFwd drops and
  // cliffDetected (the raw forward-TCRT flag) fires. The ToF obstacle
  // reading (200mm) only tracks distanceMM — it plays no part in the verdict.
  fusion::SensorSnapshot s = levelRailsSnapshot();
  s.tcrtFwdGround = false;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndRev);
  TEST_ASSERT_EQUAL_UINT16(200, st.distanceMM);
}

void test_tcrt_fwd_ground_level_holds_clear() {
  SystemState st;
  kIdentity.evaluate(levelRailsSnapshot(), st);  // both rails grounded, level
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_TRUE(st.gndRev);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(200, st.distanceMM);
}

void test_tcrt_tilted_void_abstains() {
  // Far past the 35° tilt gate the fusion abstains: a void rail must NOT
  // clear the held ground bits (45° nose-up here).
  fusion::SensorSnapshot s;
  s.ax = kSin45;
  s.ay = 0.0f;
  s.az = -kCos45;
  s.tofMm = 200;
  s.tofValid = true;
  s.mpuHealthy = true;
  s.tcrtFwdGround = false;
  s.tcrtFwdValid = true;
  s.tcrtRevGround = false;
  s.tcrtRevValid = true;
  SystemState st;  // boots with ground present
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 45.0f, st.pitch);
  TEST_ASSERT_TRUE(st.gndFwd);  // held, never cleared by a tilted verdict
  TEST_ASSERT_TRUE(st.gndRev);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_EQUAL_UINT16(200, st.distanceMM);  // obstacle path still live
}

void test_tcrt_30deg_roll_fires() {
  // Tilt gate is total-tilt-magnitude < 35°: 30° roll is level enough that a
  // void forward rail fires, while the rear rail stays grounded.
  fusion::SensorSnapshot s;
  s.ay = 0.5f;  // sin30
  s.az = -0.8660254f;
  s.tofMm = 200;
  s.tofValid = true;
  s.mpuHealthy = true;
  s.tcrtFwdGround = false;
  s.tcrtFwdValid = true;
  s.tcrtRevGround = true;
  s.tcrtRevValid = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndRev);
}

void test_tcrt_level_2deg_fires() {
  // Bench verdict pin: ~2° total tilt is level enough to fire on a void rail.
  fusion::SensorSnapshot s;
  s.ax = 0.0348995f;  // sin2
  s.ay = 0.0f;
  s.az = -0.9993908f;  // -cos2
  s.tofMm = 200;
  s.tofValid = true;
  s.mpuHealthy = true;
  s.tcrtFwdGround = false;
  s.tcrtFwdValid = true;
  s.tcrtRevGround = true;
  s.tcrtRevValid = true;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_TRUE(st.gndRev);
}

void test_unhealthy_mpu_freezes_tilt_and_flags() {
  SystemState st;
  st.pitch = 10.0f;
  st.roll = -7.0f;
  st.distanceMM = 60;
  st.cliffDetected = true;
  st.isPickedUp = true;
  st.gndFwd = false;
  st.gndRev = false;

  fusion::SensorSnapshot s;
  s.ax = kSin45;
  s.az = -kCos45;
  s.tofMm = 150;
  s.tofValid = true;
  s.mpuHealthy = false;
  s.tcrtFwdGround = true;  // live rails, but the MPU gate is dead ...
  s.tcrtFwdValid = true;
  s.tcrtRevGround = true;
  s.tcrtRevValid = true;
  kIdentity.evaluate(s, st);

  TEST_ASSERT_EQUAL_FLOAT(10.0f, st.pitch);
  TEST_ASSERT_EQUAL_FLOAT(-7.0f, st.roll);
  TEST_ASSERT_TRUE(st.cliffDetected);  // dead sensor never clears flags
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_FALSE(st.gndRev);
  TEST_ASSERT_TRUE(st.isPickedUp);               // fusion never touches pickup
  TEST_ASSERT_EQUAL_UINT16(150, st.distanceMM);  // healthy ToF path stays live
}

void test_invalid_tof_retains_distance_rails_still_vote() {
  SystemState st;
  st.distanceMM = 80;
  st.cliffDetected = false;

  // Stale ToF garbage: distanceMM holds, but live TCRT rails + healthy MPU
  // still vote — a void forward rail fires the cliff while level.
  fusion::SensorSnapshot s = levelRailsSnapshot();
  s.tofMm = 999;  // stale garbage: must be ignored
  s.tofValid = false;
  s.tcrtFwdGround = false;
  kIdentity.evaluate(s, st);

  TEST_ASSERT_EQUAL_UINT16(80, st.distanceMM);
  TEST_ASSERT_TRUE(st.cliffDetected);
  TEST_ASSERT_FALSE(st.gndFwd);
  TEST_ASSERT_TRUE(st.gndRev);
  // Healthy MPU path stays live even while ToF is invalid.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, st.pitch);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, st.roll);
}

void test_tcrt_tof_tracks_obstacle_only() {
  // distanceMM follows the forward ToF through near and far readings while
  // the ground bits stay pinned by grounded rails: proximity never votes.
  fusion::SensorSnapshot s = levelRailsSnapshot();
  SystemState st;
  s.tofMm = 60;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_EQUAL_UINT16(60, st.distanceMM);
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_FALSE(st.cliffDetected);
  s.tofMm = 3000;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_EQUAL_UINT16(3000, st.distanceMM);
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_FALSE(st.cliffDetected);
}

void test_rev_void_level_drops_rev_only() {
  // Rear TCRT rail void while level: gndRev drops, fwd stays grounded and
  // cliffDetected (raw fwd flag) stays false.
  fusion::SensorSnapshot s = levelRailsSnapshot();
  s.tcrtRevGround = false;
  SystemState st;
  kIdentity.evaluate(s, st);
  TEST_ASSERT_TRUE(st.gndFwd);
  TEST_ASSERT_FALSE(st.cliffDetected);
  TEST_ASSERT_FALSE(st.gndRev);
}

void test_rev_invalid_holds_default_true() {
  // Disabled rear rail holds gndRev at default-true even when the fwd rail
  // drops into a cliff.
  fusion::SensorSnapshot s = levelRailsSnapshot();
  s.tcrtFwdGround = false;
  s.tcrtRevValid = false;
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
  fusion::SensorSnapshot dead = levelRailsSnapshot();
  dead.tofValid = false;
  dead.tcrtFwdValid = false;
  dead.tcrtRevValid = false;
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
  fusion::SensorSnapshot half = levelRailsSnapshot();
  half.tcrtFwdValid = false;  // fwd dead, rev still live-but-grounded
  half.mpuHealthy = true;     // tilt still live
  kIdentity.evaluate(half, st2);
  TEST_ASSERT_TRUE(st2.gndFwd);
  TEST_ASSERT_TRUE(st2.gndRev);
  TEST_ASSERT_FALSE(st2.cliffDetected);
}

// Single-program runner lives in test_udp_codec.cpp (pio test links all
// test/*.cpp into one binary): the fusion tests are declared extern there.
