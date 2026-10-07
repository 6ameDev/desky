// Host unit tests for the testbench baseline config (no hardware).
// These pin the rig to the classic ESP32 DevKit v1 map so a bad MCU-header
// edit fails on the laptop, not on the bench.
// Run: pio test -e native-test

#include <unity.h>

#include "../include/config.h"

void test_mcu_identity_names_classic_esp32() {
  TEST_ASSERT_EQUAL_STRING("ESP32-DevKit-v1-30pin", MCU_NAME);
  TEST_ASSERT_EQUAL_INT(2, MCU_NUM_CORES);
  TEST_ASSERT_EQUAL_INT(0, MCU_HAS_PSRAM);
}

void test_i2c_bus_pins_match_v1_baseline() {
  TEST_ASSERT_EQUAL_INT(21, CFG_I2C_SDA);
  TEST_ASSERT_EQUAL_INT(22, CFG_I2C_SCL);
  TEST_ASSERT_EQUAL_INT(400000, CFG_I2C_FREQ_HZ);
}

void test_scan_cadence_is_positive() {
  TEST_ASSERT_GREATER_THAN_INT(0, CFG_LED_STEP_MS);
  TEST_ASSERT_GREATER_THAN_INT(0, CFG_SCAN_EVERY_N_BREATHS);
}

void test_breathe_caps_at_quarter_power() {
  TEST_ASSERT_EQUAL_INT(8, CFG_LED_RES_BITS);
  TEST_ASSERT_EQUAL_INT(63, CFG_LED_MAX_DUTY);
  TEST_ASSERT_LESS_THAN_INT(1 << CFG_LED_RES_BITS, CFG_LED_MAX_DUTY + 1);
}
