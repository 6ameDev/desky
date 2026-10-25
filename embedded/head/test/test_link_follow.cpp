// Dual-mode follow order + credential bundle contract (no hardware):
// candidate order, sealed-bundle goldens, store validity. Runner lives in
// test_main.cpp.

#include <string.h>
#include <unity.h>

#include "../include/config.h"
#include "../src/services/wifi_store.h"
#include "../src/transport/link_codec.h"

namespace {

const uint8_t kSsidHome[] = {'h', 'o', 'm', 'e'};
const uint8_t kPassHome[] = {'s', 'e', 'c', 'r', 'e', 't', '1', '2'};

}  // namespace

void test_follow_first_target_matrix() {
  using F = wifilink::FollowTarget;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kLastGood),
                          static_cast<uint8_t>(wifilink::firstFollowTarget(true, true)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kLastGood),
                          static_cast<uint8_t>(wifilink::firstFollowTarget(true, false)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kHome),
                          static_cast<uint8_t>(wifilink::firstFollowTarget(false, true)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kCoreAp),
                          static_cast<uint8_t>(wifilink::firstFollowTarget(false, false)));
}

void test_follow_advance_order_and_skips() {
  using F = wifilink::FollowTarget;
  F next = F::kCoreAp;
  // Full order with everything available: last-good -> home -> core AP.
  TEST_ASSERT_TRUE(wifilink::advanceFollowTarget(F::kLastGood, true, true, next));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kHome), static_cast<uint8_t>(next));
  TEST_ASSERT_TRUE(wifilink::advanceFollowTarget(F::kHome, true, true, next));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kCoreAp), static_cast<uint8_t>(next));
  // Core AP exhausts the cycle: the caller backs off before restarting.
  TEST_ASSERT_FALSE(wifilink::advanceFollowTarget(F::kCoreAp, true, true, next));
  // Without home creds the order skips straight to the AP.
  TEST_ASSERT_TRUE(wifilink::advanceFollowTarget(F::kLastGood, true, false, next));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kCoreAp), static_cast<uint8_t>(next));
  TEST_ASSERT_TRUE(wifilink::advanceFollowTarget(F::kHome, false, false, next));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(F::kCoreAp), static_cast<uint8_t>(next));
  TEST_ASSERT_FALSE(wifilink::advanceFollowTarget(F::kCoreAp, false, false, next));
}

void test_follow_backoff_config() {
  TEST_ASSERT_EQUAL_UINT32(10000, static_cast<uint32_t>(CFG_LINK_FOLLOW_BACKOFF_MS));
  TEST_ASSERT_EQUAL_STRING("desky", CFG_LINK_MDNS_HOST);
}

void test_follow_creds_literal_contract() {
  TEST_ASSERT_EQUAL_UINT8(0x09, linkcode::kTypeWifiCreds);  // literal bundle type byte
  TEST_ASSERT_TRUE(linkcode::isKnownType(0x09));
  TEST_ASSERT_FALSE(linkcode::isKnownType(0x0A));  // next free: still unknown
  TEST_ASSERT_EQUAL_STRING("WIFICREDS", wifilink::typeName(0x09));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", wifilink::typeName(0x0A));
  TEST_ASSERT_TRUE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, linkcode::kWifiCredMinLen));
  TEST_ASSERT_TRUE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, linkcode::kWifiCredMaxLen));
  TEST_ASSERT_FALSE(linkcode::isValidPayloadLen(linkcode::kTypeWifiCreds, 0));
}

void test_follow_creds_plaintext_golden() {
  // Same wire bytes the core seals: 04 "home" 08 "secret12".
  const uint8_t want[14] = {0x04, 0x68, 0x6F, 0x6D, 0x65, 0x08, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x31, 0x32};
  uint8_t plain[linkcode::kWifiCredPlainMax] = {};
  const size_t n =
      linkcode::packWifiCreds(kSsidHome, sizeof(kSsidHome), kPassHome, sizeof(kPassHome), plain, sizeof(plain));
  TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, plain, sizeof(want));
  const uint8_t* ssid = nullptr;
  size_t ssidLen = 0;
  const uint8_t* pass = nullptr;
  size_t passLen = 0;
  TEST_ASSERT_TRUE(linkcode::unpackWifiCreds(plain, n, ssid, ssidLen, pass, passLen));
  TEST_ASSERT_EQUAL_size_t(4, ssidLen);
  TEST_ASSERT_EQUAL_size_t(8, passLen);
  // CLEAR reads back as the wipe signal.
  uint8_t clear[linkcode::kWifiCredPlainMax] = {};
  TEST_ASSERT_EQUAL_size_t(2, linkcode::packWifiCreds(nullptr, 0, nullptr, 0, clear, sizeof(clear)));
  TEST_ASSERT_TRUE(linkcode::isClearWifiCreds(clear, 2));
  // Tampered lengths fail closed, never misread as CLEAR.
  const uint8_t mix[3] = {0x00, 0x01, 'x'};
  TEST_ASSERT_FALSE(linkcode::unpackWifiCreds(mix, sizeof(mix), ssid, ssidLen, pass, passLen));
  TEST_ASSERT_FALSE(linkcode::isClearWifiCreds(mix, sizeof(mix)));
}

void test_follow_store_validity() {
  TEST_ASSERT_TRUE(headwifi::credentialsUsable(true, 1));
  TEST_ASSERT_TRUE(headwifi::credentialsUsable(true, 32));
  TEST_ASSERT_FALSE(headwifi::credentialsUsable(false, 4));
  TEST_ASSERT_FALSE(headwifi::credentialsUsable(true, 0));
  TEST_ASSERT_FALSE(headwifi::credentialsUsable(true, 33));
  TEST_ASSERT_EQUAL_size_t(32, headwifi::kSsidMax);
  TEST_ASSERT_EQUAL_size_t(64, headwifi::kPassMax);
}
