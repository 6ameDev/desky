// Command LITERALS: literal wire bytes through decode -> mapCmd.
// Frames are literal [4C|type|seq|frag|len|payload|CRC16BE] from the encoder;
// any constant renumber breaks these by construction.

#include <unity.h>

#include "../src/services/power_manager.h"  // headapply:: (Arduino-free top half)
#include "../src/transport/link_codec.h"    // linkcode:: alias + wifilink::

namespace {

// Literal frames, seq 0x0001, frag 0/1 (from the real encoder).
const uint8_t kOled00[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x7D, 0x38};
const uint8_t kOled01[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x6D, 0x19};
const uint8_t kOled02[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x5D, 0x7A};
const uint8_t kOled03[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x03, 0x4D, 0x5B};
const uint8_t kCam00[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3A, 0xEB};
const uint8_t kCam01[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x2A, 0xCA};
const uint8_t kCam02[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x1A, 0xA9};

// Full path for one literal frame: decode must succeed, then map.
bool decodeAndMap(const uint8_t frame[11], PowerCommand& cmdOut, linkcode::Frame& fOut) {
  if (!linkcode::decodeFrame(frame, 11, fOut)) {
    return false;
  }
  return wifilink::mapCmd(fOut.type, fOut.payload, fOut.payloadLen, cmdOut);
}

}  // namespace

void test_cmd_literals_oled_wire_values() {
  PowerCommand cmd;
  linkcode::Frame f;
  TEST_ASSERT_TRUE(decodeAndMap(kOled00, cmd, f));
  TEST_ASSERT_EQUAL_UINT8(0x02, f.type);  // literal OLED type byte
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::OLED), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(2, cmd.arg);
  TEST_ASSERT_TRUE(decodeAndMap(kOled01, cmd, f));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(decodeAndMap(kOled02, cmd, f));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  // 0x03 is a live frame (CRC valid) but not a mappable value.
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kOled03, 11, f));
  TEST_ASSERT_FALSE(wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd));
}

void test_cmd_literals_camctl_wire_values() {
  PowerCommand cmd;
  linkcode::Frame f;
  TEST_ASSERT_TRUE(decodeAndMap(kCam00, cmd, f));
  TEST_ASSERT_EQUAL_UINT8(0x03, f.type);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::CAM), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  TEST_ASSERT_TRUE(decodeAndMap(kCam01, cmd, f));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kCam02, 11, f));
  TEST_ASSERT_FALSE(wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd));
}

void test_cmd_literals_tampered_frames_reject() {
  linkcode::Frame f;
  uint8_t buf[11];
  for (size_t i = 0; i < sizeof(buf); ++i) {
    buf[i] = kOled00[i];
  }
  TEST_ASSERT_TRUE(linkcode::decodeFrame(buf, sizeof(buf), f));
  buf[10] ^= 0x01;  // flip a CRC bit
  TEST_ASSERT_FALSE(linkcode::decodeFrame(buf, sizeof(buf), f));
  buf[10] ^= 0x01;
  buf[0] = 0xAA;  // phone-wire header: never a link frame
  TEST_ASSERT_FALSE(linkcode::decodeFrame(buf, sizeof(buf), f));
  TEST_ASSERT_FALSE(linkcode::decodeFrame(kCam02, 10, f));  // truncated
}

// Shared value table (pinned through the Arduino-free helper).
void test_headapply_value_table() {
  DisplayState oled = OLED_OFF;
  TEST_ASSERT_TRUE(headapply::resolveOled(0, oled));
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(oled));
  TEST_ASSERT_TRUE(headapply::resolveOled(1, oled));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(oled));
  TEST_ASSERT_TRUE(headapply::resolveOled(2, oled));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(oled));
  TEST_ASSERT_FALSE(headapply::resolveOled(3, oled));
  TEST_ASSERT_FALSE(headapply::resolveOled(-1, oled));
  TEST_ASSERT_FALSE(headapply::resolveOled(99, oled));
  TEST_ASSERT_EQUAL_UINT8(CAM_OFF, headapply::resolveCam(0));
  TEST_ASSERT_EQUAL_UINT8(CAM_ON, headapply::resolveCam(1));
  TEST_ASSERT_EQUAL_UINT8(CAM_ON, headapply::resolveCam(7));  // nonzero gate, not ==1
  TEST_ASSERT_EQUAL_UINT8(4, headapply::resolveFace(4));
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(5));
  TEST_ASSERT_EQUAL_UINT8(6, headapply::resolveFace(6));
  TEST_ASSERT_EQUAL_UINT8(7, headapply::resolveFace(7));
  TEST_ASSERT_EQUAL_UINT8(8, headapply::resolveFace(8));
  TEST_ASSERT_EQUAL_UINT8(9, headapply::resolveFace(9));
  TEST_ASSERT_EQUAL_UINT8(10, headapply::resolveFace(10));
  TEST_ASSERT_EQUAL_UINT8(11, headapply::resolveFace(11));
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(12));
}

// Shared codec vocabulary guards.
void test_cmd_literals_shared_codec_oled() {
  uint8_t wire = 0xFF;
  TEST_ASSERT_TRUE(linkcode::packOled(0x00, wire));
  TEST_ASSERT_EQUAL_UINT8(0x00, wire);
  TEST_ASSERT_TRUE(linkcode::packOled(0x01, wire));
  TEST_ASSERT_EQUAL_UINT8(0x01, wire);
  TEST_ASSERT_TRUE(linkcode::packOled(0x02, wire));
  TEST_ASSERT_EQUAL_UINT8(0x02, wire);
  TEST_ASSERT_FALSE(linkcode::packOled(0x03, wire));
  uint8_t mode = 0xFF;
  TEST_ASSERT_TRUE(linkcode::unpackOled(0x00, mode));
  TEST_ASSERT_EQUAL_UINT8(0x00, mode);
  TEST_ASSERT_TRUE(linkcode::unpackOled(0x01, mode));
  TEST_ASSERT_EQUAL_UINT8(0x01, mode);
  TEST_ASSERT_TRUE(linkcode::unpackOled(0x02, mode));
  TEST_ASSERT_EQUAL_UINT8(0x02, mode);
  TEST_ASSERT_FALSE(linkcode::unpackOled(0x03, mode));
}

void test_cmd_literals_shared_codec_camctl() {
  uint8_t wire = 0xFF;
  TEST_ASSERT_TRUE(linkcode::packCamCtl(0x00, wire));
  TEST_ASSERT_EQUAL_UINT8(0x00, wire);
  TEST_ASSERT_TRUE(linkcode::packCamCtl(0x01, wire));
  TEST_ASSERT_EQUAL_UINT8(0x01, wire);
  TEST_ASSERT_FALSE(linkcode::packCamCtl(0x02, wire));
  uint8_t mode = 0xFF;
  TEST_ASSERT_TRUE(linkcode::unpackCamCtl(0x00, mode));
  TEST_ASSERT_EQUAL_UINT8(0x00, mode);
  TEST_ASSERT_TRUE(linkcode::unpackCamCtl(0x01, mode));
  TEST_ASSERT_EQUAL_UINT8(0x01, mode);
  TEST_ASSERT_FALSE(linkcode::unpackCamCtl(0x02, mode));
}
