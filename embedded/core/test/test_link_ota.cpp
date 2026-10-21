// Host unit tests for the carrier OTA trigger (no hardware).
// Run: pio test -e native-test
//
// Covers the additive kTypeOta=0x05 bump (both sides together): the shared
// codec length contract (1..128, range shape like JPEG), the shared URL
// grammar + manifest text contract (link/ota_manifest.h), the core staging
// gate vocabulary (linkserver::otaReadyToSend), and the `otahead` console
// verb grammar (corecmd::). Runner lives in test_udp_codec.cpp.
//
// Wire literals below are computed, not symbolic: the representative carrier
// URL "http://192.168.4.1:8080/head.bin" (32B) in a seq=0x0042 OTA frame is
//   4C 05 00 42 00 01 00 20 <32 URL bytes> 97 B8  (42B total).

#include <string.h>
#include <unity.h>

#include "../src/services/cli_manager.h"
#include "../src/transport/link_server.h"
#include "link/ota_manifest.h"

// Pre-bump contract mirror: the frozen type set BEFORE kTypeOta existed.
// An old head runs exactly this table, so it must reject 0x05 — the
// mixed-fleet drop-as-unknown case. If the live set below ever gains a type,
// this mirror stays frozen on purpose (it models the OLD peer, not the new).
namespace {
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

const char kCarrierUrl[] = "http://192.168.4.1:8080/head.bin";  // 32B representative URL
const uint8_t kOtaGolden[42] = {0x4C, 0x05, 0x00, 0x42, 0x00, 0x01, 0x00, 0x20, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F,
                                0x2F, 0x31, 0x39, 0x32, 0x2E, 0x31, 0x36, 0x38, 0x2E, 0x34, 0x2E, 0x31, 0x3A, 0x38,
                                0x30, 0x38, 0x30, 0x2F, 0x68, 0x65, 0x61, 0x64, 0x2E, 0x62, 0x69, 0x6E, 0x97, 0xB8};
}  // namespace

void test_link_ota_golden_encode() {
  const size_t urlLen = strlen(kCarrierUrl);
  TEST_ASSERT_EQUAL_size_t(32, urlLen);
  uint8_t out[linkcode::kMaxFrame] = {};
  const size_t n = linkcode::encodeFrame(linkcode::kTypeOta, 0x0042, reinterpret_cast<const uint8_t*>(kCarrierUrl),
                                         static_cast<uint16_t>(urlLen), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(kOtaGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kOtaGolden, out, sizeof(kOtaGolden));
  TEST_ASSERT_EQUAL_UINT8(0x05, out[1]);  // literal OTA type byte
}

void test_link_ota_golden_decode() {
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(kOtaGolden, sizeof(kOtaGolden), f));
  TEST_ASSERT_EQUAL_UINT8(linkcode::kTypeOta, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0042, f.seq);
  TEST_ASSERT_EQUAL_UINT8(0, f.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, f.fragTotal);
  TEST_ASSERT_EQUAL_UINT16(32, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kCarrierUrl, f.payload, 32);
}

void test_link_ota_staged_url_round_trip_identical() {
  // The stageOtaUrl path: shared-grammar copy -> encode -> decode -> the
  // URL bytes come back identical (staged URL -> frame -> byte-identical).
  char staged[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::copyOtaUrl(reinterpret_cast<const uint8_t*>(kCarrierUrl), strlen(kCarrierUrl), staged));
  TEST_ASSERT_EQUAL_STRING(kCarrierUrl, staged);
  uint8_t out[linkcode::kMaxFrame] = {};
  const size_t n = linkcode::encodeFrame(linkcode::kTypeOta, 9, reinterpret_cast<const uint8_t*>(staged),
                                         static_cast<uint16_t>(strlen(staged)), 0, 1, out, sizeof(out));
  TEST_ASSERT_TRUE(n > linkcode::kOverhead);
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(out, n, f));
  char back[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::copyOtaUrl(f.payload, f.payloadLen, back));
  TEST_ASSERT_EQUAL_STRING(kCarrierUrl, back);
}

void test_link_ota_length_gates_reject_0_and_129() {
  uint8_t big[129];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = 'a';
  }
  uint8_t out[linkcode::kMaxFrame] = {};
  // Encode gate: 0 and 129 are outside 1..128.
  TEST_ASSERT_EQUAL_size_t(
      0, linkcode::encodeFrame(linkcode::kTypeOta, 1, reinterpret_cast<const uint8_t*>(kCarrierUrl), 0, 0, 1, out,
                               sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, linkcode::encodeFrame(linkcode::kTypeOta, 1, big, sizeof(big), 0, 1, out, sizeof(out)));
  // Decode gate: hand-built 0B and 129B frames with valid CRCs still fail.
  uint8_t empty10[linkcode::kOverhead] = {0x4C, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
  linkcode::putU16BE(empty10 + 8, linkcode::crc16Ccitt(empty10, 8));
  linkcode::Frame f;
  TEST_ASSERT_FALSE(linkcode::decodeFrame(empty10, sizeof(empty10), f));
  uint8_t wide[linkcode::kOverhead + 129] = {};
  wide[0] = linkcode::kMagic;
  wide[1] = linkcode::kTypeOta;
  linkcode::putU16BE(wide + 2, 3);
  wide[4] = 0;
  wide[5] = 1;
  linkcode::putU16BE(wide + 6, 129);
  for (size_t i = 0; i < 129; ++i) {
    wide[linkcode::kHeaderSize + i] = 'b';
  }
  linkcode::putU16BE(wide + linkcode::kHeaderSize + 129, linkcode::crc16Ccitt(wide, linkcode::kHeaderSize + 129));
  TEST_ASSERT_FALSE(linkcode::decodeFrame(wide, sizeof(wide), f));
  // Boundaries 1 and 128 encode.
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + 1,
                           linkcode::encodeFrame(linkcode::kTypeOta, 1, big, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + 128,
                           linkcode::encodeFrame(linkcode::kTypeOta, 1, big, 128, 0, 1, out, sizeof(out)));
}

void test_link_ota_known_type_literal_set() {
  // New bump + frozen history: exactly the eight old types + 0x05 (OTA) +
  // 0x06 (PAIRSHOW) + 0x07 (PAIRDONE).
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x01));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x02));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x03));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x04));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x05));  // literal: the OTA bump
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x06));  // literal: the PAIRSHOW bump
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x07));  // literal: the PAIRDONE bump
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x10));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x20));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x21));
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x22));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x00));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x08));  // next free: still unknown
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x0F));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x11));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x1F));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x23));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0xFF));
  // Shared URL bound pins to the codec contract (never mirrored silently).
  TEST_ASSERT_EQUAL_size_t(linkcode::kOtaUrlMinLen, 1);
  TEST_ASSERT_EQUAL_size_t(linkcode::kOtaUrlMaxLen, 128);
  TEST_ASSERT_EQUAL_size_t(linkcode::kOtaUrlMaxLen, otamanifest::kUrlMax);
  // Unknown types never encode (fail-closed at the producer seam).
  uint8_t out[linkcode::kMaxFrame] = {};
  const uint8_t one[1] = {0x00};
  TEST_ASSERT_EQUAL_size_t(0, linkcode::encodeFrame(0x08, 1, one, 1, 0, 1, out, sizeof(out)));
}

void test_link_ota_old_head_drops_as_unknown() {
  // Mixed fleet: the old head's frozen table rejects 0x05 while accepting
  // everything it shipped with — a pre-bump peer drops the OTA frame as
  // unknown (mechanism pinned: the live codec rejects the NEXT free type
  // 0x08 the same way, valid CRC and all).
  TEST_ASSERT_FALSE(oldIsKnownType(0x05));
  TEST_ASSERT_TRUE(oldIsKnownType(0x01));
  TEST_ASSERT_TRUE(oldIsKnownType(0x02));
  TEST_ASSERT_TRUE(oldIsKnownType(0x03));
  TEST_ASSERT_TRUE(oldIsKnownType(0x04));
  TEST_ASSERT_TRUE(oldIsKnownType(0x10));
  TEST_ASSERT_TRUE(oldIsKnownType(0x20));
  TEST_ASSERT_TRUE(oldIsKnownType(0x21));
  TEST_ASSERT_TRUE(oldIsKnownType(0x22));
  uint8_t out[linkcode::kMaxFrame] = {};
  const uint8_t one[1] = {0x00};
  const size_t n = linkcode::encodeFrame(0x08, 7, one, 1, 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, n);  // unknown types never even encode
  uint8_t hand[linkcode::kOverhead + 1] = {0x4C, 0x08, 0x00, 0x07, 0x00, 0x01, 0x00, 0x01, 0x00};
  linkcode::putU16BE(hand + 9, linkcode::crc16Ccitt(hand, 9));
  linkcode::Frame f;
  TEST_ASSERT_FALSE(linkcode::decodeFrame(hand, sizeof(hand), f));  // valid CRC, unknown type
}

void test_linkserver_ota_gate_once_per_command_with_peer() {
  TEST_ASSERT_TRUE(linkserver::otaReadyToSend(true, false, true));
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(false, false, true));  // nothing staged
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(true, true, true));    // already sent this command
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(true, false, false));  // no head linked: wait
}

void test_linkserver_ota_type_name() {
  TEST_ASSERT_EQUAL_STRING("OTA", linkserver::typeName(linkcode::kTypeOta));
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", linkserver::typeName(linkcode::kTypePairShow));
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", linkserver::typeName(linkcode::kTypePairDone));
  TEST_ASSERT_EQUAL_STRING("PAIRSHOW", linkserver::typeName(0x06));  // literal: the PAIRSHOW bump
  TEST_ASSERT_EQUAL_STRING("PAIRDONE", linkserver::typeName(0x07));  // literal: the PAIRDONE bump
}

void test_otamanifest_ota_url_grammar() {
  const uint8_t good[] = "http://192.168.4.1:8080/head.bin";
  TEST_ASSERT_TRUE(otamanifest::isValidOtaUrlBytes(good, sizeof(good) - 1));
  char out[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::copyOtaUrl(good, sizeof(good) - 1, out));
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1:8080/head.bin", out);
  // Fail-closed: empty, overlong, credentials, whitespace, non-printable.
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(good, 0));
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(nullptr, 3));
  uint8_t big[129];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = 'x';
  }
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(big, sizeof(big)));
  const uint8_t cred[] = "http://user@h/fw.bin";
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(cred, sizeof(cred) - 1));
  const uint8_t space[] = "http://h/fw a.bin";
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(space, sizeof(space) - 1));
  const uint8_t nul[] = {'h', 't', 0x01, 'p'};
  TEST_ASSERT_FALSE(otamanifest::isValidOtaUrlBytes(nul, sizeof(nul)));
  // copyOtaUrl leaves `out` untouched on failure.
  out[0] = 'Z';
  out[1] = '\0';
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(cred, sizeof(cred) - 1, out));
  TEST_ASSERT_EQUAL_STRING("Z", out);
  TEST_ASSERT_FALSE(otamanifest::copyOtaUrl(good, sizeof(good) - 1, nullptr));
}

void test_otamanifest_build_parse_round_trip() {
  const char* sha = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  char text[otamanifest::kManifestCap];
  TEST_ASSERT_TRUE(otamanifest::buildManifest(2, 1234567, sha, text, sizeof(text)));
  TEST_ASSERT_EQUAL_STRING(
      "version=2\nsize=1234567\nsha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\n", text);
  otamanifest::Manifest man;
  TEST_ASSERT_TRUE(otamanifest::parseManifest(text, man));
  TEST_ASSERT_EQUAL_UINT32(2, man.version);
  TEST_ASSERT_EQUAL_UINT32(1234567, man.size);
  TEST_ASSERT_EQUAL_STRING(sha, man.sha);
  // Strict rejects: missing line, duplicate, bad hex, bad decimal, null.
  TEST_ASSERT_FALSE(otamanifest::parseManifest("version=2\nsize=9\n", man));
  TEST_ASSERT_FALSE(otamanifest::parseManifest(
      "version=2\nversion=3\nsize=9\nsha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\n", man));
  TEST_ASSERT_FALSE(otamanifest::parseManifest("version=2\nsize=9\nsha256=zzzz\n", man));
  TEST_ASSERT_FALSE(otamanifest::parseManifest(
      "version=x\nsize=9\nsha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\n", man));
  TEST_ASSERT_FALSE(otamanifest::parseManifest(nullptr, man));
  TEST_ASSERT_FALSE(otamanifest::buildManifest(1, 1, "short", text, sizeof(text)));
  TEST_ASSERT_FALSE(otamanifest::buildManifest(1, 1, sha, nullptr, sizeof(text)));
  // Order-free + CRLF tolerated; unknown lines ignored.
  TEST_ASSERT_TRUE(
      otamanifest::parseManifest("size=7\r\nnote=carrier\r\nsha256="
                                 "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08\r\nversion=4\r\n",
                                 man));
  TEST_ASSERT_EQUAL_UINT32(4, man.version);
  TEST_ASSERT_EQUAL_UINT32(7, man.size);
}

void test_otamanifest_sibling_url_and_gate() {
  char murl[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(otamanifest::manifestUrlFor("http://192.168.4.1:8080/head.bin", murl));
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1:8080/head-version", murl);
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor("http://h/fw.bin", murl));  // non-convention: direct pull
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor("http://h/head.bin/extra", murl));
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor(nullptr, murl));
  TEST_ASSERT_FALSE(otamanifest::manifestUrlFor("http://h/head.bin", nullptr));
  TEST_ASSERT_TRUE(otamanifest::shouldPull(2, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(1, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(0, 1));
  TEST_ASSERT_FALSE(otamanifest::shouldPull(1, 2));
}

void test_corecli_otahead_verb() {
  corecmd::CoreCommand cmd;
  TEST_ASSERT_TRUE(corecmd::parseLine("otahead http://192.168.4.1:8080/head.bin", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(corecmd::Target::OTAHEAD), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(corecmd::parseLine("  otahead http://h/a.bin 2  ", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(corecmd::Target::OTAHEAD), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(corecmd::parseLine("help", cmd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(corecmd::Target::HELP), static_cast<uint8_t>(cmd.target));
  TEST_ASSERT_TRUE(corecmd::parseLine("  help  ", cmd));
  TEST_ASSERT_FALSE(corecmd::parseLine("otahead", cmd));     // no URL
  TEST_ASSERT_FALSE(corecmd::parseLine("otahead   ", cmd));  // blank URL
  TEST_ASSERT_FALSE(corecmd::parseLine("otaheadx http://h/a", cmd));
  TEST_ASSERT_FALSE(corecmd::parseLine("OTAHEAD http://h/a", cmd));  // CLI stays lowercase
  TEST_ASSERT_FALSE(corecmd::parseLine("", cmd));
  TEST_ASSERT_FALSE(corecmd::parseLine(nullptr, cmd));
  TEST_ASSERT_FALSE(corecmd::parseLine("status", cmd));
}

void test_corecli_otahead_tail_split() {
  // Grammar: `otahead <url>` — URL only. The version used to ride here; it
  // now rides the POST ?version= exclusively, so any trailing token fails.
  char url[otamanifest::kUrlMax + 1];
  TEST_ASSERT_TRUE(corecmd::parseOtaheadTail("http://h/a.bin", url));
  TEST_ASSERT_EQUAL_STRING("http://h/a.bin", url);
  TEST_ASSERT_TRUE(corecmd::parseOtaheadTail("  http://h:8080/head.bin  ", url));
  TEST_ASSERT_EQUAL_STRING("http://h:8080/head.bin", url);
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("", url));
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("   ", url));
  char big[140];
  for (size_t i = 0; i < sizeof(big) - 1; ++i) {
    big[i] = 'y';
  }
  big[sizeof(big) - 1] = '\0';
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail(big, url));               // over 128
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("http://h/a 2", url));    // version token rejected
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("http://h/a 1 2", url));  // trailing garbage
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("http://h/a\t2", url));   // tab-separated token
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail(nullptr, url));
  TEST_ASSERT_FALSE(corecmd::parseOtaheadTail("http://h/a", nullptr));
}

void test_otamanifest_upload_version_decision() {
  // Single source of truth: the version rides the POST; the matrix below is
  // the whole contract (missing keeps, valid adopts, malformed rejects).
  otamanifest::VersionDecision d;
  d = otamanifest::decideUploadVersion(false, false, 0, 7);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(otamanifest::VersionApply::kKeep), static_cast<uint8_t>(d.action));
  TEST_ASSERT_EQUAL_UINT32(7, d.version);
  d = otamanifest::decideUploadVersion(false, true, 9, 7);  // validity ignored when absent
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(otamanifest::VersionApply::kKeep), static_cast<uint8_t>(d.action));
  TEST_ASSERT_EQUAL_UINT32(7, d.version);
  d = otamanifest::decideUploadVersion(true, true, 2, 0);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(otamanifest::VersionApply::kAdopt), static_cast<uint8_t>(d.action));
  TEST_ASSERT_EQUAL_UINT32(2, d.version);
  d = otamanifest::decideUploadVersion(true, true, 2, 9);  // adopt even when older: label, not gate
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(otamanifest::VersionApply::kAdopt), static_cast<uint8_t>(d.action));
  TEST_ASSERT_EQUAL_UINT32(2, d.version);
  d = otamanifest::decideUploadVersion(true, false, 0, 9);  // malformed: preserve previous
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(otamanifest::VersionApply::kReject), static_cast<uint8_t>(d.action));
  TEST_ASSERT_EQUAL_UINT32(9, d.version);
}

void test_linkserver_ota_restage_rearms_send() {
  // Console-verb semantics: every valid stage replaces + re-arms, so the
  // send gate below must read ready again after a restage — this is the
  // regression for the one-shot latch (second `otahead` staged nothing).
  // Sequence: nothing staged -> not ready; staged -> ready with peer;
  // consumed (valid=false, sent=true, as trySendOta leaves it) -> not ready;
  // restaged (valid=true, sent=false) -> ready again.
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(false, false, true));
  TEST_ASSERT_TRUE(linkserver::otaReadyToSend(true, false, true));
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(true, false, false));  // no peer waits
  TEST_ASSERT_FALSE(linkserver::otaReadyToSend(true, true, true));    // consumed latch
  TEST_ASSERT_TRUE(linkserver::otaReadyToSend(true, false, true));    // restaged: re-armed
}
