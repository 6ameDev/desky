// Telemetry X-WIRE test (S): literal bytes from the REAL firmware producer
// (telemetry::encodeSnapshot + udp::encodeTelemetry) fed into the REAL
// consumer (udp::decodeTelemetry) plus an INDEPENDENT field-by-field parse.
//
// The 4 literal vectors below were printed by the real encoder and pasted
// here verbatim (same bytes as test/data/telemetry_xwire.json for app/Python
// consumers). Any scale/rounding/clamp/endianness/bit-layout drift on either
// side breaks this test by construction: the independent parse reassembles
// every field with raw shifts and literal masks (0x01/0x02/0x0C), never the
// codec helpers — and the encoder's own output must equal the literals.
//
// Coverage: negative pitch, dist=65535, all 4 modes, cliff+driving bits,
// clamp at +32767. (4 vectors: one per mode — 3 vectors cannot cover 4 modes.)
// Run: pio test -e native-test (runner: test_udp_codec.cpp).

#include <unity.h>

#include "../src/transport/udp_server.h"  // udpstatus top half + services/telemetry_manager.h

namespace {

// One vector: the Snapshot IN, the expected packet fields, and the literal
// 9B wire image [0xBB|pitchBE|rollBE|distBE|flags|XOR].
struct TelemetryXVec {
  const char* name;
  uint8_t mode;
  bool cliff;
  bool driving;
  float pitchDeg;
  float rollDeg;
  uint16_t distMm;
  int16_t pitch;  // expected decideg
  int16_t roll;   // expected decideg
  uint8_t flags;  // expected statusFlags
  uint8_t bytes[udp::kTelemetrySize];
};

// Literals from the real producer (generator: /tmp/gen_xwire.cpp, since removed).
const TelemetryXVec kVecs[] = {
    {"neg_pitch_manual",
     0,
     false,
     false,
     -12.3f,
     4.5f,
     300,
     -123,
     45,
     0x00,
     {0xBB, 0xFF, 0x85, 0x00, 0x2D, 0x01, 0x2C, 0x00, 0xC1}},
    {"maxdist_auto_both",
     1,
     true,
     true,
     45.6f,
     -0.5f,
     65535,
     456,
     -5,
     0x07,
     {0xBB, 0x01, 0xC8, 0xFF, 0xFB, 0xFF, 0xFF, 0x07, 0x71}},
    {"lowpower_cliff",
     2,
     true,
     false,
     0.0f,
     -179.9f,
     0,
     0,
     -1799,
     0x09,
     {0xBB, 0x00, 0x00, 0xF8, 0xF9, 0x00, 0x00, 0x09, 0xB3}},
    {"emergency_clamp",
     3,
     false,
     true,
     4000.0f,
     0.0f,
     1234,
     32767,
     0,
     0x0E,
     {0xBB, 0x7F, 0xFF, 0x00, 0x00, 0x04, 0xD2, 0x0E, 0xE3}},
};

const size_t kNumVecs = sizeof(kVecs) / sizeof(kVecs[0]);

}  // namespace

// Producer seam: Snapshot -> encodeSnapshot -> encodeTelemetry must reproduce
// the literal bytes exactly (catches scale/rounding/clamp/pack drift).
void test_telemetry_xwire_encode_matches_literals() {
  for (size_t i = 0; i < kNumVecs; ++i) {
    const TelemetryXVec& v = kVecs[i];
    telemetry::Snapshot s;
    s.mode = v.mode;
    s.cliff = v.cliff;
    s.driving = v.driving;
    s.pitchDeg = v.pitchDeg;
    s.rollDeg = v.rollDeg;
    s.distMm = v.distMm;
    udp::TelemetryPacket pkt;
    telemetry::encodeSnapshot(s, pkt);
    TEST_ASSERT_EQUAL_INT16(v.pitch, pkt.pitch);
    TEST_ASSERT_EQUAL_INT16(v.roll, pkt.roll);
    TEST_ASSERT_EQUAL_UINT16(v.distMm, pkt.distanceMm);
    TEST_ASSERT_EQUAL_UINT8(v.flags, pkt.statusFlags);
    uint8_t out[udp::kTelemetrySize] = {};
    TEST_ASSERT_TRUE(udp::encodeTelemetry(pkt, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v.bytes, out, udp::kTelemetrySize);
  }
}

// Consumer seam, parsed INDEPENDENTLY: raw BE reassembly, literal bit masks
// (bit0=cliff, bit1=driving, bits2-3=mode), and a hand-rolled XOR — then the
// real decoder must agree field-for-field.
void test_telemetry_xwire_independent_field_parse() {
  for (size_t i = 0; i < kNumVecs; ++i) {
    const TelemetryXVec& v = kVecs[i];
    const uint8_t* b = v.bytes;
    TEST_ASSERT_EQUAL_UINT8(0xBB, b[0]);
    const int16_t pitch = static_cast<int16_t>((static_cast<uint16_t>(b[1]) << 8) | b[2]);
    const int16_t roll = static_cast<int16_t>((static_cast<uint16_t>(b[3]) << 8) | b[4]);
    const uint16_t dist = static_cast<uint16_t>((static_cast<uint16_t>(b[5]) << 8) | b[6]);
    TEST_ASSERT_EQUAL_INT16(v.pitch, pitch);
    TEST_ASSERT_EQUAL_INT16(v.roll, roll);
    TEST_ASSERT_EQUAL_UINT16(v.distMm, dist);
    TEST_ASSERT_EQUAL_UINT8(v.cliff ? 1 : 0, b[7] & 0x01);
    TEST_ASSERT_EQUAL_UINT8(v.driving ? 2 : 0, b[7] & 0x02);
    TEST_ASSERT_EQUAL_UINT8(v.mode, (b[7] & 0x0C) >> 2);
    TEST_ASSERT_EQUAL_UINT8(v.flags, b[7]);
    uint8_t xorSum = 0;
    for (size_t k = 0; k < udp::kTelemetrySize - 1; ++k) {
      xorSum ^= b[k];
    }
    TEST_ASSERT_EQUAL_UINT8(b[udp::kTelemetrySize - 1], xorSum);
    // The real decoder agrees with the independent parse, field-for-field.
    udp::TelemetryPacket pkt;
    TEST_ASSERT_TRUE(udp::decodeTelemetry(b, udp::kTelemetrySize, pkt));
    TEST_ASSERT_EQUAL_INT16(pitch, pkt.pitch);
    TEST_ASSERT_EQUAL_INT16(roll, pkt.roll);
    TEST_ASSERT_EQUAL_UINT16(dist, pkt.distanceMm);
    TEST_ASSERT_EQUAL_UINT8(b[7], pkt.statusFlags);
  }
}

// The literals are live frames, not dead text: any single-bit flip in the
// body or checksum, or a truncation, must fail the real decoder.
void test_telemetry_xwire_literals_are_live_frames() {
  for (size_t i = 0; i < kNumVecs; ++i) {
    uint8_t buf[udp::kTelemetrySize];
    for (size_t k = 0; k < udp::kTelemetrySize; ++k) {
      buf[k] = kVecs[i].bytes[k];
    }
    udp::TelemetryPacket pkt;
    TEST_ASSERT_TRUE(udp::decodeTelemetry(buf, sizeof(buf), pkt));
    buf[7] ^= 0x01;  // flip the cliff bit: checksum now wrong
    TEST_ASSERT_FALSE(udp::decodeTelemetry(buf, sizeof(buf), pkt));
    buf[7] ^= 0x01;  // restore body, corrupt the checksum byte instead
    buf[udp::kTelemetrySize - 1] ^= 0xFF;
    TEST_ASSERT_FALSE(udp::decodeTelemetry(buf, sizeof(buf), pkt));
    TEST_ASSERT_FALSE(udp::decodeTelemetry(buf, sizeof(buf) - 1, pkt));  // truncated
  }
}

// Slice 7: RX observation (noteCommand/isDriving) — the coverage gain of the
// slice. TelemetryManager owns the stale window as a ctor passthrough (same
// pattern as rateMs_); the driving bit is nonzero-stick AND fresh, never
// coordinator state.
void test_telemetry_note_driving_while_fresh() {
  telemetry::TelemetryManager tm(66, 500);
  TEST_ASSERT_EQUAL_UINT32(500, tm.staleMs());
  TEST_ASSERT_FALSE(tm.isDriving(1000));  // No command yet.
  tm.noteCommand(0.5f, 0.0f, 1000);
  TEST_ASSERT_TRUE(tm.isDriving(1000));
  TEST_ASSERT_TRUE(tm.isDriving(1500));   // Boundary counts as fresh.
  TEST_ASSERT_FALSE(tm.isDriving(1501));  // Stale.
}

void test_telemetry_note_zero_stick_never_drives() {
  telemetry::TelemetryManager tm(66, 500);
  tm.noteCommand(0.0f, 0.0f, 1000);
  TEST_ASSERT_FALSE(tm.isDriving(1000));  // Fresh but centered: never drives.
  tm.noteCommand(0.0f, 0.25f, 2000);
  TEST_ASSERT_TRUE(tm.isDriving(2000));  // Pure turn counts as driving.
  tm.noteCommand(0.0f, 0.0f, 3000);
  TEST_ASSERT_FALSE(tm.isDriving(3000));  // Centered again: stops.
}

void test_telemetry_note_wrap_safe_millis() {
  telemetry::TelemetryManager tm(66, 500);
  tm.noteCommand(0.5f, 0.0f, 0xFFFFFF00u);
  TEST_ASSERT_TRUE(tm.isDriving(0xFFFFFF00u));
  TEST_ASSERT_TRUE(tm.isDriving(100));    // Wrapped, 356ms later: fresh.
  TEST_ASSERT_FALSE(tm.isDriving(1000));  // 1256ms later: stale.
}
