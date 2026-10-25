// Host unit tests for the dual-mode WiFi policy (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free wificreds policy in
// src/services/wifi_credentials.h: timeout constant values, credential
// validity (marker + non-empty SSID), boot skip-when-empty, STA join window
// expiry, and connected-then-lost failover timing. The NVS persistence
// (ConfigStore) and the WiFi/mDNS calls (UdpServer) are firmware glue
// exercised on hardware. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/services/wifi_credentials.h"

void test_wifi_timeout_values() {
  TEST_ASSERT_EQUAL_UINT32(15000, wificreds::kStaJoinTimeoutMs);
  TEST_ASSERT_EQUAL_UINT32(120000, wificreds::kStaFailoverMs);
}

void test_wifi_credentials_usable() {
  TEST_ASSERT_TRUE(wificreds::credentialsUsable(true, 1));
  TEST_ASSERT_TRUE(wificreds::credentialsUsable(true, 9));
  TEST_ASSERT_TRUE(wificreds::credentialsUsable(true, wificreds::kSsidMax));
}

void test_wifi_credentials_reject_no_marker() {
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(false, 1));
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(false, 9));
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(false, 0));
}

void test_wifi_credentials_reject_empty_or_oversize_ssid() {
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(true, 0));
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(true, wificreds::kSsidMax + 1));
  TEST_ASSERT_FALSE(wificreds::credentialsUsable(true, 100));
}

void test_wifi_boot_attempts_sta_when_usable() {
  TEST_ASSERT_TRUE(wificreds::shouldAttemptSta(true, "home"));
  TEST_ASSERT_EQUAL(static_cast<int>(wificreds::BootNet::kSta),
                    static_cast<int>(wificreds::decideBootNet(true, "home")));
}

void test_wifi_boot_skips_sta_when_empty() {
  TEST_ASSERT_FALSE(wificreds::shouldAttemptSta(true, ""));
  TEST_ASSERT_FALSE(wificreds::shouldAttemptSta(true, nullptr));
  TEST_ASSERT_FALSE(wificreds::shouldAttemptSta(false, "home"));
  TEST_ASSERT_FALSE(wificreds::shouldAttemptSta(false, ""));
  TEST_ASSERT_FALSE(wificreds::shouldAttemptSta(false, nullptr));
  TEST_ASSERT_EQUAL(static_cast<int>(wificreds::BootNet::kAp),
                    static_cast<int>(wificreds::decideBootNet(false, "home")));
  TEST_ASSERT_EQUAL(static_cast<int>(wificreds::BootNet::kAp), static_cast<int>(wificreds::decideBootNet(true, "")));
  TEST_ASSERT_EQUAL(static_cast<int>(wificreds::BootNet::kAp),
                    static_cast<int>(wificreds::decideBootNet(true, nullptr)));
}

void test_wifi_join_window_holds_then_expires() {
  TEST_ASSERT_FALSE(wificreds::joinTimedOut(1000u, 1000u));
  TEST_ASSERT_FALSE(wificreds::joinTimedOut(1000u, 15999u));
  TEST_ASSERT_TRUE(wificreds::joinTimedOut(1000u, 16000u));
  TEST_ASSERT_TRUE(wificreds::joinTimedOut(0u, 15000u));
  TEST_ASSERT_TRUE(wificreds::joinTimedOut(0u, 60000u));
}

void test_wifi_join_timeout_injectable() {
  TEST_ASSERT_FALSE(wificreds::joinTimedOut(0u, 4999u, 5000u));
  TEST_ASSERT_TRUE(wificreds::joinTimedOut(0u, 5000u, 5000u));
  TEST_ASSERT_FALSE(wificreds::joinTimedOut(0u, 15000u, 120000u));
}

void test_wifi_join_window_wrap_safe() {
  TEST_ASSERT_FALSE(wificreds::joinTimedOut(0xFFFFFF00u, 0xFFFFFF00u + 14999u));
  TEST_ASSERT_TRUE(wificreds::joinTimedOut(0xFFFFFF00u, 0xFFFFFF00u + 15000u));
}

void test_wifi_failover_rides_then_fires() {
  TEST_ASSERT_FALSE(wificreds::failoverDue(1000u, 1000u));
  TEST_ASSERT_FALSE(wificreds::failoverDue(1000u, 120999u));
  TEST_ASSERT_TRUE(wificreds::failoverDue(1000u, 121000u));
  TEST_ASSERT_TRUE(wificreds::failoverDue(0u, 120000u));
  TEST_ASSERT_TRUE(wificreds::failoverDue(0u, 600000u));
}

void test_wifi_failover_injectable_and_wrap_safe() {
  TEST_ASSERT_FALSE(wificreds::failoverDue(0u, 4999u, 5000u));
  TEST_ASSERT_TRUE(wificreds::failoverDue(0u, 5000u, 5000u));
  TEST_ASSERT_FALSE(wificreds::failoverDue(0xFFFFFF00u, 0xFFFFFF00u + 119999u));
  TEST_ASSERT_TRUE(wificreds::failoverDue(0xFFFFFF00u, 0xFFFFFF00u + 120000u));
}

void test_wifi_key_names() {
  TEST_ASSERT_EQUAL_STRING("wssid", wificreds::kSsidKey);
  TEST_ASSERT_EQUAL_STRING("wpass", wificreds::kPassKey);
  TEST_ASSERT_EQUAL_STRING("wok", wificreds::kValidKey);
  TEST_ASSERT_EQUAL(32, static_cast<int>(wificreds::kSsidMax));
  TEST_ASSERT_EQUAL(64, static_cast<int>(wificreds::kPassMax));
}
