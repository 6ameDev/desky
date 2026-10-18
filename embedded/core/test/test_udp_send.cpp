// Host unit tests for the checked-UDP-send helper (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free udpsend::isSendOk predicate in
// shared/common/udp_send.h: all-true -> true, each failure mode -> false.
// The Arduino-guarded udpSendChecked wrapper is thin socket glue (no logging
// inside — caller logs + counts) and is compile-covered by both firmware
// builds. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "common/udp_send.h"

void test_udpsend_all_ok_true() {
  TEST_ASSERT_TRUE(udpsend::isSendOk(1, 10, 10, 1));
  TEST_ASSERT_TRUE(udpsend::isSendOk(1, 0, 0, 1));  // empty datagram still sends
}

void test_udpsend_begin_fail_false() {
  TEST_ASSERT_FALSE(udpsend::isSendOk(0, 10, 10, 1));
  TEST_ASSERT_FALSE(udpsend::isSendOk(-1, 10, 10, 1));
}

void test_udpsend_short_write_false() {
  TEST_ASSERT_FALSE(udpsend::isSendOk(1, 9, 10, 1));
  TEST_ASSERT_FALSE(udpsend::isSendOk(1, 0, 10, 1));
}

void test_udpsend_end_fail_false() {
  TEST_ASSERT_FALSE(udpsend::isSendOk(1, 10, 10, 0));
  TEST_ASSERT_FALSE(udpsend::isSendOk(1, 10, 10, -1));
}

void test_udpsend_all_fail_false() { TEST_ASSERT_FALSE(udpsend::isSendOk(0, 0, 10, 0)); }
