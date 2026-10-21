// Host unit tests for the link endpoint helpers (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace linkserver in
// src/transport/link_server.h (seq verdicts incl. u16 wrap, TCP stream
// framing, type names). The LinkServer task (sockets/WDT) is firmware-only
// (#ifdef ARDUINO) and exercised on hardware. Runner lives in
// test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/transport/link_server.h"

void test_linkserver_first_frame_fresh() {
  TEST_ASSERT_TRUE(linkserver::classifySeq(false, 0, 0) == linkserver::SeqVerdict::kFresh);
  TEST_ASSERT_TRUE(linkserver::classifySeq(false, 1234, 1234) == linkserver::SeqVerdict::kFresh);
  TEST_ASSERT_TRUE(linkserver::classifySeq(false, 0xFFFF, 0) == linkserver::SeqVerdict::kFresh);
}

void test_linkserver_duplicate_stale_fresh() {
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 42, 42) == linkserver::SeqVerdict::kDuplicate);
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 42, 43) == linkserver::SeqVerdict::kFresh);
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 43, 42) == linkserver::SeqVerdict::kStale);
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 0xFFFF, 0) == linkserver::SeqVerdict::kFresh);
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 0, 0xFFFF) == linkserver::SeqVerdict::kStale);
  TEST_ASSERT_TRUE(linkserver::classifySeq(true, 100, static_cast<uint16_t>(100 + 0x8000)) ==
                   linkserver::SeqVerdict::kStale);
}

void test_linkserver_pull_framing() {
  size_t frameLen = 0;
  TEST_ASSERT_TRUE(linkserver::pullFrame(nullptr, 0, frameLen) == linkserver::Pull::kNeedMore);
  const uint8_t shortBuf[4] = {0x4C, 0x01, 0x00, 0x01};
  TEST_ASSERT_TRUE(linkserver::pullFrame(shortBuf, sizeof(shortBuf), frameLen) == linkserver::Pull::kNeedMore);
  uint8_t garbage[linkcode::kOverhead] = {};
  garbage[0] = 0xAA;  // Phone-wire header: never a frame start.
  TEST_ASSERT_TRUE(linkserver::pullFrame(garbage, sizeof(garbage), frameLen) == linkserver::Pull::kResync);
  // Golden FACE frame (11B): partial header -> NeedMore, full -> HaveFrame.
  const uint8_t golden[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  TEST_ASSERT_TRUE(linkserver::pullFrame(golden, 10, frameLen) == linkserver::Pull::kNeedMore);
  TEST_ASSERT_TRUE(linkserver::pullFrame(golden, sizeof(golden), frameLen) == linkserver::Pull::kHaveFrame);
  TEST_ASSERT_EQUAL_size_t(11, frameLen);
  // Insane length (claims 0xFFFF payload bytes) -> Resync, never a huge read.
  uint8_t insane[linkcode::kOverhead] = {0x4C, 0x10, 0x00, 0x01, 0x00, 0x01, 0xFF, 0xFF};
  TEST_ASSERT_TRUE(linkserver::pullFrame(insane, sizeof(insane), frameLen) == linkserver::Pull::kResync);
}

void test_linkserver_qr_gate_once_per_window_with_peer() {
  TEST_ASSERT_TRUE(linkserver::qrReadyToSend(true, false, true));
  TEST_ASSERT_FALSE(linkserver::qrReadyToSend(false, false, true));  // nothing staged
  TEST_ASSERT_FALSE(linkserver::qrReadyToSend(true, true, true));    // already sent this window
  TEST_ASSERT_FALSE(linkserver::qrReadyToSend(true, false, false));  // no head linked: wait
}

void test_linkserver_qr_pin_rule() {
  using V = linkserver::SeqVerdict;
  TEST_ASSERT_TRUE(linkserver::pinOnVerdict(V::kFresh));
  TEST_ASSERT_TRUE(linkserver::pinOnVerdict(V::kDuplicate));  // re-pin: head still here
  TEST_ASSERT_FALSE(linkserver::pinOnVerdict(V::kStale));     // replays never move the peer
}

void test_linkserver_qr_frame_63_round_trip() {
  uint8_t qr[linkcode::kQrBitmapLen];
  for (size_t i = 0; i < sizeof(qr); ++i) {
    qr[i] = static_cast<uint8_t>(i * 3u + 1u);
  }
  uint8_t out[linkcode::kOverhead + linkcode::kQrBitmapLen];
  const size_t n = linkcode::encodeFrame(linkcode::kTypeQrBitmap, 7, qr, sizeof(qr), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + linkcode::kQrBitmapLen, n);  // one 73B frame
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(out, n, f));
  TEST_ASSERT_EQUAL_UINT8(linkcode::kTypeQrBitmap, f.type);
  TEST_ASSERT_EQUAL_UINT16(7, f.seq);
  TEST_ASSERT_EQUAL_UINT16(linkcode::kQrBitmapLen, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(qr, f.payload, linkcode::kQrBitmapLen);
}

void test_linkserver_type_names() {
  TEST_ASSERT_EQUAL_STRING("FACE", linkserver::typeName(linkcode::kTypeFace));
  TEST_ASSERT_EQUAL_STRING("OLED", linkserver::typeName(linkcode::kTypeOled));
  TEST_ASSERT_EQUAL_STRING("CAMCTL", linkserver::typeName(linkcode::kTypeCamCtl));
  TEST_ASSERT_EQUAL_STRING("QRBITMAP", linkserver::typeName(linkcode::kTypeQrBitmap));
  TEST_ASSERT_EQUAL_STRING("JPEG", linkserver::typeName(linkcode::kTypeJpeg));
  TEST_ASSERT_EQUAL_STRING("HB", linkserver::typeName(linkcode::kTypeHb));
  TEST_ASSERT_EQUAL_STRING("HB_ACK", linkserver::typeName(linkcode::kTypeHbAck));
  TEST_ASSERT_EQUAL_STRING("ACK", linkserver::typeName(linkcode::kTypeAck));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", linkserver::typeName(0xFF));
}

void test_linkserver_boot_epoch_resync_gate() {
  // No epoch yet -> any bootId resyncs (first contact after either reboot).
  TEST_ASSERT_TRUE(linkserver::bootEpochChanged(false, 0, 0));
  TEST_ASSERT_TRUE(linkserver::bootEpochChanged(false, 41, 7));
  // Changed epoch -> resync even when the seq would otherwise be stale
  // (head rebooted with txSeq reset to 0 while we remember a high seq).
  TEST_ASSERT_TRUE(linkserver::bootEpochChanged(true, 0xA11CE001u, 0xB00DFACEu));
  TEST_ASSERT_TRUE(linkserver::bootEpochChanged(true, 60, 61));
  // Same epoch -> NOT a resync; the seq gate below decides (fresh/dup/stale).
  TEST_ASSERT_FALSE(linkserver::bootEpochChanged(true, 0xA11CE001u, 0xA11CE001u));
  TEST_ASSERT_FALSE(linkserver::bootEpochChanged(true, 0, 0));
}
