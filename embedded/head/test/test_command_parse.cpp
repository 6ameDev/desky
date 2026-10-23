// Both command doors (no hardware): lowercase CLI, UPPERCASE link stub.

#include <unity.h>

#include "../src/services/cli_manager.h"
#include "../src/transport/link_stub.h"

void test_cli_cam_on_off() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("cam on", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::CAM), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("cam off", cmd));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
}

void test_cli_cam_rejects() {
  PowerCommand cmd;
  TEST_ASSERT_FALSE(headcmd::parseLine("cam", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("cam dim", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("cam on now", cmd));  // trailing garbage
  TEST_ASSERT_FALSE(headcmd::parseLine("CAM ON", cmd));
}

void test_cli_face_numeric_and_name() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("face 3", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(3, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("face happy", cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_HAPPY, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("face curious", cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_CURIOUS, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("face surprised", cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_SURPRISED, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("face 6", cmd));
  TEST_ASSERT_EQUAL_INT32(6, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("face 7", cmd));
  TEST_ASSERT_EQUAL_INT32(7, cmd.arg);
}

void test_cli_face_rejects() {
  PowerCommand cmd;
  TEST_ASSERT_FALSE(headcmd::parseLine("face", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("face scared", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("face -1", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("face 1 2", cmd));
}

void test_cli_oled_rows() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("oled on", cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_ON, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("oled dim", cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_DIM, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("oled off", cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_OFF, cmd.arg);
  TEST_ASSERT_FALSE(headcmd::parseLine("oled bright", cmd));
}

void test_cli_status_help_and_noise() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("status", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::STATUS), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(headcmd::parseLine("help", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::HELP), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(headcmd::parseLine("  cam on  ", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("   ", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine(nullptr, cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("reboot", cmd));
}

void test_link_face_numeric() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(linkstub::mapLine("FACE 2", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(2, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("FACE 6", cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_CURIOUS, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("FACE 7", cmd));
  TEST_ASSERT_EQUAL_INT32(FACE_SURPRISED, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("FACE 9", cmd));
  TEST_ASSERT_EQUAL_INT32(9, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("FACE 1\r\n", cmd));
}

void test_link_face_rejects() {
  PowerCommand cmd;
  TEST_ASSERT_FALSE(linkstub::mapLine("FACE happy", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("FACE", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("face 1", cmd));
}

void test_link_cam_oled_verbs() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(linkstub::mapLine("CAM ON", cmd));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("CAM OFF", cmd));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("OLED DIM", cmd));
  TEST_ASSERT_EQUAL_INT32(OLED_DIM, cmd.arg);
  TEST_ASSERT_TRUE(linkstub::mapLine("OLED OFF", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("CAM TOGGLE", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("OLED BRIGHT", cmd));
}

void test_link_noise_rejected() {
  PowerCommand cmd;
  TEST_ASSERT_FALSE(linkstub::mapLine("", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("AWAKE", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine("HB", cmd));
  TEST_ASSERT_FALSE(linkstub::mapLine(nullptr, cmd));
}
