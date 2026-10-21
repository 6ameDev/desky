// Host unit tests for the Pre-2 authoritative-PIN pairing display (no hardware).
// Run: pio test -e native-test
//
// Covers the head side of kTypePairShow=0x06 / kTypePairDone=0x07: real
// producer frame bytes (linkcode::encodeFrame) through the exact handleCmd
// seam (linkcode::unpackPairShow — the function handleCmd calls), the shared
// seq gate framed as duplicate-re-ACK + stale-drop cases (the verdicts ride
// the same classifyRx gate handleCmd applies; the firmware handleCmd call
// itself is compile-covered by the desky-head build), the pairdisplay::
// deadline/state machine, the side-by-side PIN-line geometry, and the
// PIN->bitmap equivalence against the 123456 packed golden.
//
// The 18B/10B pair goldens below are literal frames printed by the real
// encoder and pasted verbatim (seq 0x0043 show "123456"+120s, seq 0x0044
// empty done) — a constant renumber or contract change breaks these by
// construction. The 63B packed golden is duplicated from
// test_qr_display.cpp (itself duplicated from the canonical core vector in
// embedded/core/test/test_qr_pairing.cpp); the core test is canonical on any
// drift.

#include <string.h>
#include <unity.h>

#include "../src/abilities/face/face_codec.h"        // qrdisplay:: + pairdisplay:: (Arduino-free,
                                                     // incl. the shared qr:: encoder through the head TU)
#include "../src/abilities/pairing/pairing_codec.h"  // pairinglink:: gates (+ wifilink:: spelling)
#include "../src/transport/link_codec.h"             // linkcode:: alias + wifilink:: (canonical
// linkcode:: spelling is pinned by core's test_link_codec.cpp goldens; head TUs
// must use the alias — see the POSIX ::link note in link_codec.h)

namespace {

// Literal PAIRSHOW frame, seq 0x0043, frag 0/1, 8B payload ("123456" +
// valid-seconds 120 = 0x0078), CRC 0x810E (from the real encoder).
const uint8_t kPairShowGolden[18] = {0x4C, 0x06, 0x00, 0x43, 0x00, 0x01, 0x00, 0x08, 0x31,
                                     0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x78, 0x81, 0x0E};
// Literal PAIRDONE frame, seq 0x0044, frag 0/1, empty payload, CRC 0x6043.
const uint8_t kPairDoneGolden[10] = {0x4C, 0x07, 0x00, 0x44, 0x00, 0x01, 0x00, 0x00, 0x60, 0x43};

// 63B "123456" packed golden, duplicated from test_qr_display.cpp so the
// PIN->bitmap equivalence below stays self-contained in this TU.
const uint8_t kGolden123456[63] = {0xFE, 0x5B, 0xF8, 0x82, 0x72, 0x08, 0xBA, 0xDA, 0xE8, 0xBA, 0x52, 0xE8, 0xBA,
                                   0x2A, 0xE8, 0x82, 0x0A, 0x08, 0xFE, 0xAB, 0xF8, 0x00, 0xD8, 0x00, 0xEF, 0xF6,
                                   0x20, 0xA0, 0xC2, 0x08, 0x22, 0xA8, 0xC0, 0x7D, 0xE2, 0x48, 0xBA, 0x4A, 0x98,
                                   0x00, 0xB5, 0x50, 0xFE, 0xF7, 0x20, 0x82, 0xBD, 0xC0, 0xBA, 0xB7, 0x28, 0xBA,
                                   0x02, 0x30, 0xBA, 0xE8, 0x88, 0x82, 0xA2, 0x20, 0xFE, 0xCA, 0xA8};

}  // namespace

void test_pairshow_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x06, linkcode::kTypePairShow);  // literal PAIRSHOW type byte
  TEST_ASSERT_EQUAL_UINT8(0x07, linkcode::kTypePairDone);  // literal PAIRDONE type byte
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x06));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x07));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x08));  // next free type after the bump
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", wifilink::typeName(0x06));
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", wifilink::typeName(0x07));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0x08));
  TEST_ASSERT_EQUAL_size_t(8, linkcode::kPairShowLen);
  TEST_ASSERT_EQUAL_size_t(0, linkcode::kPairDoneLen);
  // Frame gates: exact lengths only (decodeFrame already enforces these; the
  // helpers mirror the handleCmd branch conditions).
  TEST_ASSERT_TRUE(wifilink::isPairShowFrame(linkcode::kTypePairShow, linkcode::kPairShowLen));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypePairShow, 7));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypePairShow, 9));
  TEST_ASSERT_FALSE(wifilink::isPairShowFrame(linkcode::kTypeFace, 8));
  TEST_ASSERT_TRUE(wifilink::isPairDoneFrame(linkcode::kTypePairDone, linkcode::kPairDoneLen));
  TEST_ASSERT_FALSE(wifilink::isPairDoneFrame(linkcode::kTypePairDone, 1));
  TEST_ASSERT_FALSE(wifilink::isPairDoneFrame(linkcode::kTypeFace, 0));
  // Pair frames never map through the FACE/OLED/CAMCTL command table.
  PowerCommand cmd;
  const uint8_t one = 0;
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairShow, &one, 1, cmd));
  TEST_ASSERT_FALSE(wifilink::mapCmd(linkcode::kTypePairDone, &one, 1, cmd));
}

void test_pairshow_shared_codec_pack_unpack() {
  // Shared vocabulary (both sides call these — never mirrored): digits ride
  // raw, valid-secs rides u16BE, non-digits reject fail-closed both ways.
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
  // Rejects: non-digit in, wrong length, nulls.
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
  // Real-producer path, exactly as handleCmd runs it: encode -> literal
  // match -> decode -> shared unpack hands the renderer digits + window.
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
  // PAIRDONE: literal match, empty decodes, gate passes (presence = signal).
  uint8_t done[linkcode::kOverhead] = {};
  TEST_ASSERT_EQUAL_size_t(
      sizeof(done), linkcode::encodeFrame(linkcode::kTypePairDone, 0x0044, nullptr, 0, 0, 1, done, sizeof(done)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPairDoneGolden, done, sizeof(kPairDoneGolden));
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kPairDoneGolden, sizeof(kPairDoneGolden), f));
  TEST_ASSERT_EQUAL_UINT8(0x07, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0044, f.seq);
  TEST_ASSERT_EQUAL_UINT16(0, f.payloadLen);
  TEST_ASSERT_TRUE(wifilink::isPairDoneFrame(f.type, f.payloadLen));
  // A stowaway byte on the empty contract fails the decode gate.
  uint8_t stuffed[sizeof(kPairDoneGolden) + 1];
  for (size_t i = 0; i < sizeof(kPairDoneGolden); ++i) {
    stuffed[i] = kPairDoneGolden[i];
  }
  stuffed[sizeof(stuffed) - 1] = 0x00;
  TEST_ASSERT_FALSE(linkcode::decodeFrame(stuffed, sizeof(stuffed), f));
}

void test_pairshow_duplicate_reack_and_stale_drop_cases() {
  // Duplicate seq on the shared cmd domain re-ACKs without re-executing;
  // stale drops. Framed here as pair seqs (the verdicts ride the same
  // classifyRx gate handleCmd applies to every command type).
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
  // Show arms (deadline = show-time + valid*1000) and re-arms on new show.
  pairdisplay::show(s, good, 1000, 120);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(good, s.pin, sizeof(good));
  TEST_ASSERT_EQUAL_UINT32(121000, s.deadlineMs);
  const uint8_t other[6] = {'6', '5', '4', '3', '2', '1'};
  pairdisplay::show(s, other, 5000, 60);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(other, s.pin, sizeof(other));
  TEST_ASSERT_EQUAL_UINT32(65000, s.deadlineMs);
  // Bad PIN changes no state (fail-closed).
  pairdisplay::show(s, bad, 9000, 60);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(other, s.pin, sizeof(other));
  TEST_ASSERT_EQUAL_UINT32(65000, s.deadlineMs);
  pairdisplay::hide(s);
  TEST_ASSERT_FALSE(s.active);
}

void test_pairdisplay_deadline_expiry() {
  // Wrap-safe expiry: false before the deadline, true at/after it —
  // including across the u32 wrap.
  TEST_ASSERT_EQUAL_UINT32(121000, pairdisplay::deadlineFor(1000, 120));
  TEST_ASSERT_EQUAL_UINT32(1000, pairdisplay::deadlineFor(1000, 0));  // 0 = already-expired
  TEST_ASSERT_FALSE(pairdisplay::isExpired(120999, 121000));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(121000, 121000));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(121001, 121000));
  TEST_ASSERT_EQUAL_UINT32(1744, pairdisplay::deadlineFor(0xFFFFFF00u, 2) & 0xFFFFFFFFu);
  const uint32_t wrapDeadline = pairdisplay::deadlineFor(0xFFFFFF00u, 2);  // 0x6D0, past the u32 wrap
  TEST_ASSERT_FALSE(pairdisplay::isExpired(0xFFFFFF00u, wrapDeadline));    // show-time itself: not expired
  TEST_ASSERT_FALSE(pairdisplay::isExpired(0x00000600u, wrapDeadline));    // 0x600 < 0x6D0: not yet
  TEST_ASSERT_TRUE(pairdisplay::isExpired(wrapDeadline, wrapDeadline));
  TEST_ASSERT_TRUE(pairdisplay::isExpired(wrapDeadline + 100u, wrapDeadline));
}

void test_pairdisplay_pin_line_geometry() {
  // Side-by-side layout: the 58px QR block moved left, PIN digits right of
  // it in the default font. Quiet zone untouched (still scale-2 21 modules +
  // 8px quiet); every rect fits the 128x64 panel.
  TEST_ASSERT_EQUAL_INT(3, qrdisplay::kX0);  // left block (was 35 centered)
  TEST_ASSERT_EQUAL_INT(3, qrdisplay::kY0);
  TEST_ASSERT_EQUAL_INT(58, qrdisplay::kExtentPx);  // 21*2+16
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kQuietPx);    // spec 4 modules/side @2px
  TEST_ASSERT_TRUE(qrdisplay::fitsPanel());
  TEST_ASSERT_EQUAL_INT(66, qrdisplay::kPinX);
  TEST_ASSERT_EQUAL_INT(28, qrdisplay::kPinY);
  TEST_ASSERT_EQUAL_INT(36, qrdisplay::kPinW);  // 6 cols * 6px/char
  TEST_ASSERT_EQUAL_INT(8, qrdisplay::kPinH);   // one 8px text line
  TEST_ASSERT_TRUE(qrdisplay::fitsPinLine());
  TEST_ASSERT_TRUE(qrdisplay::kPinX + qrdisplay::kPinW <= qrdisplay::kPanelW);
  TEST_ASSERT_TRUE(qrdisplay::kPinY + qrdisplay::kPinH <= qrdisplay::kPanelH);
  TEST_ASSERT_TRUE(qrdisplay::kPinX >= qrdisplay::kX0 + qrdisplay::kExtentPx);  // no overlap
}

void test_pairshow_pin_to_bitmap_equivalence() {
  // Authoritative-PIN identity: the locally re-derived bitmap for "123456"
  // equals the existing packed golden — this keeps the bitmap suite
  // meaningful now that no bitmap rides the wire (rendered ONCE at show).
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
  // Non-digit PINs never encode (the show path fails closed before paint).
  const uint8_t bad[6] = {'1', '2', '3', '4', '5', 'x'};
  TEST_ASSERT_FALSE(qr::encode6Digits(bad, modules));
}
