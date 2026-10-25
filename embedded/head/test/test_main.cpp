// Host-test runner: single-program main listing every test.

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
void test_display_port_rail_contract();
void test_wifilink_seq_first_fresh_then_duplicate_then_stale();
void test_wifilink_seq_wraps();
void test_wifilink_type_names();
void test_wifilink_map_retired_face_drops();
void test_wifilink_map_oled_rows();
void test_wifilink_map_cam();
void test_wifilink_map_rejects_non_commands();
void test_wifilink_hb_and_retry_gates();
void test_wifilink_assoc_step_begins_once_then_waits();
void test_wifilink_assoc_step_connects_on_later_tick();
void test_wifilink_assoc_step_timeout_restarts();
void test_wifilink_assoc_step_wrap_safe();
void test_wifilink_link_down_rule();
void test_wifilink_frag_plan();
void test_qr_payload_len_pins_display_contract();
void test_qr_geometry_fits_panel();
void test_qr_module_msb_first_row_major();
void test_qr_module_fail_closed();
void test_qr_e2e_encode_pack_display_identity();
void test_qr_bug_signature_first63_unpacked_differs();
void test_qr_face_id_appended_not_renumbered();
void test_qr_pairing_window_order_head();
void test_empty_table_suppresses_nothing();
void test_quirk_line_matches_with_sdk_prefix();
void test_other_gpio_errors_still_print();
void test_partial_phrase_does_not_match();
void test_null_and_empty_input_never_match();
void test_rejects_bad_rules();
void test_table_full_rejects_overflow();
void test_clear_resets_policy();

// Command LITERALS, defined in test_command_literals.cpp.
void test_cmd_literals_oled_wire_values();
void test_cmd_literals_camctl_wire_values();
void test_cmd_literals_tampered_frames_reject();
void test_headapply_value_table();

void test_cmd_literals_shared_codec_oled();
void test_cmd_literals_shared_codec_camctl();

// Link FRAG X-WIRE mirror, defined in test_link_frag_xwire.cpp.
void test_link_frag_xwire_geometry();
void test_link_frag_xwire_round_trip_all_sizes();
void test_link_frag_xwire_rejects_disorder_without_corruption();

// Carrier OTA consumer, defined in test_link_ota.cpp.
void test_link_ota_producer_bytes_to_updater_exact();
void test_link_ota_literal_contract();
void test_link_ota_duplicate_reack_and_stale_drop_cases();
void test_link_ota_old_head_drops_as_unknown();
void test_otadisplay_geometry_and_fill();
void test_otadisplay_state_transitions();
void test_link_ota_version_vocabulary_and_gate();
void test_link_ota_channel_policy();

// Dual-mode follow order + credential bundle, defined in test_link_follow.cpp.
void test_follow_first_target_matrix();
void test_follow_advance_order_and_skips();
void test_follow_backoff_config();
void test_follow_creds_literal_contract();
void test_follow_creds_plaintext_golden();
void test_follow_store_validity();

// Authoritative-PIN pairing display, defined in test_pair_show.cpp.
void test_pairshow_literal_contract();
void test_pairshow_shared_codec_pack_unpack();
void test_pairshow_producer_bytes_to_renderer_exact();
void test_pairshow_duplicate_reack_and_stale_drop_cases();
void test_pairdisplay_pin_validation_and_state();
void test_pairdisplay_deadline_expiry();
void test_pairdisplay_pin_line_geometry();
void test_pairdisplay_countdown_format();
void test_pairdisplay_remaining_secs();
void test_pair_manager_second_gate();
void test_pairshow_pin_to_bitmap_equivalence();
void test_pairing_manager_bad_pin_noop();
void test_pairing_manager_show_rearms_deadline();
void test_pairing_manager_clear_when_idle_noop();
void test_pairing_manager_show_clear_roundtrip();
void test_pairing_manager_null_face_safe();

// OTA pull client + CLI `ota <url>` verb, defined in test_ota_client.cpp.
void test_ota_parse_plain_host();
void test_ota_parse_port_and_deep_path();
void test_ota_parse_bare_host_defaults_path();
void test_ota_parse_rejects();
void test_ota_version_orders();
void test_ota_magic();
void test_ota_fits_slot();
void test_ota_stream_gate();
void test_ota_manifest_matches();
void test_ota_bytes_to_hex();
void test_ota_state_happy_path();
void test_ota_state_verify_fail_and_abort();
void test_ota_state_rejects_bad_events();

void test_eyes_map_request_ids();
void test_eyes_map_ordinals_track_emotions();
void test_eyes_map_v2_rows();
void test_eyes_map_falls_back_to_normal();

void test_exprcat_ids_mirror_face_vocabulary();
void test_exprcat_count_and_order();
void test_exprcat_names();
void test_exprcat_priorities();
void test_exprcat_play_once_and_hold();
void test_exprcat_eye_commands();
void test_exprcat_placeholders_none();
void test_exprcat_v2_bundles();
void test_exprcat_lookup_fail_closed();
void test_exprcat_accessors_fail_closed();
void test_exprcat_matches_head_tables();
void test_exprcat_v2_eye_map();

void test_expr_priority_order();
void test_expr_play_once_shape();
void test_expr_paint_stores_every_id();
void test_expr_paint_unknown_clamps_boot();
void test_expr_paint_held_pairing_rejects_zero_change();
void test_expr_paint_held_ota_rejects_zero_change();
void test_expr_paint_clears_qr_only();
void test_expr_paint_applies_after_overlay_clears();
void test_expr_trigger_paths_cover_all_five();

// Core->head expression command (EXPR 0x08 gate + arbitration), defined in
// test_link_expr.cpp.
void test_link_expr_literal_contract();
void test_link_expr_golden_round_trip();
void test_link_expr_unpack_fail_closed();
void test_link_expr_maps_to_face_gate();
void test_link_expr_unknown_maps_neutral();
void test_link_expr_v2_ids_route_to_paint();
void test_link_expr_gated_entry_routes_eyes();
void test_link_expr_pairing_screen_survives_command();
void test_link_expr_ota_screen_survives_command();
void test_link_expr_bench_path_intact();

// Shared link crypto contract (shared/link/link_crypto.h), defined in
// test_link_crypto.cpp.
void test_linkcrypto_provision_exact_length_only();
void test_linkcrypto_key_constants();
void test_linkcrypto_counter_nonce_layout();
void test_linkcrypto_gcm_zero_empty_vector();
void test_linkcrypto_gcm_zero_16_vector();
void test_linkcrypto_seal_deterministic_golden();
void test_linkcrypto_round_trip_sizes();
void test_linkcrypto_tamper_rejected();
void test_linkcrypto_wrong_key_and_nonce_rejected();
void test_linkcrypto_truncated_and_short_rejected();
void test_linkcrypto_nonce_reuse_is_visible();
void test_linkcrypto_null_and_cap_safety();
void test_linkcrypto_open_failure_zeroes_output();
void test_linkcrypto_passphrase_goldens();
void test_linkcrypto_passphrase_shape();

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
  RUN_TEST(test_display_port_rail_contract);
  RUN_TEST(test_wifilink_seq_first_fresh_then_duplicate_then_stale);
  RUN_TEST(test_wifilink_seq_wraps);
  RUN_TEST(test_wifilink_type_names);
  RUN_TEST(test_wifilink_map_retired_face_drops);
  RUN_TEST(test_wifilink_map_oled_rows);
  RUN_TEST(test_wifilink_map_cam);
  RUN_TEST(test_wifilink_map_rejects_non_commands);
  RUN_TEST(test_wifilink_hb_and_retry_gates);
  RUN_TEST(test_wifilink_assoc_step_begins_once_then_waits);
  RUN_TEST(test_wifilink_assoc_step_connects_on_later_tick);
  RUN_TEST(test_wifilink_assoc_step_timeout_restarts);
  RUN_TEST(test_wifilink_assoc_step_wrap_safe);
  RUN_TEST(test_wifilink_link_down_rule);
  RUN_TEST(test_wifilink_frag_plan);
  RUN_TEST(test_qr_payload_len_pins_display_contract);
  RUN_TEST(test_qr_geometry_fits_panel);
  RUN_TEST(test_qr_module_msb_first_row_major);
  RUN_TEST(test_qr_module_fail_closed);
  RUN_TEST(test_qr_e2e_encode_pack_display_identity);
  RUN_TEST(test_qr_bug_signature_first63_unpacked_differs);
  RUN_TEST(test_qr_face_id_appended_not_renumbered);
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
  RUN_TEST(test_cmd_literals_tampered_frames_reject);
  RUN_TEST(test_headapply_value_table);
  RUN_TEST(test_cmd_literals_shared_codec_oled);
  RUN_TEST(test_cmd_literals_shared_codec_camctl);
  RUN_TEST(test_link_frag_xwire_geometry);
  RUN_TEST(test_link_frag_xwire_round_trip_all_sizes);
  RUN_TEST(test_link_frag_xwire_rejects_disorder_without_corruption);
  RUN_TEST(test_link_ota_producer_bytes_to_updater_exact);
  RUN_TEST(test_link_ota_literal_contract);
  RUN_TEST(test_link_ota_duplicate_reack_and_stale_drop_cases);
  RUN_TEST(test_link_ota_old_head_drops_as_unknown);
  RUN_TEST(test_otadisplay_geometry_and_fill);
  RUN_TEST(test_otadisplay_state_transitions);
  RUN_TEST(test_link_ota_version_vocabulary_and_gate);
  RUN_TEST(test_link_ota_channel_policy);
  RUN_TEST(test_follow_first_target_matrix);
  RUN_TEST(test_follow_advance_order_and_skips);
  RUN_TEST(test_follow_backoff_config);
  RUN_TEST(test_follow_creds_literal_contract);
  RUN_TEST(test_follow_creds_plaintext_golden);
  RUN_TEST(test_follow_store_validity);
  RUN_TEST(test_pairshow_literal_contract);
  RUN_TEST(test_pairshow_shared_codec_pack_unpack);
  RUN_TEST(test_pairshow_producer_bytes_to_renderer_exact);
  RUN_TEST(test_pairshow_duplicate_reack_and_stale_drop_cases);
  RUN_TEST(test_pairdisplay_pin_validation_and_state);
  RUN_TEST(test_pairdisplay_deadline_expiry);
  RUN_TEST(test_pairdisplay_pin_line_geometry);
  RUN_TEST(test_pairdisplay_countdown_format);
  RUN_TEST(test_pairdisplay_remaining_secs);
  RUN_TEST(test_pair_manager_second_gate);
  RUN_TEST(test_pairshow_pin_to_bitmap_equivalence);
  RUN_TEST(test_pairing_manager_bad_pin_noop);
  RUN_TEST(test_pairing_manager_show_rearms_deadline);
  RUN_TEST(test_pairing_manager_clear_when_idle_noop);
  RUN_TEST(test_pairing_manager_show_clear_roundtrip);
  RUN_TEST(test_pairing_manager_null_face_safe);
  RUN_TEST(test_ota_parse_plain_host);
  RUN_TEST(test_ota_parse_port_and_deep_path);
  RUN_TEST(test_ota_parse_bare_host_defaults_path);
  RUN_TEST(test_ota_parse_rejects);
  RUN_TEST(test_ota_version_orders);
  RUN_TEST(test_ota_magic);
  RUN_TEST(test_ota_fits_slot);
  RUN_TEST(test_ota_stream_gate);
  RUN_TEST(test_ota_manifest_matches);
  RUN_TEST(test_ota_bytes_to_hex);
  RUN_TEST(test_ota_state_happy_path);
  RUN_TEST(test_ota_state_verify_fail_and_abort);
  RUN_TEST(test_ota_state_rejects_bad_events);
  RUN_TEST(test_eyes_map_request_ids);
  RUN_TEST(test_eyes_map_ordinals_track_emotions);
  RUN_TEST(test_eyes_map_v2_rows);
  RUN_TEST(test_eyes_map_falls_back_to_normal);
  RUN_TEST(test_exprcat_ids_mirror_face_vocabulary);
  RUN_TEST(test_exprcat_count_and_order);
  RUN_TEST(test_exprcat_names);
  RUN_TEST(test_exprcat_priorities);
  RUN_TEST(test_exprcat_play_once_and_hold);
  RUN_TEST(test_exprcat_eye_commands);
  RUN_TEST(test_exprcat_placeholders_none);
  RUN_TEST(test_exprcat_v2_bundles);
  RUN_TEST(test_exprcat_lookup_fail_closed);
  RUN_TEST(test_exprcat_accessors_fail_closed);
  RUN_TEST(test_exprcat_matches_head_tables);
  RUN_TEST(test_exprcat_v2_eye_map);
  RUN_TEST(test_expr_priority_order);
  RUN_TEST(test_expr_play_once_shape);
  RUN_TEST(test_expr_paint_stores_every_id);
  RUN_TEST(test_expr_paint_unknown_clamps_boot);
  RUN_TEST(test_expr_paint_held_pairing_rejects_zero_change);
  RUN_TEST(test_expr_paint_held_ota_rejects_zero_change);
  RUN_TEST(test_expr_paint_clears_qr_only);
  RUN_TEST(test_expr_paint_applies_after_overlay_clears);
  RUN_TEST(test_expr_trigger_paths_cover_all_five);
  RUN_TEST(test_link_expr_literal_contract);
  RUN_TEST(test_link_expr_golden_round_trip);
  RUN_TEST(test_link_expr_unpack_fail_closed);
  RUN_TEST(test_link_expr_maps_to_face_gate);
  RUN_TEST(test_link_expr_unknown_maps_neutral);
  RUN_TEST(test_link_expr_v2_ids_route_to_paint);
  RUN_TEST(test_link_expr_gated_entry_routes_eyes);
  RUN_TEST(test_link_expr_pairing_screen_survives_command);
  RUN_TEST(test_link_expr_ota_screen_survives_command);
  RUN_TEST(test_link_expr_bench_path_intact);
  RUN_TEST(test_linkcrypto_provision_exact_length_only);
  RUN_TEST(test_linkcrypto_key_constants);
  RUN_TEST(test_linkcrypto_counter_nonce_layout);
  RUN_TEST(test_linkcrypto_gcm_zero_empty_vector);
  RUN_TEST(test_linkcrypto_gcm_zero_16_vector);
  RUN_TEST(test_linkcrypto_seal_deterministic_golden);
  RUN_TEST(test_linkcrypto_round_trip_sizes);
  RUN_TEST(test_linkcrypto_tamper_rejected);
  RUN_TEST(test_linkcrypto_wrong_key_and_nonce_rejected);
  RUN_TEST(test_linkcrypto_truncated_and_short_rejected);
  RUN_TEST(test_linkcrypto_nonce_reuse_is_visible);
  RUN_TEST(test_linkcrypto_null_and_cap_safety);
  RUN_TEST(test_linkcrypto_open_failure_zeroes_output);
  RUN_TEST(test_linkcrypto_passphrase_goldens);
  RUN_TEST(test_linkcrypto_passphrase_shape);
  return UNITY_END();
}
