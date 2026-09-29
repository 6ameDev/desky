// Host unit tests for the head power matrix (no hardware).
// Run: pio test -e native-test

#include <unity.h>

#include "../src/services/power_manager.h"

void test_boot_validation_lean0_all_on() {
  CameraState cam = CAM_OFF;
  DisplayState oled = OLED_OFF;
  bootPowerState(0, cam, oled);
  TEST_ASSERT_EQUAL_UINT8(CAM_ON, cam);
  TEST_ASSERT_EQUAL_UINT8(OLED_ON, oled);
}

void test_boot_lean_nonzero_cam_off_oled_dim() {
  CameraState cam = CAM_ON;
  DisplayState oled = OLED_ON;
  bootPowerState(1, cam, oled);
  TEST_ASSERT_EQUAL_UINT8(CAM_OFF, cam);
  TEST_ASSERT_EQUAL_UINT8(OLED_DIM, oled);
}

void test_cam_matrix_flips_both_ways() {
  TEST_ASSERT_EQUAL_UINT8(CAM_ON, resolveCamCommand(CAM_OFF, true));
  TEST_ASSERT_EQUAL_UINT8(CAM_OFF, resolveCamCommand(CAM_ON, false));
  TEST_ASSERT_EQUAL_UINT8(CAM_ON, resolveCamCommand(CAM_ON, true));  // idempotent
  TEST_ASSERT_EQUAL_UINT8(CAM_OFF, resolveCamCommand(CAM_OFF, false));
}

void test_oled_matrix_all_rows() {
  TEST_ASSERT_EQUAL_UINT8(OLED_ON, resolveOledCommand(OLED_OFF, OLED_ON));
  TEST_ASSERT_EQUAL_UINT8(OLED_DIM, resolveOledCommand(OLED_ON, OLED_DIM));
  TEST_ASSERT_EQUAL_UINT8(OLED_OFF, resolveOledCommand(OLED_DIM, OLED_OFF));
  TEST_ASSERT_EQUAL_UINT8(OLED_DIM, resolveOledCommand(OLED_DIM, OLED_DIM));  // idempotent
}

void test_power_state_names() {
  TEST_ASSERT_EQUAL_STRING("on", camStateName(CAM_ON));
  TEST_ASSERT_EQUAL_STRING("off", camStateName(CAM_OFF));
  TEST_ASSERT_EQUAL_STRING("on", oledStateName(OLED_ON));
  TEST_ASSERT_EQUAL_STRING("dim", oledStateName(OLED_DIM));
  TEST_ASSERT_EQUAL_STRING("off", oledStateName(OLED_OFF));
}
