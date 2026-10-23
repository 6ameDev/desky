// QR show mode (no hardware): qrdisplay geometry/payload contract,
// FACE_QR id rules, `qr` / `qr clear` CLI verbs.

#include <string.h>
#include <unity.h>

#include "../../core/src/abilities/pairing/pairing_codec.h"
#include "../include/config.h"
#include "../src/abilities/face/face_codec.h"
#include "../src/services/cli_manager.h"
#include "../src/transport/link_codec.h"

// 63B "123456" packed golden (self-contained copy).
namespace {
const uint8_t kGolden123456[63] = {0xFE, 0x5B, 0xF8, 0x82, 0x72, 0x08, 0xBA, 0xDA, 0xE8, 0xBA, 0x52, 0xE8, 0xBA,
                                   0x2A, 0xE8, 0x82, 0x0A, 0x08, 0xFE, 0xAB, 0xF8, 0x00, 0xD8, 0x00, 0xEF, 0xF6,
                                   0x20, 0xA0, 0xC2, 0x08, 0x22, 0xA8, 0xC0, 0x7D, 0xE2, 0x48, 0xBA, 0x4A, 0x98,
                                   0x00, 0xB5, 0x50, 0xFE, 0xF7, 0x20, 0x82, 0xBD, 0xC0, 0xBA, 0xB7, 0x28, 0xBA,
                                   0x02, 0x30, 0xBA, 0xE8, 0x88, 0x82, 0xA2, 0x20, 0xFE, 0xCA, 0xA8};

bool headFinderOk(const uint8_t* qr63, int fx, int fy) {
  for (int dy = 0; dy < 7; ++dy) {
    for (int dx = 0; dx < 7; ++dx) {
      const bool edge = dx == 0 || dx == 6 || dy == 0 || dy == 6;
      const bool core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
      const uint8_t want = (edge || core) ? 1 : 0;
      if (qrdisplay::moduleAt(qr63, fx + dx, fy + dy) != want) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace

void test_qr_payload_len_pins_display_contract() {
  TEST_ASSERT_EQUAL_size_t(63, qrdisplay::kQrLen);
  TEST_ASSERT_TRUE(qrdisplay::isValidQrPayloadLen(63));
  TEST_ASSERT_TRUE(qrdisplay::isValidQrPayloadLen(qrdisplay::kQrLen));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(62));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(64));
  TEST_ASSERT_FALSE(qrdisplay::isValidQrPayloadLen(0));
}

void test_qr_geometry_fits_panel() {
  TEST_ASSERT_EQUAL_INT(21, qrdisplay::kModules);
  TEST_ASSERT_EQUAL_INT(2, qrdisplay::kScale);
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kQuietPx);
  TEST_ASSERT_EQUAL_INT(58, qrdisplay::kExtentPx);
  TEST_ASSERT_EQUAL_INT(4, qrdisplay::kX0);
  TEST_ASSERT_EQUAL_INT(3, qrdisplay::kY0);
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

void test_qr_e2e_encode_pack_display_identity() {
  // End-to-end identity: packed bytes equal the golden; moduleAt == getModule.
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t modules[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(code, modules));
  uint8_t packed[qr::kPackedLen];
  qr::packBitmap(modules, packed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kGolden123456, packed, sizeof(packed));
  for (int y = 0; y < qr::kSize; ++y) {
    for (int x = 0; x < qr::kSize; ++x) {
      TEST_ASSERT_EQUAL_UINT8(qr::getModule(modules, x, y), qrdisplay::moduleAt(packed, x, y));
    }
  }
}

void test_qr_bug_signature_first63_unpacked_differs() {
  // Negative fixture: 63 unpacked bytes as payload must not reproduce the finder.
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t modules[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(code, modules));
  uint8_t packed[qr::kPackedLen];
  qr::packBitmap(modules, packed);
  uint8_t bug[sizeof(packed)];
  for (size_t i = 0; i < sizeof(bug); ++i) {
    bug[i] = modules[i];  // first-63 unpacked bytes as if packed
  }

  TEST_ASSERT_FALSE(memcmp(bug, packed, sizeof(bug)) == 0);
  TEST_ASSERT_FALSE(memcmp(bug, kGolden123456, sizeof(bug)) == 0);

  TEST_ASSERT_TRUE(headFinderOk(packed, 0, 0));
  TEST_ASSERT_TRUE(headFinderOk(kGolden123456, 0, 0));
  TEST_ASSERT_FALSE(headFinderOk(bug, 0, 0));
}

void test_qr_face_id_appended_not_renumbered() {
  TEST_ASSERT_EQUAL_UINT8(5, FACE_QR);
  TEST_ASSERT_EQUAL_UINT8(6, FACE_COUNT);
  TEST_ASSERT_EQUAL_UINT8(0, FACE_BOOT);
  TEST_ASSERT_EQUAL_UINT8(4, FACE_ALERT);
  TEST_ASSERT_EQUAL_STRING("qr", faceName(FACE_QR));
  TEST_ASSERT_EQUAL_UINT8(FACE_BOOT, clampFace(FACE_QR));
  TEST_ASSERT_EQUAL_UINT8(0xFF, faceIdFromName("qr"));
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
  TEST_ASSERT_FALSE(headcmd::parseLine("QR", cmd));
  TEST_ASSERT_FALSE(headcmd::parseLine("qrx", cmd));
}

void test_qr_pairing_window_order_head() {
  // QR show (120s) clears well before the core pairing window (300s).
  TEST_ASSERT_EQUAL_UINT32(120000, static_cast<uint32_t>(CFG_FACE_QR_TIMEOUT_MS));
  TEST_ASSERT_EQUAL_UINT32(300000, pairing::kWindowMs);
  TEST_ASSERT_TRUE(static_cast<uint32_t>(CFG_FACE_QR_TIMEOUT_MS) < pairing::kWindowMs);
}
