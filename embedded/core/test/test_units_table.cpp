// Host unit tests pinning the RELATIONSHIPS between timeouts/rates (no hardware).
// Run: pio test -e native-test (runner lives in test_udp_codec.cpp).
//
// Every timeout/rate below is already tested where it is defined; what was
// missing is the products and orderings that otherwise live only in
// developers' heads. Each test asserts one intended relation with literals
// on at least one side plus a comment stating the intent, so flipping either
// constant breaks the test and forces a conscious decision.
//
// Indirection note: core include/config.h IS host-includable (the MCU
// headers are stdint-only), so the CFG_* macros below are the true project
// values, not header fallbacks. Two exceptions use literals + comments: the
// head QR timeout (head include/config.h cannot share this TU — CFG_I2C_SDA
// et al. would redeclare with different values) and the firmware-only class
// constexprs (UdpServer::kTelemetryMs/kStaleMs, Coordinator::kStaleMs live
// behind #ifdef ARDUINO), whose defining expressions are recomputed here.

#include <unity.h>

#include "../../head/src/transport/link_codec.h"
#include "../include/config.h"
#include "../src/abilities/pairing/pairing_codec.h"
#include "../src/behavior/coordinator.h"
#include "../src/transport/udp_server.h"

void test_units_qr_pairing_window_order() {
  // Intent: the head QR show clears (120s) well before the core pairing
  // window expires (300s), so a displayed code can never outlive the window
  // that accepts it — QR-clears-before-expiry BY DESIGN. The QR side is the
  // literal from head include/config.h:49 (fallback face_renderer.h:83);
  // the head mirror test_qr_pairing_window_order_head pins the real macro.
  TEST_ASSERT_EQUAL_UINT32(300000, pairing::kWindowMs);
  const uint32_t kQrShowMs = 120000;  // == CFG_FACE_QR_TIMEOUT_MS (head)
  TEST_ASSERT_TRUE(kQrShowMs < pairing::kWindowMs);
  TEST_ASSERT_EQUAL_UINT32(180000, pairing::kWindowMs - kQrShowMs);
}

void test_units_telemetry_rate_product() {
  // Intent: telemetry ships at nominal CFG_TELEMETRY_HZ with the
  // integer-truncated period the firmware actually uses
  // (UdpServer::kTelemetryMs = 1000 / CFG_TELEMETRY_HZ, firmware-only).
  // 1000/15 truncates 66.67 -> 66, so the fence runs 10ms/s fast: 66ms*15 =
  // 990ms, effective ~15.15Hz — inside the frozen 10-20Hz wire window
  // (shared/protocol/README.md §telemetry). Both sides (firmware TX cadence,
  // bench/app RX expectations of nominal 15.00Hz) must agree on 66, not 67.
  TEST_ASSERT_EQUAL_UINT32(15, static_cast<uint32_t>(CFG_TELEMETRY_HZ));
  TEST_ASSERT_EQUAL_UINT32(66, 1000u / static_cast<uint32_t>(CFG_TELEMETRY_HZ));
  TEST_ASSERT_EQUAL_UINT32(990, 66u * 15u);
  telemetry::TelemetryManager tm(66);
  TEST_ASSERT_EQUAL_UINT32(66, tm.rateMs());
}

void test_units_alive_stale_fence() {
  // Intent: the 5s alive window covers exactly 10x the 500ms coordinator
  // stale failsafe, so the robot stops driving (stale) long before the link
  // is declared dead (alive) — failsafe-first BY DESIGN. kAliveMs is the
  // single owner (udpstatus, udp_server.h:136); the stale side is
  // CFG_COORDINATOR_STALE_MS, mirrored firmware-only as
  // Coordinator::kStaleMs and UdpServer::kStaleMs (both #ifdef ARDUINO).
  TEST_ASSERT_EQUAL_UINT32(5000, udpstatus::kAliveMs);
  TEST_ASSERT_EQUAL_UINT32(500, static_cast<uint32_t>(CFG_COORDINATOR_STALE_MS));
  TEST_ASSERT_EQUAL_UINT32(10, udpstatus::kAliveMs / static_cast<uint32_t>(CFG_COORDINATOR_STALE_MS));
  TEST_ASSERT_EQUAL_UINT32(udpstatus::kAliveMs, 10u * static_cast<uint32_t>(CFG_COORDINATOR_STALE_MS));
}

void test_units_linkdown_cadence() {
  // Intent: the head re-associates after ~3s of silence — 3 missed HB ACKs
  // at the 1Hz heartbeat. linkDownAfter(3) is already pinned in the head
  // suite; this pins the PRODUCT both sides must agree on: 3 * 1000ms,
  // matching the firmware re-associate gate (wifi_link.h heartbeatTick:
  // `>= 3u * CFG_LINK_HEARTBEAT_MS`). Note the gate's `3u` duplicates
  // CFG_LINK_MAX_MISSED — this test ties the two spellings together.
  // The heartbeat side is the literal from head include/config.h:20
  // (fallbacks wifi_link.h:279, link_stub.h:110 — all firmware-gated, so no
  // host-visible macro exists; same reason as the QR literal above).
  const uint32_t kHbMs = 1000;  // == CFG_LINK_HEARTBEAT_MS (head)
  TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(CFG_LINK_MAX_MISSED));
  TEST_ASSERT_FALSE(wifilink::linkDownAfter(2));
  TEST_ASSERT_TRUE(wifilink::linkDownAfter(3));
  TEST_ASSERT_EQUAL_UINT32(3000, 3u * kHbMs);
  TEST_ASSERT_EQUAL_UINT32(3000, static_cast<uint32_t>(CFG_LINK_MAX_MISSED) * kHbMs);
}

void test_units_rekey_margin_seconds() {
  // Intent: the silent era+PSK rotation starts REKEY_MARGIN frames before
  // the fail-closed exhaustion gate, leaving seconds (not frames) of
  // headroom for the 3x 1s REKEY_REQ/ACK exchange plus retries. Pinned as a
  // rate-parameterized relation (seconds = frames / Hz), never a bare 20.
  // Tree values: margin is 1000 frames (app-side DriveCounter.REKEY_MARGIN,
  // client/.../DriveCounter.kt:239 — no firmware constexpr exists, and the
  // brief's "50" matches nothing in-tree, so it is NOT pinned here) at the
  // spec 50Hz drive rate (telemetry-phase2-spec.md:11, README §rekey).
  const uint32_t kDriveHz = 50;
  const uint32_t kMarginFrames = 1000;  // == DriveCounter.REKEY_MARGIN
  const uint32_t kGate = 0xFFFFFF00u;   // == DriveCounter.EXHAUSTION_GATE
  TEST_ASSERT_EQUAL_UINT32(1000, kMarginFrames);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFB18u, kGate - kMarginFrames);
  TEST_ASSERT_EQUAL_UINT32(20000, kMarginFrames * 1000u / kDriveHz);
}
