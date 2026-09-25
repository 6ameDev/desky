// Host unit tests for the coordinator arbitrator (no hardware).
// Run: pio test -e native-test
//
// Covers coordinator::arbitrate() only — the pure P1>P2>P3>P4 decision
// function. The Coordinator task (event drain, L1 calls, WDT) is firmware-only
// (#ifdef ARDUINO) and exercised on hardware via synthetic events.
//
// Vocabulary is per-direction ground: gndFwd/gndRev levels (1 = ground),
// groundEvent edge (fresh 1→0 on either rail), emergencyLatched. The 16 legacy
// scenarios replay with rev pinned true (gndRev=true) with identical verdicts
// to the old single-rail cliff suite; the rev/bit suites below cover the new
// direction (entry on rev edge, reverse veto, escape-forward, resume variants).

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
  in.gndFwd = true;
  in.gndRev = true;
  return in;
}

coordinator::ArbitrateInput latchedDrive(float v, float omega) {
  coordinator::ArbitrateInput in = manualDrive(v, omega);
  in.mode = SystemState::MODE_EMERGENCY;
  in.emergencyLatched = true;
  in.gndFwd = false;  // fwd void, rev pinned true: replays every old behavior
  in.gndRev = true;
  in.freshCommand = false;  // held stick by default: escape motion passes, latch holds
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

void test_coord_ground_bits_pack_unpack() {
  TEST_ASSERT_EQUAL_UINT32(0x03u, packGround(true, true));
  TEST_ASSERT_EQUAL_UINT32(0x02u, packGround(false, true));
  TEST_ASSERT_EQUAL_UINT32(0x01u, packGround(true, false));
  TEST_ASSERT_EQUAL_UINT32(0x00u, packGround(false, false));
  bool fwd = false;
  bool rev = false;
  unpackGround(0x02u, fwd, rev);
  TEST_ASSERT_FALSE(fwd);
  TEST_ASSERT_TRUE(rev);
  unpackGround(0x01u, fwd, rev);
  TEST_ASSERT_TRUE(fwd);
  TEST_ASSERT_FALSE(rev);
}

void test_coord_cliff_entry_brakes_and_latches() {
  coordinator::ArbitrateInput in = manualDrive(0.6f, 0.0f);
  in.groundEvent = true;
  in.gndFwd = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_TRUE(out.doBrake);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_entry_on_rev_edge() {
  coordinator::ArbitrateInput in = manualDrive(0.0f, 0.0f);
  in.groundEvent = true;
  in.gndFwd = true;
  in.gndRev = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_TRUE(out.doBrake);
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

void test_coord_reverse_veto_while_rev_latched() {
  coordinator::ArbitrateInput in = manualDrive(-0.6f, 0.0f);
  in.mode = SystemState::MODE_EMERGENCY;
  in.emergencyLatched = true;
  in.gndFwd = true;
  in.gndRev = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
}

void test_coord_escape_forward_while_rev_latched() {
  // Held (not fresh) forward while rev is void: motion passes, latch holds.
  coordinator::ArbitrateInput in = manualDrive(0.6f, 0.0f);
  in.mode = SystemState::MODE_EMERGENCY;
  in.emergencyLatched = true;
  in.gndFwd = true;
  in.gndRev = false;
  in.freshCommand = false;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, out.v);
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
  in.gndFwd = false;
  in.gndRev = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_blocked_rev_never_exits() {
  // Fresh reverse while rev is void: blocked direction never exits.
  coordinator::ArbitrateInput in = manualDrive(-0.5f, 0.0f);
  in.mode = SystemState::MODE_EMERGENCY;
  in.emergencyLatched = true;
  in.gndFwd = true;
  in.gndRev = false;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_NONE, out.nextBehavior);
}

void test_coord_stays_stopped_after_clear_without_new_cmd() {
  // Cleared ground with no new command stays stopped in EMERGENCY.
  coordinator::ArbitrateInput in = latchedDrive(0.0f, 0.0f);
  in.gndFwd = true;
  in.gndRev = true;
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
  in.gndFwd = true;
  in.gndRev = true;
  in.freshCommand = false;
  in.udpStale = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
}

void test_coord_stale_fresh_never_exits() {
  // Even a fresh nonzero in a permitted direction never exits while stale.
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.0f);
  in.gndFwd = true;
  in.gndRev = true;
  in.freshCommand = true;
  in.udpStale = true;
  in.hasCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
}

void test_coord_centered_fresh_cmd_does_not_resume() {
  // Clear + freshly-centered stick (no drive intent) stays stopped.
  coordinator::ArbitrateInput in = latchedDrive(0.0f, 0.0f);
  in.gndFwd = true;
  in.gndRev = true;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.omega);
}

void test_coord_resumes_on_clear_plus_fresh_cmd() {
  // Clear latched earlier as a level (different tick), fresh drive now.
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.1f);
  in.gndFwd = true;
  in.gndRev = true;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_MANUAL, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_DRIVE_FORWARD, out.nextBehavior);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.v);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, out.omega);
  TEST_ASSERT_FALSE(out.doBrake);
}

void test_coord_resume_permitted_direction_while_other_void() {
  // Fresh forward while fwd is present (rev still void): the commanded
  // direction is permitted, so it exits to MANUAL.
  coordinator::ArbitrateInput in = manualDrive(0.5f, 0.0f);
  in.mode = SystemState::MODE_EMERGENCY;
  in.emergencyLatched = true;
  in.gndFwd = true;
  in.gndRev = false;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_MANUAL, out.nextMode);
  TEST_ASSERT_EQUAL_UINT32(BEHAVIOR_DRIVE_FORWARD, out.nextBehavior);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.v);
}

void test_coord_turn_resume_needs_either_rail() {
  // Fresh pure turn with one rail present exits ...
  coordinator::ArbitrateInput in = latchedDrive(0.0f, 0.5f);
  in.gndFwd = false;
  in.gndRev = true;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_MANUAL, out.nextMode);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, out.omega);
  // ... but both-false blocks every exit (turn motion still passes silently).
  coordinator::ArbitrateInput both = latchedDrive(0.0f, 0.5f);
  both.gndFwd = false;
  both.gndRev = false;
  both.freshCommand = true;
  const coordinator::ArbitrateOutput outBoth = coordinator::arbitrate(both);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, outBoth.nextMode);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, outBoth.omega);
}

void test_coord_both_false_blocks_everything() {
  coordinator::ArbitrateInput in = latchedDrive(0.5f, 0.0f);
  in.gndFwd = false;
  in.gndRev = false;
  in.freshCommand = true;
  const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, out.nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.v);
  coordinator::ArbitrateInput rev = latchedDrive(-0.5f, 0.0f);
  rev.gndFwd = false;
  rev.gndRev = false;
  rev.freshCommand = true;
  TEST_ASSERT_EQUAL_INT(SystemState::MODE_EMERGENCY, coordinator::arbitrate(rev).nextMode);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, coordinator::arbitrate(rev).v);
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
