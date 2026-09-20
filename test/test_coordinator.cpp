// Host unit tests for the coordinator arbitrator (no hardware).
// Run: pio test -e native-test
//
// Covers coordinator::arbitrate() only — the pure P1>P2>P3>P4 decision
// function. The Coordinator task (event drain, L1 calls, WDT) is firmware-only
// (#ifdef ARDUINO) and exercised on hardware via synthetic events.

#include <unity.h>

#include "../src/behavior/coordinator.h"

namespace {

coordinator::ArbitrateInput manualDrive(float v, float omega) {
  coordinator::ArbitrateInput in;
  in.mode = SystemState::MODE_MANUAL;
  in.cmdV = v;
  in.cmdOmega = omega;
  in.hasCommand = true;
  in.freshCommand = true;
  return in;
}

coordinator::ArbitrateInput latchedDrive(float v, float omega) {
  coordinator::ArbitrateInput in = manualDrive(v, omega);
  in.mode = SystemState::MODE_EMERGENCY;
  in.cliffLatched = true;
  in.cliffActive = true;  // hazard still physically present unless a test clears it
  return in;
}

}  // namespace

void test_coord_stick_bytes_map_center_128() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, coordinator::byteToUnit(128));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, coordinator::byteToUnit(255));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, coordinator::byteToUnit(0));
  // Big-endian pack [mode|throttle|steering|flags]; centered stick -> zero cmd.
  const uint32_t payload = coordinator::packCommand(0x01, 128, 128, 0x00);
  TEST_ASSERT_EQUAL_UINT32(0x01808000UL, payload);
  uint8_t mode = 0;
  uint8_t flags = 0;
  float v = 9.0f;
  float omega = 9.0f;
  coordinator::unpackCommand(payload, mode, v, omega, flags);
  TEST_ASSERT_EQUAL_UINT8(0x01, mode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, omega);
  TEST_ASSERT_EQUAL_UINT8(0x00, flags);
}

void test_coord_cliff_entry_brakes_and_latches() {
  coordinator::ArbitrateInput in = manualDrive(0.6f, 0.0f);
  in.cliffEvent = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_TRUE(out.doBrake);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_forward_veto_while_latched() {
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(latchedDrive(0.6f, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_FALSE(out.doBrake);  // entry already braked; latch holds via stop
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_reverse_allowed_while_latched() {
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(latchedDrive(-0.6f, 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.6f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
}

void test_coord_turn_allowed_while_latched() {
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(latchedDrive(0.0f, 0.5f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.omega);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
}

void test_coord_latched_ignores_fresh_cmd_while_cliffed() {
  // Nonzero command arriving while still cliffed must NOT unlatch.
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.0f);
  in.freshCommand = true;
  in.cliffActive = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_stays_stopped_after_clear_without_new_cmd() {
  // Cleared cliff with no new command stays stopped in EMERGENCY.
  coordinator::ArbitrateInput in = latchedDrive(0.0f, 0.0f);
  in.cliffActive = false;
  in.freshCommand = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
}

void test_coord_stale_held_stick_does_not_resume() {
  // Clear happened (level), but the nonzero stick is stale-held, not fresh:
  // wheels must not restart on an old-held command.
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.0f);
  in.cliffActive = false;
  in.freshCommand = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
}

void test_coord_centered_fresh_cmd_does_not_resume() {
  // Clear + freshly-centered stick (no drive intent) stays stopped.
  coordinator::ArbitrateInput in = latchedDrive(0.0f, 0.0f);
  in.cliffActive = false;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
}

void test_coord_resumes_on_clear_plus_fresh_cmd() {
  // Clear latched earlier as a level (different tick), fresh drive now.
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.1f);
  in.cliffActive = false;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_MANUAL, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_DRIVE_FORWARD, out.nextBehavior);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.v);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, out.omega);
  TEST_ASSERT_FALSE(out.doBrake);
}

void test_coord_done_advances_behavior() {
  coordinator::ArbitrateInput in = manualDrive(0.0f, 0.0f);
  in.activeBehavior = BEHAVIOR_HAPPY_WIGGLE;
  in.doneReceived = true;
  in.doneId = BEHAVIOR_HAPPY_WIGGLE;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
  TEST_ASSERT_FALSE(out.holdMotion);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
}

void test_coord_running_maneuver_survives_centered_stick() {
  // A timed maneuver owns the L1 mailbox until DONE: centered stick must not
  // cancel it (holdMotion), while the failsafe and new intents still preempt.
  coordinator::ArbitrateInput in = manualDrive(0.0f, 0.0f);
  in.activeBehavior = BEHAVIOR_HAPPY_WIGGLE;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_HAPPY_WIGGLE, out.nextBehavior);
  TEST_ASSERT_TRUE(out.holdMotion);
}

void test_coord_stale_udp_failsafe_stop() {
  coordinator::ArbitrateInput in = manualDrive(0.5f, 0.0f);
  in.activeBehavior = BEHAVIOR_DRIVE_FORWARD;
  in.udpStale = true;
  in.hasCommand = false;
  in.freshCommand = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
  TEST_ASSERT_FALSE(out.holdMotion);
}

void test_coord_drive_passes_throttle_raw() {
  coordinator::ArbitrateInput in = manualDrive(0.5f, -0.25f);
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.v);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.25f, out.omega);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_DRIVE_FORWARD, out.nextBehavior);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_MANUAL, out.nextMode);
  TEST_ASSERT_FALSE(out.doBrake);
}

void test_coord_centered_stick_releases_drive() {
  coordinator::ArbitrateInput in = manualDrive(0.0f, 0.0f);
  in.activeBehavior = BEHAVIOR_DRIVE_FORWARD;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_FALSE(out.holdMotion);
}

void test_coord_mismatched_done_does_not_clear() {
  coordinator::ArbitrateInput in = manualDrive(0.0f, 0.0f);
  in.activeBehavior = BEHAVIOR_HAPPY_WIGGLE;
  in.doneReceived = true;
  in.doneId = BEHAVIOR_DRIVE_FORWARD;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_HAPPY_WIGGLE, out.nextBehavior);
  TEST_ASSERT_TRUE(out.holdMotion);
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the coordinator tests are declared extern there.
