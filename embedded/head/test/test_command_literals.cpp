// Command LITERALS test (S): literal wire bytes through the REAL consumer
// seam (linkcode::decodeFrame -> wifilink::mapCmd) plus the CLI door
// (headcmd::parseLine) and the shared value table (headapply::).
//
// Every frame below is a literal 11B link frame [4C|type|seq0001|frag00/01|
// len0001|payload|CRC16BE] printed by the real encoder and pasted verbatim —
// never symbolic constants. A constant renumber on either side (wire type,
// kOledOn/Dim/Off, DisplayState, FaceId) breaks this test by construction:
//   OLED  {0x00}->arg 2 (ON), {0x01}->arg 1 (DIM), {0x02}->arg 0 (OFF),
//         {0x03}->mapCmd false.
//   CAMCTL {0x00}->0, {0x01}->1, {0x02}->mapCmd false.
//   FACE  {0x00..0x04}->same id, {0x05,0x63,0xFF}->mapCmd true then clamp->0.
//   CLI   "oled on" identical to wire {0x00}, "oled off" to wire {0x02}.
// Run: pio test -e native-test (runner: test_main.cpp).

#include <unity.h>

#include "../src/middleware/wifi_link.h"  // linkcode:: alias + wifilink:: (canonical
// linkcode:: spelling is pinned by core's test_link_codec.cpp goldens; head TUs
// must use the alias — see the POSIX ::link note in wifi_link.h)
#include "../src/services/cli_manager.h"
#include "../src/services/power_manager.h"  // headapply:: (Arduino-free top half)

namespace {

// Literal frames, seq 0x0001, frag 0/1 (from the real encoder).
const uint8_t kOled00[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x7D, 0x38};
const uint8_t kOled01[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x6D, 0x19};
const uint8_t kOled02[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x5D, 0x7A};
const uint8_t kOled03[11] = {0x4C, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x03, 0x4D, 0x5B};
const uint8_t kCam00[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3A, 0xEB};
const uint8_t kCam01[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x2A, 0xCA};
const uint8_t kCam02[11] = {0x4C, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x1A, 0xA9};
const uint8_t kFace00[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0xB5, 0x4D};
const uint8_t kFace01[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0xA5, 0x6C};
const uint8_t kFace02[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
const uint8_t kFace03[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x03, 0x85, 0x2E};
const uint8_t kFace04[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x04, 0xF5, 0xC9};
const uint8_t kFace05[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x05, 0xE5, 0xE8};
const uint8_t kFace63[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x63, 0xE9, 0x88};
const uint8_t kFaceFf[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0xFF, 0xAB, 0xBD};

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
  TEST_ASSERT_EQUAL_INT32(2, cmd.arg);  // literal: wire 0x00 == DisplayState ON (2)
  TEST_ASSERT_TRUE(decodeAndMap(kOled01, cmd, f));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);  // literal: wire 0x01 == DIM (1)
  TEST_ASSERT_TRUE(decodeAndMap(kOled02, cmd, f));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);  // literal: wire 0x02 == OFF (0)
  // 0x03 is a live frame (CRC valid) but not a mappable value.
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kOled03, 11, f));
  TEST_ASSERT_FALSE(wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd));
}

void test_cmd_literals_camctl_wire_values() {
  PowerCommand cmd;
  linkcode::Frame f;
  TEST_ASSERT_TRUE(decodeAndMap(kCam00, cmd, f));
  TEST_ASSERT_EQUAL_UINT8(0x03, f.type);  // literal CAMCTL type byte
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::CAM), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_EQUAL_INT32(0, cmd.arg);
  TEST_ASSERT_TRUE(decodeAndMap(kCam01, cmd, f));
  TEST_ASSERT_EQUAL_INT32(1, cmd.arg);
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kCam02, 11, f));
  TEST_ASSERT_FALSE(wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd));
}

void test_cmd_literals_face_ids_and_clamp() {
  PowerCommand cmd;
  linkcode::Frame f;
  const uint8_t* frames[5] = {kFace00, kFace01, kFace02, kFace03, kFace04};
  for (int32_t id = 0; id < 5; ++id) {
    TEST_ASSERT_TRUE(decodeAndMap(frames[id], cmd, f));
    TEST_ASSERT_EQUAL_UINT8(0x01, f.type);  // literal FACE type byte
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
    TEST_ASSERT_EQUAL_INT32(id, cmd.arg);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(id), headapply::resolveFace(cmd.arg));  // passthrough
  }
  // Out-of-range ids still decode+map (passthrough), then clamp to BOOT (0).
  const uint8_t* bad[3] = {kFace05, kFace63, kFaceFf};
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(decodeAndMap(bad[i], cmd, f));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CmdTarget::FACE), static_cast<uint8_t>(cmd.target));
    TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(cmd.arg));
  }
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(-1));
}

void test_cmd_literals_cli_equivalence() {
  PowerCommand cli;
  PowerCommand wire;
  linkcode::Frame f;
  // "oled on" (human door) is the same command as wire {0x00} (link door).
  TEST_ASSERT_TRUE(headcmd::parseLine("oled on", cli));
  TEST_ASSERT_TRUE(decodeAndMap(kOled00, wire, f));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cli.target), static_cast<uint8_t>(wire.target));
  TEST_ASSERT_EQUAL_INT32(2, cli.arg);  // literal DisplayState ON
  TEST_ASSERT_EQUAL_INT32(cli.arg, wire.arg);
  // "oled off" (human door) is the same command as wire {0x02}.
  TEST_ASSERT_TRUE(headcmd::parseLine("oled off", cli));
  TEST_ASSERT_TRUE(decodeAndMap(kOled02, wire, f));
  TEST_ASSERT_EQUAL_INT32(0, cli.arg);  // literal DisplayState OFF
  TEST_ASSERT_EQUAL_INT32(cli.arg, wire.arg);
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
  TEST_ASSERT_FALSE(linkcode::decodeFrame(kFace02, 10, f));  // truncated
}

// The shared value table apply() runs on (firmware-guarded class, so pinned
// here through the Arduino-free helper — minimal diff, behavior-identical).
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
}

// Shared-entity guards: the literal wire bytes above must validate through
// the shared codec vocabulary (linkcode::pack/unpack), not just the consumer
// seam. Literals in, validated modes out, 0x03-style rejects — a renumber on
// either side breaks these by construction, same as the consumer asserts.
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

void test_cmd_literals_shared_codec_face() {
  // Map-then-clamp contract: every wire byte maps (identity passthrough);
  // the valid display ids 0x00..0x04 survive, 0x05 (FACE_QR: never a FACE
  // id), 0x63 and 0xFF fall back to BOOT downstream via clampFace.
  uint8_t id = 0xFF;
  for (uint8_t lit = 0x00; lit <= 0x04; ++lit) {
    TEST_ASSERT_TRUE(linkcode::unpackFace(lit, id));
    TEST_ASSERT_EQUAL_UINT8(lit, id);
  }
  TEST_ASSERT_TRUE(linkcode::unpackFace(0x05, id));
  TEST_ASSERT_EQUAL_UINT8(0x05, id);
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(id));
  TEST_ASSERT_TRUE(linkcode::unpackFace(0x63, id));
  TEST_ASSERT_EQUAL_UINT8(0x63, id);
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(id));
  TEST_ASSERT_TRUE(linkcode::unpackFace(0xFF, id));
  TEST_ASSERT_EQUAL_UINT8(0xFF, id);
  TEST_ASSERT_EQUAL_UINT8(0, headapply::resolveFace(id));
}
