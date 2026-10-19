// Host unit tests for the carrier OTA consumer (no hardware).
// Run: pio test -e native-test
//
// Covers the head side of the kTypeOta=0x05 bump: real-producer URL bytes
// (linkcode::encodeFrame) through the exact handleCmd seam
// (otamanifest::copyOtaUrl — the function handleCmd calls), the shared seq
// gate framed as duplicate-re-ACK + stale-drop OTA cases, the OLED progress
// state machine (otadisplay::), and the HEAD_FW_VERSION pull-gate vocabulary.
// The firmware handleCmd -> OtaUpdater::startIfNewer call itself is
// compile-covered by the desky-head build (Arduino-only path), like the rest
// of the socket/task code.
//
// The 42B OTA golden below is duplicated from the canonical core vector in
// embedded/core/test/test_link_ota.cpp (seq=0x0042, URL
// "http://192.168.4.1:8080/head.bin", CRC 0x97B8). Duplicated here (not
// shared) so the head consumer stays self-contained; the core test is
// canonical on any drift.

#include <string.h>
#include <unity.h>

#include "../include/config.h"
#include "../src/middleware/face_renderer.h"  // otadisplay:: (Arduino-free top)
#include "../src/middleware/ota_client.h"     // ota::compareVersion (Arduino-free top)
#include "../src/middleware/wifi_link.h"      // linkcode:: alias + wifilink:: (canonical
// linkcode:: spelling is pinned by core's test_link_codec.cpp goldens; head TUs
// must use the alias — see the POSIX ::link note in wifi_link.h)

namespace {

// Literal OTA frame, seq 0x0042, frag 0/1, 32B URL payload (from the encoder).
const uint8_t kOtaGolden[42] = {0x4C, 0x05, 0x00, 0x42, 0x00, 0x01, 0x00, 0x20, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F,
                                0x2F, 0x31, 0x39, 0x32, 0x2E, 0x31, 0x36, 0x38, 0x2E, 0x34, 0x2E, 0x31, 0x3A, 0x38,
                                0x30, 0x38, 0x30, 0x2F, 0x68, 0x65, 0x61, 0x64, 0x2E, 0x62, 0x69, 0x6E, 0x97, 0xB8};
const char kCarrierUrl[] = "http://192.168.4.1:8080/head.bin";

// Pre-bump contract mirror: the frozen type set BEFORE kTypeOta existed.
// Models the OLD head on a mixed fleet — it must reject 0x05 (stays frozen
// on purpose even as the live set grows).
bool oldIsKnownType(uint8_t type) {
  switch (type) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x10:
    case 0x20:
    case 0x21:
    case 0x22:
      return true;
    default:
      return false;
  }
}

}  // namespace

void test_link_ota_producer_bytes_to_updater_exact() {
  // Real-producer path, exactly as handleCmd runs it: encode -> decode ->
  // shared copy -> the updater receives the exact URL bytes (NUL-terminated
  // for the start() entry; here pinned byte-identical through the seam).
  uint8_t produced[linkcode::kOverhead + 32] = {};
  const size_t n = linkcode::encodeFrame(linkcode::kTypeOta, 0x0042, reinterpret_cast<const uint8_t*>(kCarrierUrl), 32,
                                         0, 1, produced, sizeof(produced));
  TEST_ASSERT_EQUAL_size_t(sizeof(kOtaGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kOtaGolden, produced, sizeof(kOtaGolden));
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(produced, n, f));
  TEST_ASSERT_TRUE(wifilink::isOtaFrame(f.type, f.payloadLen));
  char url[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::copyOtaUrl(f.payload, f.payloadLen, url));
  TEST_ASSERT_EQUAL_STRING(kCarrierUrl, url);
}

void test_link_ota_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x05, linkcode::kTypeOta);  // literal OTA type byte
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x05));
  TEST_ASSERT_EQUAL_STRING("OTA", wifilink::typeName(0x05));
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kOtaGolden, sizeof(kOtaGolden), f));
  TEST_ASSERT_EQUAL_UINT8(0x05, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0042, f.seq);
  TEST_ASSERT_EQUAL_UINT16(32, f.payloadLen);
  // Grammar rejects at the seam (the updater entry is never reached).
  char url[otamanifest::kUrlMax + 1];
  const uint8_t cred[] = "http://user@h/fw.bin";
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(cred, sizeof(cred) - 1, url));
  const uint8_t space[] = "http://h/fw a.bin";
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(space, sizeof(space) - 1, url));
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(f.payload, 0, url));
  uint8_t big[129];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = 'x';
  }
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(big, sizeof(big), url));
  // Non-command types never pass the OTA gate.
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeFace, 1));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeQrBitmap, linkcode::kQrBitmapLen));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeOta, 0));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeOta, 129));
}

void test_link_ota_duplicate_reack_and_stale_drop_cases() {
  // Duplicate seq on the shared cmd domain re-ACKs without re-triggering
  // the download; stale drops. Framed here as OTA seqs (the verdicts ride
  // the same classifyRx gate handleCmd applies to every command type).
  using V = wifilink::RxVerdict;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(false, 0, 41)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kDuplicate),
                          static_cast<uint8_t>(wifilink::classifyRx(true, 41, 41)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kStale), static_cast<uint8_t>(wifilink::classifyRx(true, 41, 40)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(V::kFresh), static_cast<uint8_t>(wifilink::classifyRx(true, 41, 42)));
}

void test_link_ota_old_head_drops_as_unknown() {
  TEST_ASSERT_FALSE(oldIsKnownType(0x05));
  TEST_ASSERT_TRUE(oldIsKnownType(0x04));
  // The mechanism both old and new share: the next free type (0x06) is
  // rejected with a valid CRC — old heads reject 0x05 identically.
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x06));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0x06));
  uint8_t hand[linkcode::kOverhead + 1] = {0x4C, 0x06, 0x00, 0x07, 0x00, 0x01, 0x00, 0x01, 0x00};
  linkcode::putU16BE(hand + 9, linkcode::crc16Ccitt(hand, 9));
  linkcode::Frame f;
  TEST_ASSERT_FALSE(linkcode::decodeFrame(hand, sizeof(hand), f));
}

void test_otadisplay_geometry_and_fill() {
  TEST_ASSERT_EQUAL_INT(14, otadisplay::kBarX0);
  TEST_ASSERT_EQUAL_INT(27, otadisplay::kBarY0);
  TEST_ASSERT_EQUAL_INT(100, otadisplay::kBarW);
  TEST_ASSERT_EQUAL_INT(10, otadisplay::kBarH);
  TEST_ASSERT_TRUE(otadisplay::fitsPanel());
  TEST_ASSERT_TRUE(otadisplay::kBarX0 + otadisplay::kBarW <= qrdisplay::kPanelW);
  TEST_ASSERT_TRUE(otadisplay::kBarY0 + otadisplay::kBarH <= qrdisplay::kPanelH);
  TEST_ASSERT_EQUAL_INT(0, otadisplay::fillW(0));
  TEST_ASSERT_EQUAL_INT(48, otadisplay::fillW(50));  // (100-4)*50/100
  TEST_ASSERT_EQUAL_INT(96, otadisplay::fillW(100));
  TEST_ASSERT_EQUAL_INT(96, otadisplay::fillW(200));  // clamped
  TEST_ASSERT_EQUAL_INT(96, otadisplay::fillW(255));  // clamped
}

void test_otadisplay_state_transitions() {
  otadisplay::Progress p;
  TEST_ASSERT_FALSE(p.active);
  otadisplay::show(p, 0);  // FETCH starts empty
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_FALSE(p.failed);
  TEST_ASSERT_EQUAL_UINT8(0, p.pct);
  otadisplay::show(p, 30);  // progress refresh keeps active, clears no fail
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_EQUAL_UINT8(30, p.pct);
  otadisplay::show(p, 250);  // clamped, never overflows the bar
  TEST_ASSERT_EQUAL_UINT8(100, p.pct);
  otadisplay::markFailed(p);  // abort/failure -> alert face
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_TRUE(p.failed);
  otadisplay::hide(p);  // success -> stored face
  TEST_ASSERT_FALSE(p.active);
  TEST_ASSERT_FALSE(p.failed);
  TEST_ASSERT_EQUAL_UINT8(0, p.pct);
  otadisplay::markFailed(p);  // failed-without-show still shows (fail-closed visible)
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_TRUE(p.failed);
  otadisplay::hide(p);
  TEST_ASSERT_FALSE(p.active);
}

void test_link_ota_version_vocabulary_and_gate() {
  const uint32_t running = static_cast<uint32_t>(HEAD_FW_VERSION);
  TEST_ASSERT_EQUAL_UINT32(120000, static_cast<uint32_t>(CFG_FACE_OTA_TIMEOUT_MS));  // literal: fail/timeout rule
  // Gate is relative to the baked macro (bump-per-release discipline): a
  // manifest one above running pulls, equal/older stands down. Literals pin
  // the pure helper; the macro-relative lines below track the macro itself.
  TEST_ASSERT_TRUE(otamanifest::shouldPull(2, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(1, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(0, 1));
  TEST_ASSERT_TRUE(otamanifest::shouldPull(running + 1, running));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(running, running));
  TEST_ASSERT_EQUAL_INT(1, ota::compareVersion(2, 1));
  TEST_ASSERT_EQUAL_INT(0, ota::compareVersion(1, 1));
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(1, 2));
  // Manifest sibling derivation (carrier convention: /head.bin -> /head-version).
  char murl[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::manifestUrlFor(kCarrierUrl, murl));
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1:8080/head-version", murl);
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor("http://h/fw.bin", murl));
  // Shared build/parse contract, head side (core test pins the canonical form).
  const char* sha = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  char text[otamanifest::kManifestCap];
  TEST_ASSERT_TRUE(otamanifest::buildManifest(running + 1, 1234567, sha, text, sizeof(text)));
  otamanifest::Manifest man;
  TEST_ASSERT_TRUE(otamanifest::parseManifest(text, man));
  TEST_ASSERT_EQUAL_UINT32(running + 1, man.version);
  TEST_ASSERT_TRUE(otamanifest::shouldPull(man.version, running));
  TEST_ASSERT_EQUAL_UINT32(1234567, man.size);
}
