// Sealed home-STA credential bundle (no hardware): shared plaintext
// pack/unpack goldens, sealed length gates, seal/open round trip, and the
// core push retry policy. Runner lives in test_udp_codec.cpp.

#include <string.h>
#include <unity.h>

#include "../src/transport/link_server.h"

namespace {

const uint8_t kSsidHome[] = {'h', 'o', 'm', 'e'};
const uint8_t kPassHome[] = {'s', 'e', 'c', 'r', 'e', 't', '1', '2'};
// Plaintext golden: 04 "home" 08 "secret12" (14B, computed once, pinned here).
const uint8_t kCredsGolden[14] = {0x04, 0x68, 0x6F, 0x6D, 0x65, 0x08, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x31, 0x32};

const uint8_t kTestKey[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                              0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

}  // namespace

void test_link_wificreds_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x09, linkcode::kTypeWifiCreds);  // literal bundle type byte
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x09));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x0A));  // next free: still unknown
  TEST_ASSERT_EQUAL_size_t(32, linkcode::kWifiCredSsidMax);
  TEST_ASSERT_EQUAL_size_t(64, linkcode::kWifiCredPassMax);
  TEST_ASSERT_EQUAL_size_t(2, linkcode::kWifiCredPlainMin);
  TEST_ASSERT_EQUAL_size_t(98, linkcode::kWifiCredPlainMax);
  // Sealed frame: nonce(12) + plaintext + tag(16).
  TEST_ASSERT_EQUAL_size_t(30, linkcode::kWifiCredMinLen);
  TEST_ASSERT_EQUAL_size_t(126, linkcode::kWifiCredMaxLen);
  TEST_ASSERT_FALSE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, 29));
  TEST_ASSERT_TRUE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, 30));
  TEST_ASSERT_TRUE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, 126));
  TEST_ASSERT_FALSE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, 127));
}

void test_link_wificreds_plaintext_golden() {
  uint8_t plain[linkcode::kWifiCredPlainMax] = {};
  const size_t n =
      linkcode::packWifiCreds(kSsidHome, sizeof(kSsidHome), kPassHome, sizeof(kPassHome), plain, sizeof(plain));
  TEST_ASSERT_EQUAL_size_t(sizeof(kCredsGolden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kCredsGolden, plain, sizeof(kCredsGolden));
  const uint8_t* ssid = nullptr;
  size_t ssidLen = 0;
  const uint8_t* pass = nullptr;
  size_t passLen = 0;
  TEST_ASSERT_TRUE(linkcode::unpackWifiCreds(plain, n, ssid, ssidLen, pass, passLen));
  TEST_ASSERT_EQUAL_size_t(sizeof(kSsidHome), ssidLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSsidHome, ssid, sizeof(kSsidHome));
  TEST_ASSERT_EQUAL_size_t(sizeof(kPassHome), passLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPassHome, pass, sizeof(kPassHome));
  TEST_ASSERT_FALSE(linkcode::isClearWifiCreds(plain, n));
}

void test_link_wificreds_clear_and_open_network() {
  // CLEAR: both lengths zero packs to 2B and reads back as the wipe signal.
  uint8_t clear[linkcode::kWifiCredPlainMax] = {};
  const size_t n = linkcode::packWifiCreds(nullptr, 0, nullptr, 0, clear, sizeof(clear));
  TEST_ASSERT_EQUAL_size_t(2, n);
  const uint8_t wantClear[2] = {0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantClear, clear, sizeof(wantClear));
  TEST_ASSERT_TRUE(linkcode::isClearWifiCreds(clear, n));
  // Open network: populated SSID with an empty passphrase.
  uint8_t open[linkcode::kWifiCredPlainMax] = {};
  const size_t m = linkcode::packWifiCreds(kSsidHome, sizeof(kSsidHome), nullptr, 0, open, sizeof(open));
  TEST_ASSERT_EQUAL_size_t(6, m);
  const uint8_t* ssid = nullptr;
  size_t ssidLen = 0;
  const uint8_t* pass = nullptr;
  size_t passLen = 0;
  TEST_ASSERT_TRUE(linkcode::unpackWifiCreds(open, m, ssid, ssidLen, pass, passLen));
  TEST_ASSERT_EQUAL_size_t(4, ssidLen);
  TEST_ASSERT_EQUAL_size_t(0, passLen);
  TEST_ASSERT_FALSE(linkcode::isClearWifiCreds(open, m));
}

void test_link_wificreds_pack_rejects() {
  uint8_t out[linkcode::kWifiCredPlainMax] = {};
  uint8_t big[65];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = 'x';
  }
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(big, 33, kPassHome, 8, out, sizeof(out)));     // SSID overlong
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(kSsidHome, 4, big, 65, out, sizeof(out)));     // pass overlong
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(nullptr, 0, kPassHome, 8, out, sizeof(out)));  // zero-SSID mix
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(nullptr, 1, kPassHome, 8, out, sizeof(out)));  // null SSID
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(kSsidHome, 4, nullptr, 1, out, sizeof(out)));  // null pass
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(kSsidHome, 4, kPassHome, 8, nullptr, sizeof(out)));  // null out
  TEST_ASSERT_EQUAL_size_t(0, linkcode::packWifiCreds(kSsidHome, 4, kPassHome, 8, out, 13));               // short cap
  // Boundary 32/64 packs exactly.
  TEST_ASSERT_EQUAL_size_t(98, linkcode::packWifiCreds(big, 32, big, 64, out, sizeof(out)));
}

void test_link_wificreds_unpack_rejects() {
  const uint8_t* ssid = nullptr;
  size_t ssidLen = 0;
  const uint8_t* pass = nullptr;
  size_t passLen = 0;
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(kCredsGolden, 1, ssid, ssidLen, pass, passLen));  // truncated
  uint8_t over[99] = {};
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(over, sizeof(over), ssid, ssidLen, pass, passLen));  // overlong
  const uint8_t badLen[4] = {0x21, 0x00, 0x00, 0x00};  // SSID 33 declared, 2 ride
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(badLen, sizeof(badLen), ssid, ssidLen, pass, passLen));
  const uint8_t trailing[15] = {0x04, 0x68, 0x6F, 0x6D, 0x65, 0x08, 0x73, 0x65,
                                0x63, 0x72, 0x65, 0x74, 0x31, 0x32, 0x00};  // stowaway byte
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(trailing, sizeof(trailing), ssid, ssidLen, pass, passLen));
  const uint8_t mix[3] = {0x00, 0x01, 'x'};  // zero SSID, non-zero pass
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(mix, sizeof(mix), ssid, ssidLen, pass, passLen));
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(nullptr, 2, ssid, ssidLen, pass, passLen));
  TEST_ASSERT_FALSE(linkcode::isClearWifiCreds(mix, sizeof(mix)));
  TEST_ASSERT_FALSE(linkcode::isClearWifiCreds(nullptr, 2));
}

void test_link_wificreds_sealed_round_trip() {
  const linkcrypto::Key key = linkcrypto::provision(kTestKey, sizeof(kTestKey));
  TEST_ASSERT_TRUE(key.ok);
  uint8_t nonce[linkcrypto::kNonceLen] = {};
  TEST_ASSERT_TRUE(linkcrypto::makeCounterNonce(7, nonce));
  uint8_t plain[linkcode::kWifiCredPlainMax] = {};
  const size_t plainLen =
      linkcode::packWifiCreds(kSsidHome, sizeof(kSsidHome), kPassHome, sizeof(kPassHome), plain, sizeof(plain));
  uint8_t sealed[linkcrypto::kNonceLen + linkcode::kWifiCredPlainMax + linkcrypto::kTagLen] = {};
  for (size_t i = 0; i < linkcrypto::kNonceLen; ++i) {
    sealed[i] = nonce[i];
  }
  const size_t bodyLen = linkcrypto::seal(key, nonce, plain, plainLen, sealed + linkcrypto::kNonceLen,
                                          sizeof(sealed) - linkcrypto::kNonceLen);
  TEST_ASSERT_EQUAL_size_t(plainLen + linkcrypto::kTagLen, bodyLen);
  // Frame carries nonce + ciphertext + tag: 12 + 14 + 16 = 42B.
  const uint16_t payloadLen = static_cast<uint16_t>(linkcrypto::kNonceLen + bodyLen);
  TEST_ASSERT_EQUAL_UINT16(42, payloadLen);
  uint8_t frame[linkcode::kMaxFrame] = {};
  const size_t n =
      linkcode::encodeFrame(linkcode::kTypeWifiCreds, 0x0021, sealed, payloadLen, 0, 1, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + payloadLen, n);
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(frame, n, f));
  TEST_ASSERT_EQUAL_UINT8(linkcode::kTypeWifiCreds, f.type);
  TEST_ASSERT_EQUAL_UINT16(payloadLen, f.payloadLen);
  // Receiver splits the nonce back off and opens fail-closed.
  uint8_t opened[linkcode::kWifiCredPlainMax] = {};
  size_t openedLen = 0;
  TEST_ASSERT_TRUE(linkcrypto::open(key, f.payload, f.payload + linkcrypto::kNonceLen,
                                    f.payloadLen - linkcrypto::kNonceLen, opened, sizeof(opened), openedLen));
  TEST_ASSERT_EQUAL_size_t(plainLen, openedLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, opened, plainLen);
  const uint8_t* ssid = nullptr;
  size_t ssidLen = 0;
  const uint8_t* pass = nullptr;
  size_t passLen = 0;
  TEST_ASSERT_TRUE(linkcode::unpackWifiCreds(opened, openedLen, ssid, ssidLen, pass, passLen));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSsidHome, ssid, sizeof(kSsidHome));
  // Tampered ciphertext fails closed with the output zeroed.
  sealed[linkcrypto::kNonceLen] ^= 0x01;
  TEST_ASSERT_FALSE(
      linkcrypto::open(key, sealed, sealed + linkcrypto::kNonceLen, bodyLen, opened, sizeof(opened), openedLen));
}

void test_link_wificreds_clear_sealed_minimum() {
  // CLEAR seals to the 30B frame minimum end to end.
  const linkcrypto::Key key = linkcrypto::provision(kTestKey, sizeof(kTestKey));
  uint8_t nonce[linkcrypto::kNonceLen] = {};
  TEST_ASSERT_TRUE(linkcrypto::makeCounterNonce(1, nonce));
  uint8_t plain[linkcode::kWifiCredPlainMax] = {};
  const size_t plainLen = linkcode::packWifiCreds(nullptr, 0, nullptr, 0, plain, sizeof(plain));
  TEST_ASSERT_EQUAL_size_t(2, plainLen);
  uint8_t sealed[linkcode::kWifiCredMinLen] = {};
  for (size_t i = 0; i < linkcrypto::kNonceLen; ++i) {
    sealed[i] = nonce[i];
  }
  const size_t bodyLen = linkcrypto::seal(key, nonce, plain, plainLen, sealed + linkcrypto::kNonceLen,
                                          sizeof(sealed) - linkcrypto::kNonceLen);
  TEST_ASSERT_EQUAL_size_t(2 + linkcrypto::kTagLen, bodyLen);
  uint8_t frame[linkcode::kMaxFrame] = {};
  const size_t n =
      linkcode::encodeFrame(linkcode::kTypeWifiCreds, 3, sealed, linkcode::kWifiCredMinLen, 0, 1, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_size_t(linkcode::kOverhead + linkcode::kWifiCredMinLen, n);
  linkcode::Frame f;
  TEST_ASSERT_TRUE(linkcode::decodeFrame(frame, n, f));
  uint8_t opened[linkcode::kWifiCredPlainMax] = {};
  size_t openedLen = 0;
  TEST_ASSERT_TRUE(linkcrypto::open(key, f.payload, f.payload + linkcrypto::kNonceLen,
                                    f.payloadLen - linkcrypto::kNonceLen, opened, sizeof(opened), openedLen));
  TEST_ASSERT_TRUE(linkcode::isClearWifiCreds(opened, openedLen));
}

void test_linkserver_cred_retry_policy() {
  // 5 attempts over ~5s: first at the portal relay, then every 1s.
  TEST_ASSERT_EQUAL_UINT8(5, linkserver::kCredMaxAttempts);
  TEST_ASSERT_EQUAL_UINT32(1000, linkserver::kCredRetryMs);
  TEST_ASSERT_TRUE(linkserver::credRetryDue(0, 0, 1000));
  TEST_ASSERT_TRUE(linkserver::credRetryDue(4, 0, 1000));
  TEST_ASSERT_FALSE(linkserver::credRetryDue(4, 0, 999));
  TEST_ASSERT_FALSE(linkserver::credRetryDue(5, 0, 9000));                          // budget spent
  TEST_ASSERT_FALSE(linkserver::credRetryDue(200, 0, 9000));                        // counter never wraps past the cap
  TEST_ASSERT_TRUE(linkserver::credRetryDue(1, 0xFFFFFF00u, 0xFFFFFF00u + 3000u));  // wrap-safe
  TEST_ASSERT_FALSE(linkserver::credRetryDue(1, 0xFFFFFF00u, 0xFFFFFF00u + 999u));
}

void test_linkserver_wificreds_type_name() {
  TEST_ASSERT_EQUAL_STRING("WIFICREDS", linkserver::typeName(linkcode::kTypeWifiCreds));
  TEST_ASSERT_EQUAL_STRING("WIFICREDS", linkserver::typeName(0x09));  // literal: the bundle bump
}

void test_linkserver_cred_arm_only_if_sent() {
  // Refused first send arms nothing: no budget, no seq.
  const linkserver::CredPushArm refused = linkserver::credPushArm(false, 0);
  TEST_ASSERT_FALSE(refused.armed);
  TEST_ASSERT_FALSE(refused.haveSeq);
  TEST_ASSERT_EQUAL_UINT16(0, refused.seq);
  TEST_ASSERT_EQUAL_UINT8(0, refused.attempts);
  // Refused send never arms, even with a stale seq value on hand.
  const linkserver::CredPushArm refusedStale = linkserver::credPushArm(false, 41);
  TEST_ASSERT_FALSE(refusedStale.armed);
  TEST_ASSERT_FALSE(refusedStale.haveSeq);
  TEST_ASSERT_EQUAL_UINT8(0, refusedStale.attempts);
  // Sent first datagram arms one attempt carrying the minted seq.
  const linkserver::CredPushArm armed = linkserver::credPushArm(true, 41);
  TEST_ASSERT_TRUE(armed.armed);
  TEST_ASSERT_TRUE(armed.haveSeq);
  TEST_ASSERT_EQUAL_UINT16(41, armed.seq);
  TEST_ASSERT_EQUAL_UINT8(1, armed.attempts);
}
