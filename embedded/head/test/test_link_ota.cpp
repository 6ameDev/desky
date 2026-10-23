// Carrier OTA consumer (no hardware): producer URL bytes through the
// handleCmd seam, seq gate, OLED progress state, version pull-gate.
// 42B golden: seq 0x0042, "http://192.168.4.1:8080/head.bin", CRC 0x97B8.

#include <string.h>
#include <unity.h>

#include "../include/config.h"
#include "../src/abilities/display/display_codec.h"  // otadisplay:: (Arduino-free)
#include "../src/abilities/ota/ota_codec.h"          // ota::compareVersion (Arduino-free)
#include "../src/transport/link_codec.h"             // linkcode:: alias + wifilink:: (canonical
// linkcode:: spelling is pinned by core's test_link_codec.cpp goldens; head TUs
// must use the alias — see the POSIX ::link note in link_codec.h)

namespace {

// Literal OTA frame, seq 0x0042, frag 0/1, 32B URL payload (from the encoder).
const uint8_t kOtaGolden[42] = {0x4C, 0x05, 0x00, 0x42, 0x00, 0x01, 0x00, 0x20, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F,
                                0x2F, 0x31, 0x39, 0x32, 0x2E, 0x31, 0x36, 0x38, 0x2E, 0x34, 0x2E, 0x31, 0x3A, 0x38,
                                0x30, 0x38, 0x30, 0x2F, 0x68, 0x65, 0x61, 0x64, 0x2E, 0x62, 0x69, 0x6E, 0x97, 0xB8};
const char kCarrierUrl[] = "http://192.168.4.1:8080/head.bin";

// Pre-bump type set (old heads reject 0x05).
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
  // Real-producer path: encode -> decode -> shared copy hands the updater exact bytes.
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

  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeFace, 1));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypePairShow, linkcode::kPairShowLen));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeOta, 0));
  TEST_ASSERT_FALSE(wifilink::isOtaFrame(linkcode::kTypeOta, 129));
}

void test_link_ota_duplicate_reack_and_stale_drop_cases() {
  // Duplicate re-ACKs without re-triggering; stale drops (shared classifyRx gate).
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
  // Next free type (0x08) rejects with valid CRC.
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x06));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x07));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x08));
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", wifilink::typeName(0x06));
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", wifilink::typeName(0x07));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0x08));
  uint8_t hand[linkcode::kOverhead + 1] = {0x4C, 0x08, 0x00, 0x07, 0x00, 0x01, 0x00, 0x01, 0x00};
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
  otadisplay::show(p, 0);
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_FALSE(p.failed);
  TEST_ASSERT_EQUAL_UINT8(0, p.pct);
  otadisplay::show(p, 30);
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_EQUAL_UINT8(30, p.pct);
  otadisplay::show(p, 250);
  TEST_ASSERT_EQUAL_UINT8(100, p.pct);
  otadisplay::markFailed(p);
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_TRUE(p.failed);
  otadisplay::hide(p);
  TEST_ASSERT_FALSE(p.active);
  TEST_ASSERT_FALSE(p.failed);
  TEST_ASSERT_EQUAL_UINT8(0, p.pct);
  otadisplay::markFailed(p);
  TEST_ASSERT_TRUE(p.active);
  TEST_ASSERT_TRUE(p.failed);
  otadisplay::hide(p);
  TEST_ASSERT_FALSE(p.active);
}

void test_link_ota_version_vocabulary_and_gate() {
  const uint32_t running = static_cast<uint32_t>(HEAD_FW_VERSION);
  TEST_ASSERT_EQUAL_UINT32(120000, static_cast<uint32_t>(CFG_DISPLAY_OTA_TIMEOUT_MS));
  // Macro-relative: one above running pulls, equal/older stands down.
  TEST_ASSERT_TRUE(otamanifest::shouldPull(2, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(1, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(0, 1));
  TEST_ASSERT_TRUE(otamanifest::shouldPull(running + 1, running));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(running, running));
  TEST_ASSERT_EQUAL_INT(1, ota::compareVersion(2, 1));
  TEST_ASSERT_EQUAL_INT(0, ota::compareVersion(1, 1));
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(1, 2));

  char murl[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::manifestUrlFor(kCarrierUrl, murl));
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1:8080/head-version", murl);
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor("http://h/fw.bin", murl));

  const char* sha = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  char text[otamanifest::kManifestCap];
  TEST_ASSERT_TRUE(otamanifest::buildManifest(running + 1, 1234567, sha, text, sizeof(text)));
  otamanifest::Manifest man;
  TEST_ASSERT_TRUE(otamanifest::parseManifest(text, man));
  TEST_ASSERT_EQUAL_UINT32(running + 1, man.version);
  TEST_ASSERT_TRUE(otamanifest::shouldPull(man.version, running));
  TEST_ASSERT_EQUAL_UINT32(1234567, man.size);
}
