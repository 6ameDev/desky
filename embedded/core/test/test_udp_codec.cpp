// Host unit tests for the UDP packet codec (no hardware).
// Run: pio test -e native-test

#include <unity.h>

#include "../src/transport/udp_codec.h"
#include "../src/transport/udp_server.h"

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

// ALIVE/DISCONNECT (robot alive rule): the new 0x55/0x56 headers never
// collide with the frozen control/telemetry headers owned by this codec,
// and their 14B size matches neither the 6B control nor the 9B telemetry
// shape — so neither legacy decode path can ever accept one.
void test_codec_alive_disconnect_headers_distinct() {
  TEST_ASSERT_TRUE(udp::kControlHeader != udpstatus::kAliveHeader);
  TEST_ASSERT_TRUE(udp::kControlHeader != udpstatus::kDisconnectHeader);
  TEST_ASSERT_TRUE(udp::kTelemetryHeader != udpstatus::kAliveHeader);
  TEST_ASSERT_TRUE(udp::kTelemetryHeader != udpstatus::kDisconnectHeader);
  TEST_ASSERT_TRUE(udpstatus::kAliveHeader != udpstatus::kDisconnectHeader);
  TEST_ASSERT_TRUE(udpstatus::kAliveSize != udp::kControlSize);
  TEST_ASSERT_TRUE(udpstatus::kAliveSize != udp::kTelemetrySize);
  TEST_ASSERT_TRUE(udpstatus::kDisconnectSize != udp::kControlSize);
  TEST_ASSERT_TRUE(udpstatus::kDisconnectSize != udp::kTelemetrySize);
}

// Sensor fusion tests, defined in test_sensor_fusion.cpp (pio test links all
// test/*.cpp into one binary, so the single main lives here).
void test_flat_rest_no_cliff_near_zero_tilt();
void test_nose_up_45_pitch_tracks_identity();
void test_nose_up_45_pitch_tracks_swapped();
void test_sign_convention_identity();
void test_sign_convention_swapped();
void test_board_mounting_matches_bench();
void test_tcrt_fwd_void_level_drops_fwd_and_fires_cliff();
void test_tcrt_fwd_ground_level_holds_clear();
void test_tcrt_tilted_void_abstains();
void test_tcrt_30deg_roll_fires();
void test_tcrt_level_2deg_fires();
void test_unhealthy_mpu_freezes_tilt_and_flags();
void test_invalid_tof_retains_distance_rails_still_vote();
void test_tcrt_tof_tracks_obstacle_only();
void test_rev_void_level_drops_rev_only();
void test_rev_invalid_holds_default_true();
void test_dead_sources_hold_both_bits();

// TCRT hysteresis rail tests, defined in test_tcrt.cpp.
void test_tcrt_rail_asserts_above_threshold_plus_hyst();
void test_tcrt_rail_clears_below_threshold_minus_hyst();
void test_tcrt_rail_holds_inside_band();
void test_tcrt_rail_full_sweep_latches();
void test_tcrt_rail_custom_thresholds();
void test_tcrt_rail_cfg_defaults_match_header();
void test_tcrt_rail_inverted_asserts_below_threshold_minus_hyst();
void test_tcrt_rail_inverted_clears_above_threshold_plus_hyst();
void test_tcrt_rail_inverted_bench_bands();

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
void test_obstacle_assert_fires_once_below_band();
void test_obstacle_rearm_needs_clear_above_band();
void test_obstacle_far_readings_never_fire();
void test_obstacle_dead_ticks_hold_silence();

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
// Pairing/auth v1 RX demux (Task 5), defined in test_udp_server.cpp.
void test_udp_wire_constants();
void test_udp_fresh_counter_greater_ok();
void test_udp_fresh_counter_equal_older_reject();
void test_udp_fresh_counter_wrap_accept();
void test_udp_fresh_counter_half_range_reject();
void test_udp_classify_table();
void test_udp_classify_wrong_header_or_size_drops();
void test_udp_redeliver_same_true();
void test_udp_redeliver_different_nonce_false();
void test_udp_redeliver_different_code_false();
void test_udp_redeliver_expired_false();
void test_udp_redeliver_invalid_or_null_false();
void test_udp_redeliver_millis_wrap_true();
void test_udp_unbond_wire_constants();
void test_udp_classify_unbond();
void test_udp_unbond_tag_msg_golden();
void test_udp_rekey_wire_constants();
void test_udp_classify_rekey();
void test_udp_alive_disconnect_wire_constants();
void test_udp_classify_alive_disconnect();
void test_udp_claimed_headers_distinct();
void test_udp_alive_fresh_inside_window();
void test_udp_alive_stale_past_window();
void test_udp_alive_fresh_millis_wrap();
void test_udp_telemetry_gate_unicast_when_bonded_alive();
void test_udp_telemetry_gate_silent_when_stale();
void test_udp_telemetry_gate_silent_when_no_peer();
void test_udp_telemetry_gate_silent_pre_bond();
void test_udp_telemetry_gate_wrap();
void test_udp_frozen_sizes();
void test_conn_ip_u32_roundtrip();
void test_conn_initial_cleared();
void test_conn_markalive_pins_and_alive();
void test_conn_markalive_rejects_unverified();
void test_conn_refresh_then_clear();
void test_conn_pin_peer_no_stamp();
void test_conn_isalive_wrap_safe();
void test_tokens_move_transfers_validity();
void test_telemetry_encode_snapshot_golden();
void test_telemetry_publish_sends_when_due();
void test_telemetry_publish_silent_without_gate();
void test_telemetry_publish_holds_cadence();
void test_telemetry_publish_edge_forces_early();

// I2S audio DSP + meter policy, defined in test_audio.cpp.
void test_audio_dual_mono32_top_justified();
void test_audio_chime_shape_and_decay();
void test_audio_int24_unpack_top24_in_32();
void test_audio_rms_and_louder_side();

// Pairing core (Tier 1 enrolment gate), defined in test_pairing.cpp.
void test_pair_golden_accept_burns_once();
void test_pair_bad_code_counts_tries();
void test_pair_fifth_try_locks_then_expires();
void test_pair_window_expiry_rejects();
void test_pair_pre_window_rejects();
void test_pair_malformed_input_is_bad_code();
void test_pair_persist_every_guard();
void test_pair_wrap_layout_consts();
void test_rekey_wire_consts();
void test_rekey_tag_msg_golden();
void test_rekey_ack_tag_msg_golden();
void test_alive_disconnect_wire_consts();
void test_alive_tag_msg_golden();
void test_disconnect_tag_msg_golden();
void test_pair_core_bond_lifecycle();
void test_pair_wire_reasons_frozen();
// M9 three-way mirror gate (defined in test_udp_server.cpp).
void test_udp_mirror_freeze();
void test_udp_pair_req_frame_golden();
void test_udp_pair_err_frame_golden();
void test_udp_pair_err_reason4_frame_golden();
void test_udp_drive_v2_frame_golden();
void test_udp_unbond_frame_golden();
void test_udp_rekey_frame_goldens();
void test_udp_alive_disconnect_frame_goldens();
void test_udp_pair_ok_framing();
void test_udp_pack_qr_bitmap_bridge();

// QR pairing bitmap (Phase 0), defined in test_qr_pairing.cpp.
void test_qr_size_is_21();
void test_qr_golden_encodes();
void test_qr_finder_patterns_three_corners();
void test_qr_timing_patterns();
void test_qr_deterministic_reencode_equal();
void test_qr_rejects_null_short_nondigit();
void test_qr_row_bytes_msb_first();
void test_qr_packed_matches_golden_rows();
void test_qr_pack_equals_rowbytes_concat();
void test_qr_rs_syndrome_zero();
void test_qr_boundary_goldens_self_consistent();

// Link frame codec (B-link core<->head), defined in test_link_codec.cpp.
void test_link_face_golden_encode();
void test_link_crc16_standard_vector();
void test_link_face_golden_decode();
void test_link_hb_golden_encode();
void test_link_hb_golden_decode();
void test_link_rejects_bad_magic();
void test_link_rejects_bad_crc();
void test_link_rejects_length_mismatch();
void test_link_null_safety();
void test_link_encode_rejects_bad_args();
void test_link_decode_rejects_bad_frag_and_unknown_type();
void test_link_qr_exact63_accept();
void test_link_qr_rejects_62_and_64();
void test_link_jpeg_max_boundary();
void test_link_fixed_type_lengths();
void test_link_round_trip_all_types();
void test_link_hb_ack_ack_pack();
void test_link_fresh_seq_golden();
void test_link_fresh_seq_wrap();
void test_link_mtu_helpers();
void test_link_reassembly_in_order();
void test_link_reassembly_duplicate_reject();
void test_link_reassembly_total_mismatch_reject();
void test_link_reassembly_overlap_and_gap_reject();
void test_link_reassembly_overflow_reject();
void test_link_phone_wire_headers_distinct();
void test_link_pairshow_golden_pack();
void test_link_pairshow_golden_frame();
void test_link_pairshow_round_trip_edges();
void test_link_pairshow_rejects();
void test_link_pairdone_empty_frame();

// Link command vocabulary (producer side: future stageFace/stageOled/stageCam
// MUST pack through these), defined in test_link_cmd_vocab.cpp.
void test_link_cmd_vocab_oled_literal_round_trip();
void test_link_cmd_vocab_camctl_literal_round_trip();
void test_link_cmd_vocab_ota_literal_length();
void test_link_cmd_vocab_face_literal_range();

// Link endpoint helpers (H2.2 core link task), defined in test_link_server.cpp.
void test_linkserver_first_frame_fresh();
void test_linkserver_duplicate_stale_fresh();
void test_linkserver_pull_framing();
void test_linkserver_type_names();
void test_linkserver_qr_gate_once_per_window_with_peer();
void test_linkserver_qr_pin_rule();
void test_linkserver_qr_frame_63_round_trip();
void test_linkserver_boot_epoch_resync_gate();

// Telemetry X-WIRE (literal producer bytes -> real consumer + independent
// parse), defined in test_telemetry_xwire.cpp.
void test_telemetry_xwire_encode_matches_literals();
void test_telemetry_xwire_independent_field_parse();
void test_telemetry_xwire_literals_are_live_frames();
void test_telemetry_note_driving_while_fresh();
void test_telemetry_note_zero_stick_never_drives();
void test_telemetry_note_wrap_safe_millis();

// Link FRAG X-WIRE (head fragSlice -> encode -> pull -> decode -> reassemble),
// defined in test_link_frag_xwire.cpp.
void test_link_frag_xwire_geometry();
void test_link_frag_xwire_round_trip_all_sizes();
void test_link_frag_xwire_rejects_disorder_without_corruption();

// Carrier OTA trigger (kTypeOta bump + manifest + `otahead` verb), defined
// in test_link_ota.cpp.
void test_link_ota_golden_encode();
void test_link_ota_golden_decode();
void test_link_ota_staged_url_round_trip_identical();
void test_link_ota_length_gates_reject_0_and_129();
void test_link_ota_known_type_literal_set();
void test_link_ota_old_head_drops_as_unknown();
void test_linkserver_ota_gate_once_per_command_with_peer();
void test_linkserver_ota_type_name();
void test_otamanifest_ota_url_grammar();
void test_otamanifest_build_parse_round_trip();
void test_otamanifest_sibling_url_and_gate();
void test_corecli_otahead_verb();
void test_corecli_otahead_tail_split();
void test_otamanifest_upload_version_decision();
void test_linkserver_ota_restage_rearms_send();

// OTA staging ability (Slice 5 codec+manager extraction), defined in
// test_ota_staging.cpp.
void test_ota_codec_routes_and_caps();
void test_ota_codec_parse_version_param();
void test_ota_codec_sha_helpers();
void test_ota_mgr_empty_views();
void test_ota_mgr_commit_adopts_version();
void test_ota_mgr_no_version_keeps_label();
void test_ota_mgr_version_atomic_at_commit();
void test_ota_mgr_bad_version_rejects_preserving();
void test_ota_mgr_overflow_resets_inflight();
void test_ota_mgr_busy_view_during_upload();
void test_ota_mgr_no_storage_stops_upload();
void test_ota_mgr_digest_fail_clears_sha();
void test_ota_mgr_final_without_start_fails();

// Checked UDP send helper (shared/common/udp_send.h), defined in test_udp_send.cpp.
void test_udpsend_all_ok_true();
void test_udpsend_begin_fail_false();
void test_udpsend_short_write_false();
void test_udpsend_end_fail_false();
void test_udpsend_all_fail_false();

// Cross-constant relations table (gap 9), defined in test_units_table.cpp.
void test_units_qr_pairing_window_order();
void test_units_telemetry_rate_product();
void test_units_alive_stale_fence();
void test_units_linkdown_cadence();
void test_units_rekey_margin_seconds();

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_control_golden_vector);
  RUN_TEST(test_control_decode_round_trip);
  RUN_TEST(test_control_rejects_bad_header_checksum_length);
  RUN_TEST(test_telemetry_golden_vector);
  RUN_TEST(test_telemetry_decode_round_trip_negative);
  RUN_TEST(test_telemetry_rejects_bad_header_checksum);
  RUN_TEST(test_codec_alive_disconnect_headers_distinct);
  RUN_TEST(test_flat_rest_no_cliff_near_zero_tilt);
  RUN_TEST(test_nose_up_45_pitch_tracks_identity);
  RUN_TEST(test_nose_up_45_pitch_tracks_swapped);
  RUN_TEST(test_sign_convention_identity);
  RUN_TEST(test_sign_convention_swapped);
  RUN_TEST(test_board_mounting_matches_bench);
  RUN_TEST(test_tcrt_fwd_void_level_drops_fwd_and_fires_cliff);
  RUN_TEST(test_tcrt_fwd_ground_level_holds_clear);
  RUN_TEST(test_tcrt_tilted_void_abstains);
  RUN_TEST(test_tcrt_30deg_roll_fires);
  RUN_TEST(test_tcrt_level_2deg_fires);
  RUN_TEST(test_unhealthy_mpu_freezes_tilt_and_flags);
  RUN_TEST(test_invalid_tof_retains_distance_rails_still_vote);
  RUN_TEST(test_tcrt_tof_tracks_obstacle_only);
  RUN_TEST(test_rev_void_level_drops_rev_only);
  RUN_TEST(test_rev_invalid_holds_default_true);
  RUN_TEST(test_dead_sources_hold_both_bits);
  RUN_TEST(test_tcrt_rail_asserts_above_threshold_plus_hyst);
  RUN_TEST(test_tcrt_rail_clears_below_threshold_minus_hyst);
  RUN_TEST(test_tcrt_rail_holds_inside_band);
  RUN_TEST(test_tcrt_rail_full_sweep_latches);
  RUN_TEST(test_tcrt_rail_custom_thresholds);
  RUN_TEST(test_tcrt_rail_cfg_defaults_match_header);
  RUN_TEST(test_tcrt_rail_inverted_asserts_below_threshold_minus_hyst);
  RUN_TEST(test_tcrt_rail_inverted_clears_above_threshold_plus_hyst);
  RUN_TEST(test_tcrt_rail_inverted_bench_bands);
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
  RUN_TEST(test_obstacle_assert_fires_once_below_band);
  RUN_TEST(test_obstacle_rearm_needs_clear_above_band);
  RUN_TEST(test_obstacle_far_readings_never_fire);
  RUN_TEST(test_obstacle_dead_ticks_hold_silence);
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
  RUN_TEST(test_udp_wire_constants);
  RUN_TEST(test_udp_fresh_counter_greater_ok);
  RUN_TEST(test_udp_fresh_counter_equal_older_reject);
  RUN_TEST(test_udp_fresh_counter_wrap_accept);
  RUN_TEST(test_udp_fresh_counter_half_range_reject);
  RUN_TEST(test_udp_classify_table);
  RUN_TEST(test_udp_classify_wrong_header_or_size_drops);
  RUN_TEST(test_udp_redeliver_same_true);
  RUN_TEST(test_udp_redeliver_different_nonce_false);
  RUN_TEST(test_udp_redeliver_different_code_false);
  RUN_TEST(test_udp_redeliver_expired_false);
  RUN_TEST(test_udp_redeliver_invalid_or_null_false);
  RUN_TEST(test_udp_redeliver_millis_wrap_true);
  RUN_TEST(test_udp_unbond_wire_constants);
  RUN_TEST(test_udp_classify_unbond);
  RUN_TEST(test_udp_unbond_tag_msg_golden);
  RUN_TEST(test_udp_rekey_wire_constants);
  RUN_TEST(test_udp_classify_rekey);
  RUN_TEST(test_udp_alive_disconnect_wire_constants);
  RUN_TEST(test_udp_classify_alive_disconnect);
  RUN_TEST(test_udp_claimed_headers_distinct);
  RUN_TEST(test_udp_alive_fresh_inside_window);
  RUN_TEST(test_udp_alive_stale_past_window);
  RUN_TEST(test_udp_alive_fresh_millis_wrap);
  RUN_TEST(test_udp_telemetry_gate_unicast_when_bonded_alive);
  RUN_TEST(test_udp_telemetry_gate_silent_when_stale);
  RUN_TEST(test_udp_telemetry_gate_silent_when_no_peer);
  RUN_TEST(test_udp_telemetry_gate_silent_pre_bond);
  RUN_TEST(test_udp_telemetry_gate_wrap);
  RUN_TEST(test_udp_frozen_sizes);
  RUN_TEST(test_conn_ip_u32_roundtrip);
  RUN_TEST(test_conn_initial_cleared);
  RUN_TEST(test_conn_markalive_pins_and_alive);
  RUN_TEST(test_conn_markalive_rejects_unverified);
  RUN_TEST(test_conn_refresh_then_clear);
  RUN_TEST(test_conn_pin_peer_no_stamp);
  RUN_TEST(test_conn_isalive_wrap_safe);
  RUN_TEST(test_tokens_move_transfers_validity);
  RUN_TEST(test_telemetry_encode_snapshot_golden);
  RUN_TEST(test_telemetry_publish_sends_when_due);
  RUN_TEST(test_telemetry_publish_silent_without_gate);
  RUN_TEST(test_telemetry_publish_holds_cadence);
  RUN_TEST(test_telemetry_publish_edge_forces_early);
  RUN_TEST(test_audio_dual_mono32_top_justified);
  RUN_TEST(test_audio_chime_shape_and_decay);
  RUN_TEST(test_audio_int24_unpack_top24_in_32);
  RUN_TEST(test_audio_rms_and_louder_side);
  RUN_TEST(test_pair_golden_accept_burns_once);
  RUN_TEST(test_pair_bad_code_counts_tries);
  RUN_TEST(test_pair_fifth_try_locks_then_expires);
  RUN_TEST(test_pair_window_expiry_rejects);
  RUN_TEST(test_pair_pre_window_rejects);
  RUN_TEST(test_pair_malformed_input_is_bad_code);
  RUN_TEST(test_pair_persist_every_guard);
  RUN_TEST(test_pair_wrap_layout_consts);
  RUN_TEST(test_rekey_wire_consts);
  RUN_TEST(test_rekey_tag_msg_golden);
  RUN_TEST(test_rekey_ack_tag_msg_golden);
  RUN_TEST(test_alive_disconnect_wire_consts);
  RUN_TEST(test_alive_tag_msg_golden);
  RUN_TEST(test_disconnect_tag_msg_golden);
  RUN_TEST(test_pair_core_bond_lifecycle);
  RUN_TEST(test_pair_wire_reasons_frozen);
  RUN_TEST(test_udp_mirror_freeze);
  RUN_TEST(test_udp_pair_req_frame_golden);
  RUN_TEST(test_udp_pair_err_frame_golden);
  RUN_TEST(test_udp_pair_err_reason4_frame_golden);
  RUN_TEST(test_udp_drive_v2_frame_golden);
  RUN_TEST(test_udp_unbond_frame_golden);
  RUN_TEST(test_udp_rekey_frame_goldens);
  RUN_TEST(test_udp_alive_disconnect_frame_goldens);
  RUN_TEST(test_udp_pair_ok_framing);
  RUN_TEST(test_udp_pack_qr_bitmap_bridge);
  RUN_TEST(test_qr_size_is_21);
  RUN_TEST(test_qr_golden_encodes);
  RUN_TEST(test_qr_finder_patterns_three_corners);
  RUN_TEST(test_qr_timing_patterns);
  RUN_TEST(test_qr_deterministic_reencode_equal);
  RUN_TEST(test_qr_rejects_null_short_nondigit);
  RUN_TEST(test_qr_row_bytes_msb_first);
  RUN_TEST(test_qr_packed_matches_golden_rows);
  RUN_TEST(test_qr_pack_equals_rowbytes_concat);
  RUN_TEST(test_qr_rs_syndrome_zero);
  RUN_TEST(test_qr_boundary_goldens_self_consistent);
  RUN_TEST(test_link_crc16_standard_vector);
  RUN_TEST(test_link_face_golden_encode);
  RUN_TEST(test_link_face_golden_decode);
  RUN_TEST(test_link_hb_golden_encode);
  RUN_TEST(test_link_hb_golden_decode);
  RUN_TEST(test_link_rejects_bad_magic);
  RUN_TEST(test_link_rejects_bad_crc);
  RUN_TEST(test_link_rejects_length_mismatch);
  RUN_TEST(test_link_null_safety);
  RUN_TEST(test_link_encode_rejects_bad_args);
  RUN_TEST(test_link_decode_rejects_bad_frag_and_unknown_type);
  RUN_TEST(test_link_qr_exact63_accept);
  RUN_TEST(test_link_qr_rejects_62_and_64);
  RUN_TEST(test_link_jpeg_max_boundary);
  RUN_TEST(test_link_fixed_type_lengths);
  RUN_TEST(test_link_round_trip_all_types);
  RUN_TEST(test_link_hb_ack_ack_pack);
  RUN_TEST(test_link_fresh_seq_golden);
  RUN_TEST(test_link_fresh_seq_wrap);
  RUN_TEST(test_link_mtu_helpers);
  RUN_TEST(test_link_reassembly_in_order);
  RUN_TEST(test_link_reassembly_duplicate_reject);
  RUN_TEST(test_link_reassembly_total_mismatch_reject);
  RUN_TEST(test_link_reassembly_overlap_and_gap_reject);
  RUN_TEST(test_link_reassembly_overflow_reject);
  RUN_TEST(test_link_phone_wire_headers_distinct);
  RUN_TEST(test_link_pairshow_golden_pack);
  RUN_TEST(test_link_pairshow_golden_frame);
  RUN_TEST(test_link_pairshow_round_trip_edges);
  RUN_TEST(test_link_pairshow_rejects);
  RUN_TEST(test_link_pairdone_empty_frame);
  RUN_TEST(test_link_cmd_vocab_oled_literal_round_trip);
  RUN_TEST(test_link_cmd_vocab_camctl_literal_round_trip);
  RUN_TEST(test_link_cmd_vocab_ota_literal_length);
  RUN_TEST(test_link_cmd_vocab_face_literal_range);
  RUN_TEST(test_linkserver_first_frame_fresh);
  RUN_TEST(test_linkserver_duplicate_stale_fresh);
  RUN_TEST(test_linkserver_pull_framing);
  RUN_TEST(test_linkserver_type_names);
  RUN_TEST(test_linkserver_qr_gate_once_per_window_with_peer);
  RUN_TEST(test_linkserver_qr_pin_rule);
  RUN_TEST(test_linkserver_qr_frame_63_round_trip);
  RUN_TEST(test_linkserver_boot_epoch_resync_gate);
  RUN_TEST(test_link_ota_golden_encode);
  RUN_TEST(test_link_ota_golden_decode);
  RUN_TEST(test_link_ota_staged_url_round_trip_identical);
  RUN_TEST(test_link_ota_length_gates_reject_0_and_129);
  RUN_TEST(test_link_ota_known_type_literal_set);
  RUN_TEST(test_link_ota_old_head_drops_as_unknown);
  RUN_TEST(test_linkserver_ota_gate_once_per_command_with_peer);
  RUN_TEST(test_linkserver_ota_type_name);
  RUN_TEST(test_otamanifest_ota_url_grammar);
  RUN_TEST(test_otamanifest_build_parse_round_trip);
  RUN_TEST(test_otamanifest_sibling_url_and_gate);
  RUN_TEST(test_corecli_otahead_verb);
  RUN_TEST(test_corecli_otahead_tail_split);
  RUN_TEST(test_otamanifest_upload_version_decision);
  RUN_TEST(test_linkserver_ota_restage_rearms_send);
  RUN_TEST(test_ota_codec_routes_and_caps);
  RUN_TEST(test_ota_codec_parse_version_param);
  RUN_TEST(test_ota_codec_sha_helpers);
  RUN_TEST(test_ota_mgr_empty_views);
  RUN_TEST(test_ota_mgr_commit_adopts_version);
  RUN_TEST(test_ota_mgr_no_version_keeps_label);
  RUN_TEST(test_ota_mgr_version_atomic_at_commit);
  RUN_TEST(test_ota_mgr_bad_version_rejects_preserving);
  RUN_TEST(test_ota_mgr_overflow_resets_inflight);
  RUN_TEST(test_ota_mgr_busy_view_during_upload);
  RUN_TEST(test_ota_mgr_no_storage_stops_upload);
  RUN_TEST(test_ota_mgr_digest_fail_clears_sha);
  RUN_TEST(test_ota_mgr_final_without_start_fails);
  RUN_TEST(test_telemetry_xwire_encode_matches_literals);
  RUN_TEST(test_telemetry_xwire_independent_field_parse);
  RUN_TEST(test_telemetry_xwire_literals_are_live_frames);
  RUN_TEST(test_telemetry_note_driving_while_fresh);
  RUN_TEST(test_telemetry_note_zero_stick_never_drives);
  RUN_TEST(test_telemetry_note_wrap_safe_millis);
  RUN_TEST(test_link_frag_xwire_geometry);
  RUN_TEST(test_link_frag_xwire_round_trip_all_sizes);
  RUN_TEST(test_link_frag_xwire_rejects_disorder_without_corruption);
  RUN_TEST(test_udpsend_all_ok_true);
  RUN_TEST(test_udpsend_begin_fail_false);
  RUN_TEST(test_udpsend_short_write_false);
  RUN_TEST(test_udpsend_end_fail_false);
  RUN_TEST(test_udpsend_all_fail_false);
  RUN_TEST(test_units_qr_pairing_window_order);
  RUN_TEST(test_units_telemetry_rate_product);
  RUN_TEST(test_units_alive_stale_fence);
  RUN_TEST(test_units_linkdown_cadence);
  RUN_TEST(test_units_rekey_margin_seconds);
  return UNITY_END();
}
