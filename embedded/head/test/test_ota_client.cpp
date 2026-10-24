// OTA client pure top half (no hardware): parse, version, validation,
// manifest, stream gate, state machine, CLI verb. Literals, never symbols.

#include <unity.h>

#include "../src/abilities/ota/ota_codec.h"

void test_ota_parse_plain_host() {
  ota::Url u;
  TEST_ASSERT_TRUE(ota::parseUrl("http://192.168.4.1/fw.bin", u));
  TEST_ASSERT_EQUAL_STRING("192.168.4.1", u.host);
  TEST_ASSERT_EQUAL_UINT16(80, u.port);
  TEST_ASSERT_EQUAL_STRING("/fw.bin", u.path);
}

void test_ota_parse_port_and_deep_path() {
  ota::Url u;
  TEST_ASSERT_TRUE(ota::parseUrl("http://laptop:8080/a/b/c.bin", u));
  TEST_ASSERT_EQUAL_STRING("laptop", u.host);
  TEST_ASSERT_EQUAL_UINT16(8080, u.port);
  TEST_ASSERT_EQUAL_STRING("/a/b/c.bin", u.path);
}

void test_ota_parse_bare_host_defaults_path() {
  ota::Url u;
  TEST_ASSERT_TRUE(ota::parseUrl("http://head", u));
  TEST_ASSERT_EQUAL_STRING("head", u.host);
  TEST_ASSERT_EQUAL_UINT16(80, u.port);
  TEST_ASSERT_EQUAL_STRING("/", u.path);
}

void test_ota_parse_rejects() {
  ota::Url u;
  TEST_ASSERT_FALSE(ota::parseUrl(nullptr, u));
  TEST_ASSERT_FALSE(ota::parseUrl("", u));
  TEST_ASSERT_FALSE(ota::parseUrl("https://h/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("HTTPS://h/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("ftp://h/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("192.168.4.1/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http:///fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://user:pass@h/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h/fw a.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h:0/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h:99999/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h:abc/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h:/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://h:80:80/fw.bin", u));
  TEST_ASSERT_FALSE(ota::parseUrl("http://[::1]/fw.bin", u));
}

void test_ota_version_orders() {
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(1, 2));
  TEST_ASSERT_EQUAL_INT(0, ota::compareVersion(7, 7));
  TEST_ASSERT_EQUAL_INT(1, ota::compareVersion(9, 3));

  const uint32_t v100 = ota::packVersion(1, 0, 0);
  const uint32_t v101 = ota::packVersion(1, 0, 1);
  const uint32_t v110 = ota::packVersion(1, 1, 0);
  const uint32_t v200 = ota::packVersion(2, 0, 0);
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(v100, v101));
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(v101, v110));
  TEST_ASSERT_EQUAL_INT(-1, ota::compareVersion(v110, v200));
  TEST_ASSERT_EQUAL_INT(0, ota::compareVersion(v110, v110));
  TEST_ASSERT_EQUAL_INT(1, ota::compareVersion(v200, v100));
}

void test_ota_magic() {
  const uint8_t good[] = {0xE9, 0x00, 0x04, 0x40};
  TEST_ASSERT_TRUE(ota::hasOtaMagic(good, sizeof(good)));
  const uint8_t bad[] = {0x00, 0xE9, 0xE9};
  TEST_ASSERT_FALSE(ota::hasOtaMagic(bad, sizeof(bad)));
  TEST_ASSERT_FALSE(ota::hasOtaMagic(good, 0));
  TEST_ASSERT_FALSE(ota::hasOtaMagic(nullptr, 4));
}

void test_ota_fits_slot() {
  TEST_ASSERT_TRUE(ota::fitsSlot(1, 0x1F0000));
  TEST_ASSERT_TRUE(ota::fitsSlot(0x1F0000, 0x1F0000));
  TEST_ASSERT_FALSE(ota::fitsSlot(0x1F0001, 0x1F0000));
  TEST_ASSERT_FALSE(ota::fitsSlot(0, 0x1F0000));
  TEST_ASSERT_FALSE(ota::fitsSlot(100, 0));
}

void test_ota_stream_gate() {
  TEST_ASSERT_TRUE(ota::streamComplete(100, 100));
  TEST_ASSERT_FALSE(ota::streamComplete(99, 100));
  TEST_ASSERT_FALSE(ota::streamComplete(0, 100));
  TEST_ASSERT_FALSE(ota::streamComplete(0, 0));
}

void test_ota_manifest_matches() {
  const char* lower = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  const char* upper = "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08";
  TEST_ASSERT_TRUE(ota::manifestMatches(lower, lower));
  TEST_ASSERT_TRUE(ota::manifestMatches(lower, upper));
  const char* oneOff = "8f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  TEST_ASSERT_FALSE(ota::manifestMatches(lower, oneOff));
  TEST_ASSERT_FALSE(ota::manifestMatches(lower, "9f86d081"));
  char longHex[66];
  memset(longHex, 'a', 65);
  longHex[65] = '\0';
  TEST_ASSERT_FALSE(ota::manifestMatches(lower, longHex));
  TEST_ASSERT_FALSE(ota::manifestMatches(lower, nullptr));
  TEST_ASSERT_FALSE(ota::manifestMatches(nullptr, lower));
  TEST_ASSERT_FALSE(ota::manifestMatches(lower, "gf86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"));
}

void test_ota_bytes_to_hex() {
  uint8_t digest[32];
  memset(digest, 0, sizeof(digest));
  digest[0] = 0x9F;
  digest[31] = 0x08;
  char out[65];
  ota::bytesToHex64(digest, out);
  TEST_ASSERT_EQUAL_STRING("9f00000000000000000000000000000000000000000000000000000000000008", out);

  ota::bytesToHex64(nullptr, out);
  ota::bytesToHex64(digest, nullptr);
}

void test_ota_state_happy_path() {
  ota::State s = ota::State::kIdle;
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStart));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kFetch), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kChunk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kFetch), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kChunk));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStreamEnd));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kVerify), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kVerifyOk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kCommit), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kCommitOk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));
}

void test_ota_state_verify_fail_and_abort() {
  ota::State s = ota::State::kIdle;
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStart));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStreamEnd));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kVerifyFail));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStart));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kAbort));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));

  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kAbort));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));
}

void test_ota_state_rejects_bad_events() {
  ota::State s = ota::State::kIdle;
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kStreamEnd));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kVerifyOk));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kCommitOk));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kChunk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStart));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kStart));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kVerifyOk));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kCommitOk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kFetch), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kStreamEnd));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kChunk));
  TEST_ASSERT_FALSE(ota::step(s, ota::Event::kCommitOk));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kVerify), static_cast<uint8_t>(s));
  TEST_ASSERT_TRUE(ota::step(s, ota::Event::kAbort));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::State::kIdle), static_cast<uint8_t>(s));
}
