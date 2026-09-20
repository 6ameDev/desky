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
void test_cliff_far_and_unloaded();
void test_cliff_near_ground_false();
void test_cliff_gated_when_level();
void test_unhealthy_mpu_freezes_tilt_and_flags();
void test_invalid_tof_retains_distance_no_cliff_from_stale();

// Coordinator arbitrator tests, defined in test_coordinator.cpp.
void test_coord_stick_bytes_map_center_128();
void test_coord_cliff_entry_brakes_and_latches();
void test_coord_forward_veto_while_latched();
void test_coord_reverse_allowed_while_latched();
void test_coord_turn_allowed_while_latched();
void test_coord_latched_ignores_fresh_cmd_while_cliffed();
void test_coord_stays_stopped_after_clear_without_new_cmd();
void test_coord_stale_held_stick_does_not_resume();
void test_coord_centered_fresh_cmd_does_not_resume();
void test_coord_resumes_on_clear_plus_fresh_cmd();
void test_coord_done_advances_behavior();
void test_coord_running_maneuver_survives_centered_stick();
void test_coord_stale_udp_failsafe_stop();
void test_coord_drive_passes_throttle_raw();
void test_coord_centered_stick_releases_drive();
void test_coord_mismatched_done_does_not_clear();

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
  RUN_TEST(test_cliff_far_and_unloaded);
  RUN_TEST(test_cliff_near_ground_false);
  RUN_TEST(test_cliff_gated_when_level);
  RUN_TEST(test_unhealthy_mpu_freezes_tilt_and_flags);
  RUN_TEST(test_invalid_tof_retains_distance_no_cliff_from_stale);
  RUN_TEST(test_coord_stick_bytes_map_center_128);
  RUN_TEST(test_coord_cliff_entry_brakes_and_latches);
  RUN_TEST(test_coord_forward_veto_while_latched);
  RUN_TEST(test_coord_reverse_allowed_while_latched);
  RUN_TEST(test_coord_turn_allowed_while_latched);
  RUN_TEST(test_coord_latched_ignores_fresh_cmd_while_cliffed);
  RUN_TEST(test_coord_stays_stopped_after_clear_without_new_cmd);
  RUN_TEST(test_coord_stale_held_stick_does_not_resume);
  RUN_TEST(test_coord_centered_fresh_cmd_does_not_resume);
  RUN_TEST(test_coord_resumes_on_clear_plus_fresh_cmd);
  RUN_TEST(test_coord_done_advances_behavior);
  RUN_TEST(test_coord_running_maneuver_survives_centered_stick);
  RUN_TEST(test_coord_stale_udp_failsafe_stop);
  RUN_TEST(test_coord_drive_passes_throttle_raw);
  RUN_TEST(test_coord_centered_stick_releases_drive);
  RUN_TEST(test_coord_mismatched_done_does_not_clear);
  return UNITY_END();
}
