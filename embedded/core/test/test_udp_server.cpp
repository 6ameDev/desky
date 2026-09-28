// Host unit tests for the UDP server helpers (no hardware).
// Run: pio test -e native-test
//
// Covers the Arduino-free namespace udpstatus (statusFlags bit map +
// decidegree math in src/middleware/udp_server.h), the coordinator
// packCommand->unpackCommand round-trip at the center-128 edges, and a
// telemetry encode->decode round-trip with flags set. The UdpServer task
// (WiFi/socket/WDT) is firmware-only (#ifdef ARDUINO) and exercised on
// hardware. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/behavior/coordinator.h"
#include "../src/middleware/udp_codec.h"
#include "../src/middleware/udp_server.h"

void test_status_flags_pack_bits() {
  TEST_ASSERT_EQUAL_UINT8(0x01, udpstatus::packStatus(true, false, 0));
  TEST_ASSERT_EQUAL_UINT8(0x02, udpstatus::packStatus(false, true, 0));
  TEST_ASSERT_EQUAL_UINT8(0x08, udpstatus::packStatus(false, false, 2));
  TEST_ASSERT_EQUAL_UINT8(0x0F, udpstatus::packStatus(true, true, 3));
  TEST_ASSERT_EQUAL_UINT8(0x00, udpstatus::packStatus(false, false, 0));
  // Reserved bits 4-7 stay 0 for every mode.
  for (uint8_t mode = 0; mode < 4; ++mode) {
    TEST_ASSERT_EQUAL_UINT8(0, udpstatus::packStatus(true, true, mode) & 0xF0);
  }
}

void test_status_flags_unpack_round_trip() {
  for (uint8_t mode = 0; mode < 4; ++mode) {
    const uint8_t flags = udpstatus::packStatus(true, false, mode);
    TEST_ASSERT_TRUE(udpstatus::statusCliff(flags));
    TEST_ASSERT_FALSE(udpstatus::statusDriving(flags));
    TEST_ASSERT_EQUAL_UINT8(mode, udpstatus::statusMode(flags));
  }
  const uint8_t flags = udpstatus::packStatus(false, true, 1);
  TEST_ASSERT_FALSE(udpstatus::statusCliff(flags));
  TEST_ASSERT_TRUE(udpstatus::statusDriving(flags));
  TEST_ASSERT_EQUAL_UINT8(1, udpstatus::statusMode(flags));
}

void test_pack_command_center_edges() {
  // Stick edges: 0 -> -1, 128 -> 0, 255 -> +1 through the event payload.
  uint8_t mode = 0;
  uint8_t flags = 0;
  float v = 0.0f;
  float omega = 0.0f;
  coordinator::unpackCommand(coordinator::packCommand(0x02, 0, 255, 0x05), mode, v, omega, flags);
  TEST_ASSERT_EQUAL_UINT8(0x02, mode);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, -1.0f, v);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.0f, omega);
  TEST_ASSERT_EQUAL_UINT8(0x05, flags);
  coordinator::unpackCommand(coordinator::packCommand(0x00, 128, 128, 0x00), mode, v, omega, flags);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, omega);
}

void test_telemetry_round_trip_with_flags_set() {
  udp::TelemetryPacket src;
  src.pitch = udpstatus::degToDecideg(12.3f);
  src.roll = udpstatus::degToDecideg(-4.5f);
  src.distanceMm = 1234;
  src.statusFlags = udpstatus::packStatus(true, true, 3);  // cliff + driving + EMERGENCY
  TEST_ASSERT_EQUAL_UINT8(0x0F, src.statusFlags);
  uint8_t buf[udp::kTelemetrySize];
  TEST_ASSERT_TRUE(udp::encodeTelemetry(src, buf));
  udp::TelemetryPacket dst;
  TEST_ASSERT_TRUE(udp::decodeTelemetry(buf, sizeof(buf), dst));
  TEST_ASSERT_EQUAL_INT16(123, dst.pitch);
  TEST_ASSERT_EQUAL_INT16(-45, dst.roll);
  TEST_ASSERT_EQUAL_UINT16(1234, dst.distanceMm);
  TEST_ASSERT_EQUAL_UINT8(0x0F, dst.statusFlags);
  TEST_ASSERT_TRUE(udpstatus::statusCliff(dst.statusFlags));
  TEST_ASSERT_TRUE(udpstatus::statusDriving(dst.statusFlags));
  TEST_ASSERT_EQUAL_UINT8(3, udpstatus::statusMode(dst.statusFlags));
}

void test_deg_to_decideg_edges_and_clamp() {
  TEST_ASSERT_EQUAL_INT16(0, udpstatus::degToDecideg(0.0f));
  TEST_ASSERT_EQUAL_INT16(123, udpstatus::degToDecideg(12.3f));
  TEST_ASSERT_EQUAL_INT16(-45, udpstatus::degToDecideg(-4.5f));
  TEST_ASSERT_EQUAL_INT16(32767, udpstatus::degToDecideg(4000.0f));
  TEST_ASSERT_EQUAL_INT16(-32768, udpstatus::degToDecideg(-4000.0f));
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the udpstatus tests are declared extern there.
