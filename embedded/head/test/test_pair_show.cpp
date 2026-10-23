// Authoritative-PIN pairing display (no hardware): PairShow 0x06 8B /
// PairDone 0x07 0B through the handleCmd seam, seq gate, deadline state,
// PIN-line geometry, PIN->bitmap equivalence. Goldens are literal encoder
// bytes (show seq 0x0043 "123456"+120s, done seq 0x0044 empty).

#include <string.h>
#include <unity.h>

#include "../src/abilities/face/face_codec.h"          // qrdisplay:: + pairdisplay::
#include "../src/abilities/pairing/pairing_codec.h"    // pairinglink:: gates
#include "../src/abilities/pairing/pairing_manager.h"  // PairingManager
#include "../src/transport/link_codec.h"               // linkcode:: alias + wifilink::

namespace {

// PAIRSHOW golden: seq 0x0043, "123456" + valid-seconds 120, CRC 0x810E.
const uint8_t kPairShowGolden[18] = {0x4C, 0x06, 0x00, 0x43, 0x00, 0x01, 0x00, 0x08, 0x31,
                                     0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x78, 0x81, 0x0E};
// PAIRDONE golden: seq 0x0044, empty, CRC 0x6043.
const uint8_t kPairDoneGolden[10] = {0x4C, 0x07, 0x00, 0x44, 0x00, 0x01, 0x00, 0x00, 0x60, 0x43};

// 63B "123456" packed golden (self-contained copy).
const uint8_t kGolden123456[63] = {0xFE, 0x5B, 0xF8, 0x82, 0x72, 0x08, 0xBA, 0xDA, 0xE8, 0xBA, 0x52, 0xE8, 0xBA,
                                   0x2A, 0xE8, 0x82, 0x0A, 0x08, 0xFE, 0xAB, 0xF8, 0x00, 0xD8, 0x00, 0xEF, 0xF6,
                                   0x20, 0xA0, 0xC2, 0x08, 0x22, 0xA8, 0xC0, 0x7D, 0xE2, 0x48, 0xBA, 0x4A, 0x98,
                                   0x00, 0xB5, 0x50, 0xFE, 0xF7, 0x20, 0x82, 0xBD, 0xC0, 0xBA, 0xB7, 0x28, 0xBA,
                                   0x02, 0x30, 0xBA, 0xE8, 0x88, 0x82, 0xA2, 0x20, 0xFE, 0xCA, 0xA8};

}  // namespace

void test_pairshow_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x06, linkcode::kTypePairShow);
  TEST_ASSERT_EQUAL_UINT8(0x07, linkcode::kTypePairDone);
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x06));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x07));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x08));
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", wifilink::typeName(0x06));
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", wifilink::typeName(0x07));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0x08));
  TEST_ASSERT_EQUAL_size_t(8, linkcode::kPairShowLen);
  TEST_ASSERT_EQUAL_size_t(0, linkcode::kPairDoneLen);

  TEST_ASSERT_TRUE(wifilink::isPairShowFrame(linkcode::kTypePairShow, linkcode::kPairShowLen));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypePairShow, 7));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypePairShow, 9));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypeFace, 8));
  TEST_ASSERT_TRUE(wifilink::isPairDoneFrame(linkcode::kTypePairDone, linkcode::kPairDoneLen));
  TEST_ASSERT_FALSE(wifilink::isPairDoneFrame(linkcode::kTypePairDone, 1));
  TEST_ASSERT_FALSE(wifilink::isPairDoneFrame(linkcode::kTypeFace, 0));

  PowerCommand cmd;
  const uint8_t one = 0;
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairShow, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairDone, &one, 1, cmd));
}

void test_pairshow_shared_codec_pack_unpack() {
  // Shared vocabulary: digits ride raw, valid-secs u16BE, fail-closed both ways.
  const uint8_t digits[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t packed[linkcode::kPairShowLen] = {};
  TEST_ASSERT_TRUE(linkcode::packPairShow(digits, 120, packed));
  const uint8_t want[8] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x78};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, packed, sizeof(want));
  uint8_t out[6] = {};
  uint16_t secs = 0;
  TEST_ASSERT_TRUE(linkcode::unpackPairShow(packed, sizeof(packed), out, secs));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, out, sizeof(digits));
  TEST_ASSERT_EQUAL_UINT16(120, secs);

  const uint8_t bad[6] = {'1', '2', '3', '4', '5', 'x'};
  TEST_ASSERT_FALSE(linkcode::packPairShow(bad, 120, packed));
  const uint8_t nul[6] = {'1', '2', 0x00, '4', '5', '6'};
  TEST_ASSERT_FALSE(linkcode::packPairShow(nul, 120, packed));
  TEST_ASSERT_FALSE(linkcode::packPairShow(digits, 120, nullptr));
  TEST_ASSERT_FALSE(linkcode::packPairShow(nullptr, 120, packed));
  TEST_ASSERT_FALSE(linkcode::unpackPairShow(packed, 7, out, secs));
  TEST_ASSERT_FALSE(linkcode::unpackPairShow(packed, 9, out, secs));
  TEST_ASSERT_FALSE(linkcode::unpackPairShow(nullptr, 8, out, secs));
  TEST_ASSERT_FALSE(linkcode::unpackPairShow(packed, 8, nullptr, secs));
  uint8_t badPayload[8] = {0x31, 0x32, 0x33, 0x34, 0x35, 0xFF, 0x00, 0x78};
  TEST_ASSERT_FALSE(linkcode::unpackPairShow(badPayload, sizeof(badPayload), out, secs));
}

void test_pairshow_producer_bytes_to_renderer_exact() {
  // Real-producer path: encode -> literal match -> decode -> shared unpack.
  const uint8_t digits[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t payload[linkcode::kPairShowLen] = {};
  TEST_ASSERT_TRUE(linkcode::packPairShow(digits, 120, payload));
  uint8_t produced[linkcode::kOverhead + linkcode::kPairShowLen] = {};
  const size_t n = linkcode::encodeFrame(linkcode::kTypePairShow, 0x0043, payload, sizeof(payload), 0, 1, produced,
                                         sizeof(produced));
  TEST_ASSERT_EQUAL_size_t(sizeof(kPairShowGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPairShowGolden, produced, sizeof(kPairShowGolden));
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kPairShowGolden, sizeof(kPairShowGolden), f));
  TEST_ASSERT_EQUAL_UINT8(0x06, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0043, f.seq);
  TEST_ASSERT_EQUAL_UINT16(8, f.payloadLen);
  TEST_ASSERT_TRUE(wifilink::isPairShowFrame(f.type, f.payloadLen));
  uint8_t pin[6] = {};
  uint16_t secs = 0;
  TEST_ASSERT_TRUE(linkcode::unpackPairShow(f.payload, f.payloadLen, pin, secs));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, pin, sizeof(digits));
  TEST_ASSERT_EQUAL_UINT16(120, secs);

  uint8_t done[linkcode::kOverhead] = {};
  TEST_ASSERT_EQUAL_size_t(
      sizeof(done), linkcode::encodeFrame(linkcode::kTypePairDone, 0x0044, nullptr, 0, 0, 1, done, sizeof(done)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPairDoneGolden, done, sizeof(kPairDoneGolden));
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kPairDoneGolden, sizeof(kPairDoneGolden), f));
  TEST_ASSERT_EQUAL_UINT8(0x07, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0044, f.seq);
  TEST_ASSERT_EQUAL_UINT16(0, f.payloadLen);
  TEST_ASSERT_TRUE(wifilink::isPairDoneFrame(f.type, f.payloadLen));

  uint8_t stuffed[sizeof(kPairDoneGolden) + 1];
  for (size_t i = 0; i < sizeof(kPairDoneGolden); ++i) {
    stuffed[i] = kPairDoneGolden[i];
  }
  stuffed[sizeof(stuffed) - 1] = 0x00;
  TEST_ASSERT_FALSE(linkcode::decodeFrame(stuffed, sizeof(stuffed), f));
}

void test_pairshow_duplicate_reack_and_stale_drop_cases() {
  // Duplicate re-ACKs without re-executing; stale drops (shared classifyRx gate).
  using V = wifilink::RxVerdict;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(false, 0, 0x43)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kDuplicate),
                          static_cast<uint8_t>(wifilink::classifyRx(true, 0x43, 0x43)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kStale),
                          static_cast<uint8_t>(wifilink::classifyRx(true, 0x43, 0x42)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh),
                          static_cast<uint8_t>(wifilink::classifyRx(true, 0x43, 0x44)));
}

void test_pairdisplay_pin_validation_and_state() {
  const uint8_t good[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t bad[6] = {'1', '2', '3', '4', '5', 'x'};
  TEST_ASSERT_TRUE(pairdisplay::isValidPin(good));
  TEST_ASSERT_FALSE(pairdisplay::isValidPin(bad));
  TEST_ASSERT_FALSE(pairdisplay::isValidPin(nullptr));
  pairdisplay::State s;
  TEST_ASSERT_FALSE(s.active);

  pairdisplay::show(s, good, 1000, 120);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(good, s.pin, sizeof(good));
  TEST_ASSERT_EQUAL_UINT32(121000, s.deadlineMs);
  const uint8_t other[6] = {'6', '5', '4', '3', '2', '1'};
  pairdisplay::show(s, other, 5000, 60);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(other, s.pin, sizeof(other));
  TEST_ASSERT_EQUAL_UINT32(65000, s.deadlineMs);

  pairdisplay::show(s, bad, 9000, 60);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(other, s.pin, sizeof(other));
  TEST_ASSERT_EQUAL_UINT32(65000, s.deadlineMs);
  pairdisplay::hide(s);
  TEST_ASSERT_FALSE(s.active);
}

void test_pairdisplay_deadline_expiry() {
  // Wrap-safe expiry, including across the u32 wrap.
  TEST_ASSERT_EQUAL_UINT32(121000, pairdisplay::deadlineFor(1000, 120));
  TEST_ASSERT_EQUAL_UINT32(1000, pairdisplay::deadlineFor(1000, 0));
  TEST_ASSERT_FALSE(pairdisplay::isExpired(120999, 121000));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(121000, 121000));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(121001, 121000));
  TEST_ASSERT_EQUAL_UINT32(1744, pairdisplay::deadlineFor(0xFFFFFF00u, 2) & 0xFFFFFFFFu);
  const uint32_t wrapDeadline = pairdisplay::deadlineFor(0xFFFFFF00u, 2);
  TEST_ASSERT_FALSE(pairdisplay::isExpired(0xFFFFFF00u, wrapDeadline));
  TEST_ASSERT_FALSE(pairdisplay::isExpired(0x00000600u, wrapDeadline));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(wrapDeadline, wrapDeadline));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(wrapDeadline + 100u, wrapDeadline));
}

void test_pairdisplay_pin_line_geometry() {
  // Side-by-side layout: QR block left, pair column (CODE/digits/countdown) right.
  TEST_ASSERT_EQUAL_INT(4, qrdisplay::kX0);
  TEST_ASSERT_EQUAL_INT(3, qrdisplay::kY0);
  TEST_ASSERT_EQUAL_INT(58, qrdisplay::kExtentPx);
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kQuietPx);
  TEST_ASSERT_TRUE(qrdisplay::fitsPanel());
  TEST_ASSERT_EQUAL_INT(66, qrdisplay::kPinX);
  TEST_ASSERT_EQUAL_INT(10, qrdisplay::kCodeY);
  TEST_ASSERT_EQUAL_INT(22, qrdisplay::kPinY);
  TEST_ASSERT_EQUAL_INT(38, qrdisplay::kCountdownY);
  TEST_ASSERT_EQUAL_INT(36, qrdisplay::kPinW);
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kPinH);
  TEST_ASSERT_EQUAL_INT(24, qrdisplay::kCodeW);
  TEST_ASSERT_TRUE(qrdisplay::fitsPinLine());
  TEST_ASSERT_TRUE(qrdisplay::fitsPairColumn());
  TEST_ASSERT_TRUE(qrdisplay::kPinX + qrdisplay::kPinW <= qrdisplay::kPanelW);
  TEST_ASSERT_TRUE(qrdisplay::kPinY + qrdisplay::kPinH <= qrdisplay::kPanelH);
  TEST_ASSERT_TRUE(qrdisplay::kPinX >= qrdisplay::kX0 + qrdisplay::kExtentPx);
}

void test_pairdisplay_countdown_format() {
  struct Row {
    uint32_t secs;
    const char* want;
  };
  const Row rows[] = {
      {300, "5m 00s"}, {299, "4m 59s"}, {249, "4m 09s"}, {61, "1m 01s"}, {60, "1m 00s"},
      {59, "0m 59s"},  {9, "0m 09s"},   {1, "0m 01s"},   {0, "0m 00s"},  {600, "10m 00s"},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
    char buf[pairdisplay::kCountdownBuf] = {};
    TEST_ASSERT_TRUE(pairdisplay::formatCountdown(rows[i].secs, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(rows[i].want, buf);
    char via[pairdisplay::kCountdownBuf] = {};
    TEST_ASSERT_TRUE(FaceOverlayManager::formatCountdown(rows[i].secs, via, sizeof(via)));
    TEST_ASSERT_EQUAL_STRING(rows[i].want, via);
  }
  char buf[pairdisplay::kCountdownBuf] = {};
  TEST_ASSERT_FALSE(pairdisplay::formatCountdown(9, nullptr, sizeof(buf)));
  TEST_ASSERT_FALSE(pairdisplay::formatCountdown(9, buf, 0));
  char tiny[4] = {};
  TEST_ASSERT_FALSE(pairdisplay::formatCountdown(9, tiny, sizeof(tiny)));
}

void test_pairdisplay_remaining_secs() {
  const uint32_t deadline = pairdisplay::deadlineFor(1000, 300);
  TEST_ASSERT_EQUAL_UINT32(300, pairdisplay::remainingSecs(1000, deadline));
  TEST_ASSERT_EQUAL_UINT32(300, pairdisplay::remainingSecs(1001, deadline));
  TEST_ASSERT_EQUAL_UINT32(300, pairdisplay::remainingSecs(1999, deadline));
  TEST_ASSERT_EQUAL_UINT32(299, pairdisplay::remainingSecs(2000, deadline));
  TEST_ASSERT_EQUAL_UINT32(1, pairdisplay::remainingSecs(deadline - 1000, deadline));
  TEST_ASSERT_EQUAL_UINT32(1, pairdisplay::remainingSecs(deadline - 1, deadline));
  TEST_ASSERT_EQUAL_UINT32(0, pairdisplay::remainingSecs(deadline, deadline));
  TEST_ASSERT_EQUAL_UINT32(0, pairdisplay::remainingSecs(deadline + 5000, deadline));
  const uint32_t wrapDeadline = pairdisplay::deadlineFor(0xFFFFFF00u, 2);
  TEST_ASSERT_EQUAL_UINT32(2, pairdisplay::remainingSecs(0xFFFFFF00u, wrapDeadline));
  TEST_ASSERT_EQUAL_UINT32(2, pairdisplay::remainingSecs(0xFFFFFF01u, wrapDeadline));
  TEST_ASSERT_EQUAL_UINT32(0, pairdisplay::remainingSecs(wrapDeadline, wrapDeadline));

  FaceOverlayManager face;
  FaceTransition t;
  const uint8_t pin[6] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_EQUAL_UINT32(0, face.remainingSecs(1000));
  TEST_ASSERT_TRUE(face.showPairing(pin, 300, 1000, t));
  TEST_ASSERT_EQUAL_UINT32(300, face.remainingSecs(1000));
  TEST_ASSERT_EQUAL_UINT32(299, face.remainingSecs(2000));
  TEST_ASSERT_EQUAL_UINT32(0, face.remainingSecs(301000));
  char text[pairdisplay::kCountdownBuf] = {};
  TEST_ASSERT_TRUE(face.formatCountdown(face.remainingSecs(52500), text, sizeof(text)));
  TEST_ASSERT_EQUAL_STRING("4m 09s", text);
}

void test_pair_manager_second_gate() {
  FaceOverlayManager face;
  FaceTransition t;
  const uint8_t pin[6] = {'1', '2', '3', '4', '5', '6'};
  uint32_t last = 0xFFFFFFFFu;
  TEST_ASSERT_FALSE(face.pairSecondTicked(1000, last));
  TEST_ASSERT_TRUE(face.showPairing(pin, 300, 1000, t));
  TEST_ASSERT_TRUE(face.pairSecondTicked(1000, last));
  TEST_ASSERT_EQUAL_UINT32(300, last);
  TEST_ASSERT_FALSE(face.pairSecondTicked(1000, last));
  TEST_ASSERT_FALSE(face.pairSecondTicked(1500, last));
  TEST_ASSERT_FALSE(face.pairSecondTicked(1999, last));
  TEST_ASSERT_TRUE(face.pairSecondTicked(2000, last));
  TEST_ASSERT_EQUAL_UINT32(299, last);
  TEST_ASSERT_TRUE(face.pairSecondTicked(301000, last));
  TEST_ASSERT_EQUAL_UINT32(0, last);
  TEST_ASSERT_FALSE(face.pairSecondTicked(302000, last));
  TEST_ASSERT_TRUE(face.clearPairing(t));
  TEST_ASSERT_FALSE(face.pairSecondTicked(303000, last));
  TEST_ASSERT_EQUAL_UINT32(0, face.remainingSecs(303000));
}

void test_pairshow_pin_to_bitmap_equivalence() {
  const uint8_t code[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t modules[qr::kModules];
  TEST_ASSERT_TRUE(qr::encode6Digits(code, modules));
  uint8_t packed[qr::kPackedLen];
  qr::packBitmap(modules, packed);
  TEST_ASSERT_EQUAL_size_t(qrdisplay::kQrLen, qr::kPackedLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kGolden123456, packed, sizeof(packed));
  for (int y = 0; y < qr::kSize; ++y) {
    for (int x = 0; x < qr::kSize; ++x) {
      TEST_ASSERT_EQUAL_UINT8(qr::getModule(modules, x, y), qrdisplay::moduleAt(packed, x, y));
    }
  }

  const uint8_t bad[6] = {'1', '2', '3', '4', '5', 'x'};
  TEST_ASSERT_FALSE(qr::encode6Digits(bad, modules));
}

void test_pairing_manager_bad_pin_noop() {
  // Fail-closed: a null/bad PIN changes no overlay state.
  FaceOverlayManager face;
  PairingManager mgr;
  mgr.attach(&face);
  const uint8_t bad[6] = {'1', '2', '3', '4', '5', 'x'};
  TEST_ASSERT_FALSE(mgr.show(bad, 120, 1000));
  TEST_ASSERT_FALSE(mgr.active());
  TEST_ASSERT_FALSE(face.pairActive());
  TEST_ASSERT_FALSE(mgr.show(nullptr, 120, 1000));
  TEST_ASSERT_FALSE(mgr.active());
  TEST_ASSERT_FALSE(face.pairActive());
}

void test_pairing_manager_show_rearms_deadline() {
  FaceOverlayManager face;
  PairingManager mgr;
  mgr.attach(&face);
  const uint8_t pin[6] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_TRUE(mgr.show(pin, 120, 1000));
  TEST_ASSERT_TRUE(mgr.active());
  TEST_ASSERT_FALSE(mgr.expired(120999));

  const uint8_t other[6] = {'6', '5', '4', '3', '2', '1'};
  TEST_ASSERT_TRUE(mgr.show(other, 60, 5000));
  TEST_ASSERT_TRUE(mgr.active());
  TEST_ASSERT_FALSE(mgr.expired(64999));
  TEST_ASSERT_TRUE(mgr.expired(65000));
  TEST_ASSERT_TRUE(face.pairExpired(65000));
}

void test_pairing_manager_clear_when_idle_noop() {
  // Clear with nothing up is a no-op.
  FaceOverlayManager face;
  PairingManager mgr;
  mgr.attach(&face);
  TEST_ASSERT_FALSE(mgr.clear());
  TEST_ASSERT_FALSE(mgr.active());
}

void test_pairing_manager_show_clear_roundtrip() {
  FaceOverlayManager face;
  PairingManager mgr;
  mgr.attach(&face);
  const uint8_t pin[6] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_TRUE(mgr.show(pin, 10, 1000));
  TEST_ASSERT_TRUE(mgr.active());
  TEST_ASSERT_FALSE(mgr.expired(10999));
  TEST_ASSERT_TRUE(mgr.expired(11000));
  TEST_ASSERT_TRUE(mgr.clear());
  TEST_ASSERT_FALSE(mgr.active());
  TEST_ASSERT_FALSE(face.pairActive());
  TEST_ASSERT_FALSE(mgr.expired(11000));
  TEST_ASSERT_FALSE(mgr.expired(999999));
}

void test_pairing_manager_null_face_safe() {
  PairingManager mgr;
  const uint8_t pin[6] = {'1', '2', '3', '4', '5', '6'};
  TEST_ASSERT_FALSE(mgr.show(pin, 120, 1000));
  TEST_ASSERT_FALSE(mgr.clear());
  TEST_ASSERT_FALSE(mgr.active());
  TEST_ASSERT_FALSE(mgr.expired(999999));
  mgr.attach(nullptr);
  TEST_ASSERT_FALSE(mgr.show(pin, 120, 1000));
  TEST_ASSERT_FALSE(mgr.clear());
  TEST_ASSERT_FALSE(mgr.active());
  TEST_ASSERT_FALSE(mgr.expired(999999));
}
