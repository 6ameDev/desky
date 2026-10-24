// Producer-side vocabulary test (S): literal wire bytes through the SHARED
// codec entity (link::packOled/packCamCtl/unpack*). The future core
// stageFace/stageOled/stageCam producers MUST pack through these helpers —
// never hand-roll the constants — so a renumber is a deliberate
// test-updating decision here first. Wire is frozen: OLED/CAMCTL
// 0x00/0x01(/0x02)->same, first out-of-range byte->false; FACE is
// map-then-clamp (every byte maps identical, range enforced downstream by the
// head's clampFace: 0x00..0x04 display ids, 0x05/0x63/0xFF fall back to BOOT
// — see the note on link::unpackFace).
// Run: pio test -e native-test (runner: test_udp_codec.cpp).

#include <unity.h>

#include "link/link_codec.h"

void test_link_cmd_vocab_oled_literal_round_trip() {
  uint8_t wire = 0xFF;
  TEST_ASSERT_TRUE(link::packOled(0x00, wire));
  TEST_ASSERT_EQUAL_UINT8(0x00, wire);
  TEST_ASSERT_TRUE(link::packOled(0x01, wire));
  TEST_ASSERT_EQUAL_UINT8(0x01, wire);
  TEST_ASSERT_TRUE(link::packOled(0x02, wire));
  TEST_ASSERT_EQUAL_UINT8(0x02, wire);
  TEST_ASSERT_FALSE(link::packOled(0x03, wire));
  uint8_t mode = 0xFF;
  TEST_ASSERT_TRUE(link::unpackOled(0x00, mode));
  TEST_ASSERT_EQUAL_UINT8(0x00, mode);
  TEST_ASSERT_TRUE(link::unpackOled(0x01, mode));
  TEST_ASSERT_EQUAL_UINT8(0x01, mode);
  TEST_ASSERT_TRUE(link::unpackOled(0x02, mode));
  TEST_ASSERT_EQUAL_UINT8(0x02, mode);
  TEST_ASSERT_FALSE(link::unpackOled(0x03, mode));
}

void test_link_cmd_vocab_camctl_literal_round_trip() {
  uint8_t wire = 0xFF;
  TEST_ASSERT_TRUE(link::packCamCtl(0x00, wire));
  TEST_ASSERT_EQUAL_UINT8(0x00, wire);
  TEST_ASSERT_TRUE(link::packCamCtl(0x01, wire));
  TEST_ASSERT_EQUAL_UINT8(0x01, wire);
  TEST_ASSERT_FALSE(link::packCamCtl(0x02, wire));
  uint8_t mode = 0xFF;
  TEST_ASSERT_TRUE(link::unpackCamCtl(0x00, mode));
  TEST_ASSERT_EQUAL_UINT8(0x00, mode);
  TEST_ASSERT_TRUE(link::unpackCamCtl(0x01, mode));
  TEST_ASSERT_EQUAL_UINT8(0x01, mode);
  TEST_ASSERT_FALSE(link::unpackCamCtl(0x02, mode));
}

void test_link_cmd_vocab_ota_literal_length() {
  // Carrier OTA vocabulary literal: type byte 0x05, payload 1..128B (the
  // wire contract the core producer stages and the head consumer pulls).
  TEST_ASSERT_EQUAL_UINT8(0x05, link::kTypeOta);
  TEST_ASSERT_TRUE(link::isKnownType(0x05));
  TEST_ASSERT_TRUE(link::isValidPayloadLen(0x05, 1));
  TEST_ASSERT_TRUE(link::isValidPayloadLen(0x05, 32));
  TEST_ASSERT_TRUE(link::isValidPayloadLen(0x05, 128));
  TEST_ASSERT_FALSE(link::isValidPayloadLen(0x05, 0));
  TEST_ASSERT_FALSE(link::isValidPayloadLen(0x05, 129));
}

void test_link_cmd_vocab_face_literal_range() {
  uint8_t id = 0xFF;
  for (uint8_t lit = 0x00; lit <= 0x04; ++lit) {
    TEST_ASSERT_TRUE(link::unpackFace(lit, id));
    TEST_ASSERT_EQUAL_UINT8(lit, id);
  }
  // Map-then-clamp: out-of-range bytes still map identical here; the head's
  // clampFace resolves them to BOOT downstream (never blank, never QR).
  TEST_ASSERT_TRUE(link::unpackFace(0x05, id));  // FACE_QR: never a FACE id
  TEST_ASSERT_EQUAL_UINT8(0x05, id);
  TEST_ASSERT_TRUE(link::unpackFace(0x63, id));
  TEST_ASSERT_EQUAL_UINT8(0x63, id);
  TEST_ASSERT_TRUE(link::unpackFace(0xFF, id));
  TEST_ASSERT_EQUAL_UINT8(0xFF, id);
}

void test_link_cmd_vocab_expr_literal_range() {
  // Producer side packs through packExpr: catalog ids ride raw, QR 5 and
  // anything outside the table fail closed — never hand-roll the byte.
  TEST_ASSERT_EQUAL_UINT8(0x08, link::kTypeExpr);
  const uint8_t good[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x07};
  for (size_t i = 0; i < sizeof(good); ++i) {
    uint8_t wire = 0xFF;
    TEST_ASSERT_TRUE(link::packExpr(good[i], wire));
    TEST_ASSERT_EQUAL_UINT8(good[i], wire);
    uint8_t id = 0xFF;
    TEST_ASSERT_TRUE(link::unpackExpr(good[i], id));
    TEST_ASSERT_EQUAL_UINT8(good[i], id);
  }
  const uint8_t bad[] = {0x05, 0x08, 0x63, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    uint8_t wire = 0xFF;
    TEST_ASSERT_FALSE(link::packExpr(bad[i], wire));
    uint8_t id = 0xFF;
    TEST_ASSERT_FALSE(link::unpackExpr(bad[i], id));
  }
}
