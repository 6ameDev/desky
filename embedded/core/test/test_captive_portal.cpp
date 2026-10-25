// Host unit tests for the captive portal policy (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace portal in
// src/services/captive_portal.h: store validation (non-empty SSID,
// empty-or-8+ passphrase), route contracts, the relay-wait budget pinned
// against the link retry constants, wrap-safe timeout, HTML escaping, and
// the stored header (which takes no passphrase, so none can render). The
// AsyncWebServer/DNS handlers and the NVS/link wiring are firmware glue
// exercised on hardware. Runner lives in test_udp_codec.cpp (single main).

#include <string.h>
#include <unity.h>

#include "../src/services/captive_portal.h"
#include "../src/transport/link_server.h"

void test_portal_store_valid_matrix() {
  TEST_ASSERT_TRUE(portal::storeValid(1, 0));  // open network
  TEST_ASSERT_TRUE(portal::storeValid(9, 8));
  TEST_ASSERT_TRUE(portal::storeValid(wificreds::kSsidMax, wificreds::kPassMax));
  TEST_ASSERT_FALSE(portal::storeValid(0, 0));  // empty SSID
  TEST_ASSERT_FALSE(portal::storeValid(0, 8));
  TEST_ASSERT_FALSE(portal::storeValid(wificreds::kSsidMax + 1, 8));  // SSID overlong
  TEST_ASSERT_FALSE(portal::storeValid(4, 1));                        // passphrase too short
  TEST_ASSERT_FALSE(portal::storeValid(4, 7));
  TEST_ASSERT_FALSE(portal::storeValid(4, wificreds::kPassMax + 1));  // passphrase overlong
}

void test_portal_routes() {
  TEST_ASSERT_EQUAL_STRING("/", portal::kRouteRoot);
  TEST_ASSERT_EQUAL_STRING("/store", portal::kRouteStore);
  TEST_ASSERT_EQUAL_STRING("/forget", portal::kRouteForget);
  TEST_ASSERT_EQUAL_STRING("/status", portal::kRouteStatus);
}

void test_portal_relay_budget_honors_link_retry() {
  TEST_ASSERT_EQUAL_UINT8(linkserver::kCredMaxAttempts, portal::kRelayAttempts);
  TEST_ASSERT_EQUAL_UINT32(linkserver::kCredRetryMs, portal::kRelayRetryMs);
  TEST_ASSERT_TRUE(portal::kRelayWaitMs >=
                   static_cast<uint32_t>(linkserver::kCredMaxAttempts) * linkserver::kCredRetryMs);
  TEST_ASSERT_EQUAL_UINT32(7000, portal::kRelayWaitMs);
}

void test_portal_relay_timeout_edges() {
  TEST_ASSERT_FALSE(portal::relayTimedOut(1000u, 1000u));
  TEST_ASSERT_FALSE(portal::relayTimedOut(1000u, 7999u));
  TEST_ASSERT_TRUE(portal::relayTimedOut(1000u, 8000u));
  TEST_ASSERT_TRUE(portal::relayTimedOut(0u, 60000u));
}

void test_portal_relay_timeout_wrap_safe() {
  TEST_ASSERT_FALSE(portal::relayTimedOut(0xFFFFFF00u, 0xFFFFFF00u + 6999u));
  TEST_ASSERT_TRUE(portal::relayTimedOut(0xFFFFFF00u, 0xFFFFFF00u + 7000u));
}

void test_portal_escape_html() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(7, portal::escapeHtml("a&b", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a&amp;b", out);
  TEST_ASSERT_EQUAL_size_t(14, portal::escapeHtml("<>\"", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("&lt;&gt;&quot;", out);
  char tiny[5];
  portal::escapeHtml("<scr", tiny, sizeof(tiny));
  for (size_t i = 0; tiny[i] != '\0'; ++i) {
    TEST_ASSERT_NOT_EQUAL('<', tiny[i]);
  }
  TEST_ASSERT_EQUAL_size_t(0, portal::escapeHtml(nullptr, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, portal::escapeHtml("x", nullptr, 0));
}

void test_portal_status_header() {
  char out[64];
  portal::statusHeader(out, sizeof(out), false, nullptr);
  TEST_ASSERT_EQUAL_STRING("stored: none", out);
  portal::statusHeader(out, sizeof(out), true, "home");
  TEST_ASSERT_EQUAL_STRING("stored: home", out);
  portal::statusHeader(out, sizeof(out), true, "a<b");
  TEST_ASSERT_EQUAL_STRING("stored: a&lt;b", out);
  portal::statusHeader(out, sizeof(out), true, "");
  TEST_ASSERT_EQUAL_STRING("stored: none", out);
}

void test_portal_mode_and_relay_texts() {
  TEST_ASSERT_EQUAL_STRING("First-time setup", portal::modeTitle(portal::Mode::kSetup));
  TEST_ASSERT_EQUAL_STRING("Manage WiFi", portal::modeTitle(portal::Mode::kManage));
  TEST_ASSERT_EQUAL_STRING("Recovery", portal::modeTitle(portal::Mode::kRecovery));
  TEST_ASSERT_EQUAL_STRING("relaying", portal::relayText(portal::Relay::kRelaying));
  TEST_ASSERT_EQUAL_STRING("stored", portal::relayText(portal::Relay::kStored));
  TEST_ASSERT_EQUAL_STRING("failed", portal::relayText(portal::Relay::kFailed));
  TEST_ASSERT_EQUAL_STRING("store", portal::opText(portal::Op::kStore));
  TEST_ASSERT_EQUAL_STRING("clear", portal::opText(portal::Op::kClear));
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the portal tests are declared extern there.
