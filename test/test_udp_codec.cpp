// Host unit tests for the UDP packet codec (no hardware).
// Run: pio test -e native-test

#include <unity.h>

#include "../src/middleware/udp_codec.h"

void setUp() {}
void tearDown() {}

void test_control_golden_vector() {
  udp::ControlPacket pkt{0x01, 0x80, 0x40, 0x03};
  uint8_t out[udp::kControlSize] = {};
  TEST_ASSERT_TRUE(udp::encodeControl(pkt, out));
  // AA 01 80 40 03, XOR checksum 0x68.
  const uint8_t expected[udp::kControlSize] = {0xAA, 0x01, 0x80, 0x40, 0x03, 0x68};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, udp::kControlSize);
}

void test_control_decode_round_trip() {
  const uint8_t raw[udp::kControlSize] = {0xAA, 0x02, 0xFF, 0x00, 0x10, 0x00};
  uint8_t buf[udp::kControlSize];
  for (size_t i = 0; i < udp::kControlSize - 1; ++i) {
    buf[i] = raw[i];
  }
  buf[udp::kControlSize - 1] = udp::checksum(raw, udp::kControlSize - 1);
  udp::ControlPacket pkt;
  TEST_ASSERT_TRUE(udp::decodeControl(buf, sizeof(buf), pkt));
  TEST_ASSERT_EQUAL_UINT8(0x02, pkt.mode);
  TEST_ASSERT_EQUAL_UINT8(0xFF, pkt.throttle);
  TEST_ASSERT_EQUAL_UINT8(0x00, pkt.steering);
  TEST_ASSERT_EQUAL_UINT8(0x10, pkt.flags);
}

void test_control_rejects_bad_header_checksum_length() {
  udp::ControlPacket pkt;
  const uint8_t badHeader[udp::kControlSize] = {0xBB, 0x01, 0x80, 0x40, 0x03, 0x68};
  TEST_ASSERT_FALSE(udp::decodeControl(badHeader, sizeof(badHeader), pkt));
  const uint8_t badSum[udp::kControlSize] = {0xAA, 0x01, 0x80, 0x40, 0x03, 0x00};
  TEST_ASSERT_FALSE(udp::decodeControl(badSum, sizeof(badSum), pkt));
  const uint8_t shortBuf[4] = {0xAA, 0x01, 0x80, 0x40};
  TEST_ASSERT_FALSE(udp::decodeControl(shortBuf, sizeof(shortBuf), pkt));
  TEST_ASSERT_FALSE(udp::decodeControl(nullptr, udp::kControlSize, pkt));
}

void test_telemetry_golden_vector() {
  udp::TelemetryPacket pkt{100, -50, 300, 0x05};
  uint8_t out[udp::kTelemetrySize] = {};
  TEST_ASSERT_TRUE(udp::encodeTelemetry(pkt, out));
  // BB 0064 FFCE 012C 05, XOR checksum 0xC6.
  const uint8_t expected[udp::kTelemetrySize] = {0xBB, 0x00, 0x64, 0xFF, 0xCE, 0x01, 0x2C, 0x05, 0xC6};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, udp::kTelemetrySize);
}

void test_telemetry_decode_round_trip_negative() {
  udp::TelemetryPacket src{-123, 456, 65535, 0xFF};
  uint8_t buf[udp::kTelemetrySize];
  TEST_ASSERT_TRUE(udp::encodeTelemetry(src, buf));
  udp::TelemetryPacket dst;
  TEST_ASSERT_TRUE(udp::decodeTelemetry(buf, sizeof(buf), dst));
  TEST_ASSERT_EQUAL_INT16(-123, dst.pitch);
  TEST_ASSERT_EQUAL_INT16(456, dst.roll);
  TEST_ASSERT_EQUAL_UINT16(65535, dst.distanceMm);
  TEST_ASSERT_EQUAL_UINT8(0xFF, dst.statusFlags);
}

void test_telemetry_rejects_bad_header_checksum() {
  udp::TelemetryPacket pkt;
  uint8_t buf[udp::kTelemetrySize] = {0xAA, 0x00, 0x64, 0xFF, 0xCE, 0x01, 0x2C, 0x05, 0xC6};
  TEST_ASSERT_FALSE(udp::decodeTelemetry(buf, sizeof(buf), pkt));
  buf[0] = 0xBB;
  buf[udp::kTelemetrySize - 1] ^= 0xFF;
  TEST_ASSERT_FALSE(udp::decodeTelemetry(buf, sizeof(buf), pkt));
}

// Sensor fusion tests, defined in test_sensor_fusion.cpp (pio test links all
// test/*.cpp into one binary, so the single main lives here).
void test_flat_rest_no_cliff_near_zero_tilt();
void test_nose_up_45_pitch_tracks_identity();
void test_nose_up_45_pitch_tracks_swapped();
void test_sign_convention_identity();
void test_sign_convention_swapped();
void test_board_mounting_matches_bench();
void test_cliff_far_and_level_fires();
void test_cliff_near_ground_false();
void test_cliff_tilted_far_holds();
void test_cliff_30deg_fires_45deg_holds();
void test_unhealthy_mpu_freezes_tilt_and_flags();
void test_invalid_tof_retains_distance_no_cliff_from_stale();
void test_threshold_level_is_base();
void test_threshold_nose_up_15_fwd_193();
void test_threshold_nose_down_15_fwd_71();
void test_threshold_clamp_bounds();
void test_threshold_rear_mirrors_fwd();
void test_bump_climb_120_passes_at_plus15();
void test_level_2deg_fires();
void test_rev_far_level_drops_rev_only();
void test_rev_invalid_holds_default_true();
void test_dead_sources_hold_both_bits();

// Sensor task edge + hold logic, defined in test_sensor_task.cpp.
void test_sensor_rising_publishes_once();
void test_sensor_held_levels_silent();
void test_sensor_falling_publishes_once();
void test_sensor_reassert_after_clear();
void test_sensor_boot_into_cliff_publishes_once();
void test_sensor_frozen_inputs_silent();
void test_sensor_tof_sentinel_invalid();
void test_sensor_due_wrap_safe();
void test_ground_fwd_loss_publishes_packed_bits();
void test_ground_rev_loss_publishes_packed_bits();
void test_ground_either_rail_transition_only();
void test_ground_dead_ticks_hold_silence();
void test_ground_boot_into_void_publishes_once();

// Coordinator arbitrator tests, defined in test_coordinator.cpp.
void test_coord_stick_bytes_map_center_128();
void test_coord_ground_bits_pack_unpack();
void test_coord_cliff_entry_brakes_and_latches();
void test_coord_entry_on_rev_edge();
void test_coord_forward_veto_while_latched();
void test_coord_reverse_allowed_while_latched();
void test_coord_reverse_veto_while_rev_latched();
void test_coord_escape_forward_while_rev_latched();
void test_coord_turn_allowed_while_latched();
void test_coord_latched_ignores_fresh_cmd_while_cliffed();
void test_coord_blocked_rev_never_exits();
void test_coord_stays_stopped_after_clear_without_new_cmd();
void test_coord_stale_held_stick_does_not_resume();
void test_coord_stale_fresh_never_exits();
void test_coord_centered_fresh_cmd_does_not_resume();
void test_coord_resumes_on_clear_plus_fresh_cmd();
void test_coord_resume_permitted_direction_while_other_void();
void test_coord_turn_resume_needs_either_rail();
void test_coord_both_false_blocks_everything();
void test_coord_done_advances_behavior();
void test_coord_running_maneuver_survives_centered_stick();
void test_coord_stale_udp_failsafe_stop();
void test_coord_drive_passes_throttle_raw();
void test_coord_centered_stick_releases_drive();
void test_coord_mismatched_done_does_not_clear();

// UDP server helpers (statusFlags bitmap + edges), defined in test_udp_server.cpp.
void test_status_flags_pack_bits();
void test_status_flags_unpack_round_trip();
void test_pack_command_center_edges();
void test_telemetry_round_trip_with_flags_set();
void test_deg_to_decideg_edges_and_clamp();

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_control_golden_vector);
  RUN_TEST(test_control_decode_round_trip);
  RUN_TEST(test_control_rejects_bad_header_checksum_length);
  RUN_TEST(test_telemetry_golden_vector);
  RUN_TEST(test_telemetry_decode_round_trip_negative);
  RUN_TEST(test_telemetry_rejects_bad_header_checksum);
  RUN_TEST(test_flat_rest_no_cliff_near_zero_tilt);
  RUN_TEST(test_nose_up_45_pitch_tracks_identity);
  RUN_TEST(test_nose_up_45_pitch_tracks_swapped);
  RUN_TEST(test_sign_convention_identity);
  RUN_TEST(test_sign_convention_swapped);
  RUN_TEST(test_board_mounting_matches_bench);
  RUN_TEST(test_cliff_far_and_level_fires);
  RUN_TEST(test_cliff_near_ground_false);
  RUN_TEST(test_cliff_tilted_far_holds);
  RUN_TEST(test_cliff_30deg_fires_45deg_holds);
  RUN_TEST(test_unhealthy_mpu_freezes_tilt_and_flags);
  RUN_TEST(test_invalid_tof_retains_distance_no_cliff_from_stale);
  RUN_TEST(test_threshold_level_is_base);
  RUN_TEST(test_threshold_nose_up_15_fwd_193);
  RUN_TEST(test_threshold_nose_down_15_fwd_71);
  RUN_TEST(test_threshold_clamp_bounds);
  RUN_TEST(test_threshold_rear_mirrors_fwd);
  RUN_TEST(test_bump_climb_120_passes_at_plus15);
  RUN_TEST(test_level_2deg_fires);
  RUN_TEST(test_rev_far_level_drops_rev_only);
  RUN_TEST(test_rev_invalid_holds_default_true);
  RUN_TEST(test_dead_sources_hold_both_bits);
  RUN_TEST(test_sensor_rising_publishes_once);
  RUN_TEST(test_sensor_held_levels_silent);
  RUN_TEST(test_sensor_falling_publishes_once);
  RUN_TEST(test_sensor_reassert_after_clear);
  RUN_TEST(test_sensor_boot_into_cliff_publishes_once);
  RUN_TEST(test_sensor_frozen_inputs_silent);
  RUN_TEST(test_sensor_tof_sentinel_invalid);
  RUN_TEST(test_sensor_due_wrap_safe);
  RUN_TEST(test_ground_fwd_loss_publishes_packed_bits);
  RUN_TEST(test_ground_rev_loss_publishes_packed_bits);
  RUN_TEST(test_ground_either_rail_transition_only);
  RUN_TEST(test_ground_dead_ticks_hold_silence);
  RUN_TEST(test_ground_boot_into_void_publishes_once);
  RUN_TEST(test_coord_stick_bytes_map_center_128);
  RUN_TEST(test_coord_ground_bits_pack_unpack);
  RUN_TEST(test_coord_cliff_entry_brakes_and_latches);
  RUN_TEST(test_coord_entry_on_rev_edge);
  RUN_TEST(test_coord_forward_veto_while_latched);
  RUN_TEST(test_coord_reverse_allowed_while_latched);
  RUN_TEST(test_coord_reverse_veto_while_rev_latched);
  RUN_TEST(test_coord_escape_forward_while_rev_latched);
  RUN_TEST(test_coord_turn_allowed_while_latched);
  RUN_TEST(test_coord_latched_ignores_fresh_cmd_while_cliffed);
  RUN_TEST(test_coord_blocked_rev_never_exits);
  RUN_TEST(test_coord_stays_stopped_after_clear_without_new_cmd);
  RUN_TEST(test_coord_stale_held_stick_does_not_resume);
  RUN_TEST(test_coord_stale_fresh_never_exits);
  RUN_TEST(test_coord_centered_fresh_cmd_does_not_resume);
  RUN_TEST(test_coord_resumes_on_clear_plus_fresh_cmd);
  RUN_TEST(test_coord_resume_permitted_direction_while_other_void);
  RUN_TEST(test_coord_turn_resume_needs_either_rail);
  RUN_TEST(test_coord_both_false_blocks_everything);
  RUN_TEST(test_coord_done_advances_behavior);
  RUN_TEST(test_coord_running_maneuver_survives_centered_stick);
  RUN_TEST(test_coord_stale_udp_failsafe_stop);
  RUN_TEST(test_coord_drive_passes_throttle_raw);
  RUN_TEST(test_coord_centered_stick_releases_drive);
  RUN_TEST(test_coord_mismatched_done_does_not_clear);
  RUN_TEST(test_status_flags_pack_bits);
  RUN_TEST(test_status_flags_unpack_round_trip);
  RUN_TEST(test_pack_command_center_edges);
  RUN_TEST(test_telemetry_round_trip_with_flags_set);
  RUN_TEST(test_deg_to_decideg_edges_and_clamp);
  return UNITY_END();
}
