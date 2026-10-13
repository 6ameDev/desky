// Testbench host-test runner: single-program main listing every test.
// Run: pio test -e native-test

#include <unity.h>

void setUp() {}
void tearDown() {}

void test_mcu_identity_names_classic_esp32();
void test_i2c_bus_pins_match_v1_baseline();
void test_scan_cadence_is_positive();
void test_breathe_caps_at_quarter_power();
void test_mic_unpack_top24_in_32();
void test_mic_slot_rms_reads_one_slot();
void test_mic_slot_rms_rejects_bad_input();
void test_spk_sine_shape_and_click_ramp();
void test_spk_pack_dual_mono32();
void test_demo_bar_mapping();
void test_demo_verdict_latches_on_peak();
void test_tcrt_tap_is_adc1_wifi_safe();
void test_tcrt_rail_assert_low_bands();
void test_tcrt_subtract_clamps_at_zero();
void test_tcrt_rail_assert_high_signal_space();

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mcu_identity_names_classic_esp32);
  RUN_TEST(test_i2c_bus_pins_match_v1_baseline);
  RUN_TEST(test_scan_cadence_is_positive);
  RUN_TEST(test_breathe_caps_at_quarter_power);
  RUN_TEST(test_mic_unpack_top24_in_32);
  RUN_TEST(test_mic_slot_rms_reads_one_slot);
  RUN_TEST(test_mic_slot_rms_rejects_bad_input);
  RUN_TEST(test_spk_sine_shape_and_click_ramp);
  RUN_TEST(test_spk_pack_dual_mono32);
  RUN_TEST(test_demo_bar_mapping);
  RUN_TEST(test_demo_verdict_latches_on_peak);
  RUN_TEST(test_tcrt_tap_is_adc1_wifi_safe);
  RUN_TEST(test_tcrt_rail_assert_low_bands);
  RUN_TEST(test_tcrt_subtract_clamps_at_zero);
  RUN_TEST(test_tcrt_rail_assert_high_signal_space);
  return UNITY_END();
}
