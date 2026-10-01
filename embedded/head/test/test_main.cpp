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
void test_empty_table_suppresses_nothing();
void test_quirk_line_matches_with_sdk_prefix();
void test_other_gpio_errors_still_print();
void test_partial_phrase_does_not_match();
void test_null_and_empty_input_never_match();
void test_rejects_bad_rules();
void test_table_full_rejects_overflow();
void test_clear_resets_policy();

void test_uart_crc_known_vectors();
void test_uart_roundtrip_all_types();
void test_uart_roundtrip_empty_and_max();
void test_uart_encode_rejects();
void test_uart_hdr_crc_kill();
void test_uart_pay_crc_kill();
void test_uart_magic_resync();
void test_uart_byte_at_a_time();
void test_uart_overlong_dropped();
void test_uart_back_to_back();
void test_uart_reasm_basic();
void test_uart_reasm_ooo_and_duplicate();
void test_uart_reasm_loss_then_stale();
void test_uart_reasm_cap_drop();
void test_uart_reasm_oversize_vs_dropped();
void test_uart_reasm_stale_due();
void test_uart_config_set_clamp();
void test_uart_config_reject();
void test_uart_config_get();
void test_uart_framesize_parse();
void test_uart_framesize_name();
void test_uart_config_set_framesize();
void test_synth_totals_and_flags();
void test_synth_ramp_pattern();
void test_synth_jpeg_markers_and_determinism();
void test_synth_text_shape();
void test_synth_null_safe();
void test_synth_mem_loopback_modes012();

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
  RUN_TEST(test_empty_table_suppresses_nothing);
  RUN_TEST(test_quirk_line_matches_with_sdk_prefix);
  RUN_TEST(test_other_gpio_errors_still_print);
  RUN_TEST(test_partial_phrase_does_not_match);
  RUN_TEST(test_null_and_empty_input_never_match);
  RUN_TEST(test_rejects_bad_rules);
  RUN_TEST(test_table_full_rejects_overflow);
  RUN_TEST(test_clear_resets_policy);
  RUN_TEST(test_uart_crc_known_vectors);
  RUN_TEST(test_uart_roundtrip_all_types);
  RUN_TEST(test_uart_roundtrip_empty_and_max);
  RUN_TEST(test_uart_encode_rejects);
  RUN_TEST(test_uart_hdr_crc_kill);
  RUN_TEST(test_uart_pay_crc_kill);
  RUN_TEST(test_uart_magic_resync);
  RUN_TEST(test_uart_byte_at_a_time);
  RUN_TEST(test_uart_overlong_dropped);
  RUN_TEST(test_uart_back_to_back);
  RUN_TEST(test_uart_reasm_basic);
  RUN_TEST(test_uart_reasm_ooo_and_duplicate);
  RUN_TEST(test_uart_reasm_loss_then_stale);
  RUN_TEST(test_uart_reasm_cap_drop);
  RUN_TEST(test_uart_reasm_oversize_vs_dropped);
  RUN_TEST(test_uart_reasm_stale_due);
  RUN_TEST(test_uart_config_set_clamp);
  RUN_TEST(test_uart_config_reject);
  RUN_TEST(test_uart_config_get);
  RUN_TEST(test_uart_framesize_parse);
  RUN_TEST(test_uart_framesize_name);
  RUN_TEST(test_uart_config_set_framesize);
  RUN_TEST(test_synth_totals_and_flags);
  RUN_TEST(test_synth_ramp_pattern);
  RUN_TEST(test_synth_jpeg_markers_and_determinism);
  RUN_TEST(test_synth_text_shape);
  RUN_TEST(test_synth_null_safe);
  RUN_TEST(test_synth_mem_loopback_modes012);
  return UNITY_END();
}
