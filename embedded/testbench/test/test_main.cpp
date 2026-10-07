// Testbench host-test runner: single-program main listing every test.
// Run: pio test -e native-test

#include <unity.h>

void setUp() {}
void tearDown() {}

void test_mcu_identity_names_classic_esp32();
void test_i2c_bus_pins_match_v1_baseline();
void test_scan_cadence_is_positive();
void test_breathe_caps_at_quarter_power();

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mcu_identity_names_classic_esp32);
  RUN_TEST(test_i2c_bus_pins_match_v1_baseline);
  RUN_TEST(test_scan_cadence_is_positive);
  RUN_TEST(test_breathe_caps_at_quarter_power);
  return UNITY_END();
}
