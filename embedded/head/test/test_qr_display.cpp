// Host unit tests for the H2.5 QR show mode (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free qrdisplay geometry/payload contract in
// src/middleware/face_renderer.h (cross-pinned against the frozen link
// codec length), the FACE_QR id rules in head_context.h, and the `qr` /
// `qr clear` CLI bench verbs. The firmware paint path (drawQr) is
// compile-tested by the desky-head build, not here.

#include <unity.h>

#include "../src/middleware/face_renderer.h"
#include "../src/middleware/wifi_link.h"
#include "../src/services/cli_manager.h"

void test_qr_payload_len_pins_link_contract() {
  TEST_ASSERT_EQUAL_size_t(63, qrdisplay::kQrLen);
  TEST_ASSERT_EQUAL_size_t(linkcode::kQrBitmapLen, qrdisplay::kQrLen);
  TEST_ASSERT_TRUE(qrdisplay::isValidQrPayloadLen(63));
  TEST_ASSERT_TRUE(qrdisplay::isValidQrPayloadLen(linkcode::kQrBitmapLen));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(62));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(64));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(0));
}

void test_qr_geometry_fits_panel() {
  TEST_ASSERT_EQUAL_INT(21, qrdisplay::kModules);
  TEST_ASSERT_EQUAL_INT(2, qrdisplay::kScale);
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kQuietPx);    // spec 4 modules/side @2px
  TEST_ASSERT_EQUAL_INT(58, qrdisplay::kExtentPx);  // 21*2+16
  TEST_ASSERT_EQUAL_INT(35, qrdisplay::kX0);        // (128-58)/2
  TEST_ASSERT_EQUAL_INT(3, qrdisplay::kY0);         // (64-58)/2
  TEST_ASSERT_TRUE(qrdisplay::fitsPanel());
  TEST_ASSERT_TRUE(qrdisplay::kX0 + qrdisplay::kExtentPx <= qrdisplay::kPanelW);
  TEST_ASSERT_TRUE(qrdisplay::kY0 + qrdisplay::kExtentPx <= qrdisplay::kPanelH);
}

void test_qr_module_msb_first_row_major() {
  uint8_t qr[qrdisplay::kQrLen] = {};
  qr[0] = 0x80;  // row 0, module x=0 (byte 0, bit 7)
  TEST_ASSERT_EQUAL_UINT8(1, qrdisplay::moduleAt(qr, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 1, 0));
  qr[2] = 0x08;  // row 0, module x=20 (byte 2, bit 3)
  TEST_ASSERT_EQUAL_UINT8(1, qrdisplay::moduleAt(qr, 20, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 19, 0));
  qr[3] = 0x40;  // row 1, module x=1 (byte 3, bit 6)
  TEST_ASSERT_EQUAL_UINT8(1, qrdisplay::moduleAt(qr, 1, 1));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 1, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 1, 2));
}

void test_qr_module_fail_closed() {
  uint8_t qr[qrdisplay::kQrLen] = {};
  qr[0] = 0xFF;
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(nullptr, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, -1, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 0, -1));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 21, 0));
  TEST_ASSERT_EQUAL_UINT8(0, qrdisplay::moduleAt(qr, 0, 21));
}

void test_qr_face_id_appended_not_renumbered() {
  TEST_ASSERT_EQUAL_UINT8(5, FACE_QR);
  TEST_ASSERT_EQUAL_UINT8(6, FACE_COUNT);
  TEST_ASSERT_EQUAL_UINT8(0, FACE_BOOT);  // existing ids untouched
  TEST_ASSERT_EQUAL_UINT8(4, FACE_ALERT);
  TEST_ASSERT_EQUAL_STRING("qr", faceName(FACE_QR));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(FACE_QR));  // never via a FACE id
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName("qr"));     // no CLI face name
}

void test_cli_qr_and_qr_clear() {
  PowerCommand cmd;
  TEST_ASSERT_TRUE(headcmd::parseLine("qr", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::QR), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("  qr  ", cmd));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(headcmd::parseLine("qr clear", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::QR), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  TEST_ASSERT_FALSE(headcmd::parseLine("qr foo", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("qr clear now", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("QR", cmd));  // CLI dialect stays lowercase
  TEST_ASSERT_FALSE(headcmd::parseLine("qrx", cmd));
}
