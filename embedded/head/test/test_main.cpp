// Head host-test runner: single-program main listing every test.
// (Mirrors core's test_udp_codec.cpp, which hosts the shared main.)
// Run: pio test -e native-test

#include <unity.h>

void setUp() {}
void tearDown() {}

void test_face_names_match_ids();
void test_face_name_unknown_id();
void test_face_id_from_name_all();
void test_face_id_from_name_rejects();
void test_face_clamp_valid_passthrough();
void test_face_clamp_falls_back_to_boot();
void test_head_event_names();
void test_boot_validation_lean0_all_on();
void test_boot_lean_nonzero_cam_off_oled_dim();
void test_cam_matrix_flips_both_ways();
void test_oled_matrix_all_rows();
void test_power_state_names();
void test_cli_cam_on_off();
void test_cli_cam_rejects();
void test_cli_face_numeric_and_name();
void test_cli_face_rejects();
void test_cli_oled_rows();
void test_cli_status_help_and_noise();
void test_link_face_numeric();
void test_link_face_rejects();
void test_link_cam_oled_verbs();
void test_link_noise_rejected();
void test_wifilink_seq_first_fresh_then_duplicate_then_stale();
void test_wifilink_seq_wraps();
void test_wifilink_type_names();
void test_wifilink_map_face();
void test_wifilink_map_oled_rows();
void test_wifilink_map_cam();
void test_wifilink_map_rejects_non_commands();
void test_wifilink_qr_frame_gate();
void test_wifilink_hb_and_retry_gates();
void test_wifilink_assoc_step_begins_once_then_waits();
void test_wifilink_assoc_step_connects_on_later_tick();
void test_wifilink_assoc_step_timeout_restarts();
void test_wifilink_assoc_step_wrap_safe();
void test_wifilink_link_down_rule();
void test_wifilink_frag_plan();
void test_cli_link_and_snap_parse();
void test_qr_payload_len_pins_link_contract();
void test_qr_geometry_fits_panel();
void test_qr_module_msb_first_row_major();
void test_qr_module_fail_closed();
void test_qr_e2e_encode_pack_display_identity();
void test_qr_bug_signature_first63_unpacked_differs();
void test_qr_face_id_appended_not_renumbered();
void test_cli_qr_and_qr_clear();
void test_qr_pairing_window_order_head();
void test_empty_table_suppresses_nothing();
void test_quirk_line_matches_with_sdk_prefix();
void test_other_gpio_errors_still_print();
void test_partial_phrase_does_not_match();
void test_null_and_empty_input_never_match();
void test_rejects_bad_rules();
void test_table_full_rejects_overflow();
void test_clear_resets_policy();

// Command LITERALS (literal wire bytes -> decode -> mapCmd + CLI + shared
// value table), defined in test_command_literals.cpp.
void test_cmd_literals_oled_wire_values();
void test_cmd_literals_camctl_wire_values();
void test_cmd_literals_face_ids_and_clamp();
void test_cmd_literals_cli_equivalence();
void test_cmd_literals_tampered_frames_reject();
void test_headapply_value_table();
// Shared codec vocabulary guards (literals -> pack/unpack -> rejects).
void test_cmd_literals_shared_codec_oled();
void test_cmd_literals_shared_codec_camctl();
void test_cmd_literals_shared_codec_face();

// Link FRAG X-WIRE mirror (same seam as core's test_link_frag_xwire.cpp),
// defined in test_link_frag_xwire.cpp.
void test_link_frag_xwire_geometry();
void test_link_frag_xwire_round_trip_all_sizes();
void test_link_frag_xwire_rejects_disorder_without_corruption();

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_face_names_match_ids);
  RUN_TEST(test_face_name_unknown_id);
  RUN_TEST(test_face_id_from_name_all);
  RUN_TEST(test_face_id_from_name_rejects);
  RUN_TEST(test_face_clamp_valid_passthrough);
  RUN_TEST(test_face_clamp_falls_back_to_boot);
  RUN_TEST(test_head_event_names);
  RUN_TEST(test_boot_validation_lean0_all_on);
  RUN_TEST(test_boot_lean_nonzero_cam_off_oled_dim);
  RUN_TEST(test_cam_matrix_flips_both_ways);
  RUN_TEST(test_oled_matrix_all_rows);
  RUN_TEST(test_power_state_names);
  RUN_TEST(test_cli_cam_on_off);
  RUN_TEST(test_cli_cam_rejects);
  RUN_TEST(test_cli_face_numeric_and_name);
  RUN_TEST(test_cli_face_rejects);
  RUN_TEST(test_cli_oled_rows);
  RUN_TEST(test_cli_status_help_and_noise);
  RUN_TEST(test_link_face_numeric);
  RUN_TEST(test_link_face_rejects);
  RUN_TEST(test_link_cam_oled_verbs);
  RUN_TEST(test_link_noise_rejected);
  RUN_TEST(test_wifilink_seq_first_fresh_then_duplicate_then_stale);
  RUN_TEST(test_wifilink_seq_wraps);
  RUN_TEST(test_wifilink_type_names);
  RUN_TEST(test_wifilink_map_face);
  RUN_TEST(test_wifilink_map_oled_rows);
  RUN_TEST(test_wifilink_map_cam);
  RUN_TEST(test_wifilink_map_rejects_non_commands);
  RUN_TEST(test_wifilink_qr_frame_gate);
  RUN_TEST(test_wifilink_hb_and_retry_gates);
  RUN_TEST(test_wifilink_assoc_step_begins_once_then_waits);
  RUN_TEST(test_wifilink_assoc_step_connects_on_later_tick);
  RUN_TEST(test_wifilink_assoc_step_timeout_restarts);
  RUN_TEST(test_wifilink_assoc_step_wrap_safe);
  RUN_TEST(test_wifilink_link_down_rule);
  RUN_TEST(test_wifilink_frag_plan);
  RUN_TEST(test_cli_link_and_snap_parse);
  RUN_TEST(test_qr_payload_len_pins_link_contract);
  RUN_TEST(test_qr_geometry_fits_panel);
  RUN_TEST(test_qr_module_msb_first_row_major);
  RUN_TEST(test_qr_module_fail_closed);
  RUN_TEST(test_qr_e2e_encode_pack_display_identity);
  RUN_TEST(test_qr_bug_signature_first63_unpacked_differs);
  RUN_TEST(test_qr_face_id_appended_not_renumbered);
  RUN_TEST(test_cli_qr_and_qr_clear);
  RUN_TEST(test_qr_pairing_window_order_head);
  RUN_TEST(test_empty_table_suppresses_nothing);
  RUN_TEST(test_quirk_line_matches_with_sdk_prefix);
  RUN_TEST(test_other_gpio_errors_still_print);
  RUN_TEST(test_partial_phrase_does_not_match);
  RUN_TEST(test_null_and_empty_input_never_match);
  RUN_TEST(test_rejects_bad_rules);
  RUN_TEST(test_table_full_rejects_overflow);
  RUN_TEST(test_clear_resets_policy);
  RUN_TEST(test_cmd_literals_oled_wire_values);
  RUN_TEST(test_cmd_literals_camctl_wire_values);
  RUN_TEST(test_cmd_literals_face_ids_and_clamp);
  RUN_TEST(test_cmd_literals_cli_equivalence);
  RUN_TEST(test_cmd_literals_tampered_frames_reject);
  RUN_TEST(test_headapply_value_table);
  RUN_TEST(test_cmd_literals_shared_codec_oled);
  RUN_TEST(test_cmd_literals_shared_codec_camctl);
  RUN_TEST(test_cmd_literals_shared_codec_face);
  RUN_TEST(test_link_frag_xwire_geometry);
  RUN_TEST(test_link_frag_xwire_round_trip_all_sizes);
  RUN_TEST(test_link_frag_xwire_rejects_disorder_without_corruption);
  return UNITY_END();
}
