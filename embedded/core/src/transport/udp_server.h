#pragma once
// desky v2 UDP server — header-only, Phase 4 Workstream B.
//
// Owns dual-mode WiFi + the binary control/telemetry sockets (wire format in
// transport/udp_codec.h, FROZEN) + one Core 0 FreeRTOS task. Control RX
// publishes EVENT_UDP_COMMAND_RECEIVED per datagram (never deduped — the
// coordinator resume edge needs every fresh command); telemetry TX encodes
// udp::TelemetryPacket at CFG_TELEMETRY_HZ from live sensors.
//
// Structure mirrors coordinator.h: the top half (namespace udpstatus) is
// Arduino-free byte math (stdint only) so host Unity tests include this
// header directly. The bottom half (class UdpServer) is firmware-only
// (#ifdef ARDUINO): WiFi + WiFiUDP + Core 0 task.
//
// WiFi modes (one decision at boot, no runtime switching, no portal):
//   default (DESKY_WIFI_STA absent/0) = AP "desky" (CFG_WIFI_AP_SSID/PASS),
//     fixed 192.168.4.1; humans join from a phone, zero router dependency.
//   DESKY_WIFI_STA=1 (desky-sta env) = robot joins the home router via .env
//     WIFI_SSID/WIFI_PASS (build-injected macros; PASS never logged).
// Both modes connect asynchronously: AP serves with zero clients attached;
// STA never blocks setup (V1-style while(!connected) is banned) — the task
// loop retries WiFi.begin() every CFG_WIFI_STA_RETRY_MS and logs the DHCP
// IP once when the link comes up.
//
// Telemetry source:
//   mode/active/cliff = Coordinator::snapshot() (coordinator-owned intent;
//     the cliff level is fed by the sensor task's EVENT_CLIFF_DETECTED edge
//     publish, so the bit is live);
//   distanceMM = ToF distanceMm() live; pitch/roll = fusion::Fusion over the
//     live MPU reading on demand (same atan2f convention as fusion; real
//     tilt once the MPU is healthy, 0 before that);
//   isDriving = telemetry::TelemetryManager::isDriving (services-owned RX
//     observation: last commanded v/omega nonzero AND fresh within
//     CFG_COORDINATOR_STALE_MS; fed by the coordinator drive handler via the
//     noteDriveCommand passthrough — the coordinator owns no driving flag).
// Destination: unicast-only to the last alive peer (IP+port pinned by the
// markAlive funnel on every valid bonded packet); silent otherwise — the
// subnet-broadcast fallback is deleted, pre-bond stays silent.
//
// Rules: no heap/String in the RX/TX path (fixed stack buffers: 20B RX,
// 9B telemetry, 39B PAIR_OK, 8/11/16B HMAC msg), WDT fed every tick,
// Diagnostics::logWatermarks("UDP") every ~5s, task never halts. Key, code
// and nonce bytes are never logged (the pair-code LOG_I line is the
// display); drops log first-of-boot at INFO then throttled LOG_EVERY_N,
// HMAC timing uses throttled LOG_EVERY_N.

// ── Arduino-free: statusFlags bit map + fixed-point helpers ──────────────
#include <stddef.h>
#include <stdint.h>

#include "abilities/pairing/pairing_codec.h"
#include "common/udp_send.h"
#include "transport/connection_manager.h"

// Fallback keeps the host tests + firmware compiling if config keys drift;
// in-project include/config.h always wins (same pattern as pairing_codec.h).
#ifndef CFG_PAIR_REDELIVERY_MS
#define CFG_PAIR_REDELIVERY_MS 60000
#endif

namespace udpstatus {

// Telemetry statusFlags bit map (wire byte: udp::TelemetryPacket.statusFlags,
// scripts/udp_telemetry.py decodes the same map):
//   bit 0    = cliff (1 = hazard latched)
//   bit 1    = driving (1 = last commanded v/omega nonzero and fresh)
//   bits 2-3 = mode (0 MANUAL, 1 AUTONOMOUS, 2 LOW_POWER, 3 EMERGENCY)
//   bits 4-7 = reserved, always 0.
constexpr uint8_t kCliffBit = 0;
constexpr uint8_t kDrivingBit = 1;
constexpr uint8_t kModeShift = 2;
constexpr uint8_t kModeMask = 0x03;

inline uint8_t packStatus(bool cliff, bool driving, uint8_t mode) {
  uint8_t flags = 0;
  if (cliff) {
    flags |= static_cast<uint8_t>(1u << kCliffBit);
  }
  if (driving) {
    flags |= static_cast<uint8_t>(1u << kDrivingBit);
  }
  flags |= static_cast<uint8_t>((mode & kModeMask) << kModeShift);
  return flags;
}

inline bool statusCliff(uint8_t flags) { return (flags & (1u << kCliffBit)) != 0; }

inline bool statusDriving(uint8_t flags) { return (flags & (1u << kDrivingBit)) != 0; }

inline uint8_t statusMode(uint8_t flags) { return static_cast<uint8_t>((flags >> kModeShift) & kModeMask); }

// Degrees -> decidegrees (0.1 deg), clamped to int16 for the wire.
inline int16_t degToDecideg(float deg) {
  const float d = deg * 10.0f;
  if (d >= 32767.0f) {
    return 32767;
  }
  if (d <= -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(d >= 0.0f ? d + 0.5f : d - 0.5f);
}

// ── Pairing + authenticated drive v1 (frozen spec:
// shared/protocol/README.md §"Pairing + authenticated drive v1") ─────────
// Wire constants: DRIVE_V2 App->Robot 16B, PAIR_REQ App->Robot 12B. Legacy
// literals below equal udp::kControlSize/kControlHeader (6/0xAA) but stay
// literal so this top half needs no udp_codec.h include (host-testable with
// stdint/stddef only).
constexpr uint8_t kDriveV2Header = 0xAC;
constexpr size_t kDriveV2Size = 16;
// Pairing-owned vocabulary (single owner: pairing_codec.h; mirrored here for
// the host-testable demux — equality pinned by test_udp_server static_asserts).
constexpr uint8_t kPairReqHeader = pairing::kPairReqHeader;
constexpr size_t kPairReqSize = pairing::kPairReqSize;
constexpr uint8_t kUnbondHeader = pairing::kUnbondHeader;
constexpr size_t kUnbondSize = pairing::kUnbondSize;
constexpr size_t kUnbondTagLen = pairing::kUnbondTagLen;
constexpr size_t kUnbondMsgLen = pairing::kUnbondMsgLen;  // "UNBOND" (6 ASCII) || counterBE4
// Tier-1 silent auto-rekey (see pairing_codec.h + shared/protocol/README.md):
// REKEY_REQ 14B [0x53|eraBE2|ctrBE4|tag6|XOR], REKEY_ACK 10B (TX only).
// Headers 0x53/0x54 are claimed in pairing_codec.h — never reuse 0xAA/0xAC/0xBB/
// 0x50/0x51/0x52/0xAD (Phase-2 SET_RATE must pick other values).
constexpr uint8_t kRekeyReqHeader = pairing::kRekeyReqHeader;
constexpr size_t kRekeyReqSize = pairing::kRekeyReqSize;
constexpr uint8_t kRekeyAckHeader = pairing::kRekeyAckHeader;
constexpr size_t kRekeyAckSize = pairing::kRekeyAckSize;
// ALIVE + DISCONNECT (robot alive rule, see pairing_codec.h + README):
// ALIVE 14B [0x55|eraBE2|ctrBE4|tag6|XOR], DISCONNECT 14B
// [0x56|eraBE2|ctrBE4|tag6|XOR]. Headers 0x55/0x56 are claimed in
// pairing_codec.h — never reuse 0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/0xAD.
constexpr uint8_t kAliveHeader = pairing::kAliveHeader;
constexpr size_t kAliveSize = pairing::kAliveSize;
constexpr uint8_t kDisconnectHeader = pairing::kDisconnectHeader;
constexpr size_t kDisconnectSize = pairing::kDisconnectSize;
// Robot alive freshness window: a bonded peer counts as alive while its
// last valid packet (ALIVE, DRIVE_V2, or REKEY_REQ) is within kAliveMs.
// DISCONNECT/UNBOND clear the net peer immediately (refresh-then-clear),
// so the window only matters for silent loss (no explicit goodbye).
constexpr uint32_t kAliveMs = 5000;
constexpr uint32_t kRedeliveryMs = CFG_PAIR_REDELIVERY_MS;

// RX demux result: classifyRx maps (len, header) -> kind; anything else
// (wrong size, wrong header, oversize truncation) is kDrop.
enum class RxKind : uint8_t { kLegacy, kDriveV2, kPairReq, kUnbond, kRekeyReq, kAlive, kDisconnect, kDrop };

inline RxKind classifyRx(size_t len, uint8_t header) {
  if (len == 6 && header == 0xAA) {
    return RxKind::kLegacy;
  }
  if (len == kDriveV2Size && header == kDriveV2Header) {
    return RxKind::kDriveV2;
  }
  if (len == kPairReqSize && header == kPairReqHeader) {
    return RxKind::kPairReq;
  }
  if (len == kUnbondSize && header == kUnbondHeader) {
    return RxKind::kUnbond;
  }
  if (len == kRekeyReqSize && header == kRekeyReqHeader) {
    return RxKind::kRekeyReq;
  }
  if (len == kAliveSize && header == kAliveHeader) {
    return RxKind::kAlive;
  }
  if (len == kDisconnectSize && header == kDisconnectHeader) {
    return RxKind::kDisconnect;
  }
  return RxKind::kDrop;
}

// UNBOND tag builder lives in pairing_codec.h (single owner); re-exported
// here for the demux/handlers and the host tests pinning udpstatus:: names.
using pairing::unbondTagMsg;

// Wrap-aware strictly-greater for the DRIVE_V2 32-bit BE counter (starts at
// 1; initial lastSeen=0). Unsigned subtraction throughout: wrap-safe across
// the 2^32 rollover (same idiom as coordinator/pairing millis math). A diff
// of exactly half the range is direction-ambiguous and rejected.
//
// NOTE (Tier-2 exhaustion gate): the fail-closed counter ceiling lives
// app-side only (client DriveCounter.EXHAUSTION_GATE = 0xFFFFFF00 — the app
// stops allocating and requires an explicit re-pair well before the wrap,
// so 0 and the wrap-adjacent region never reach this verify). No firmware
// ceiling here by design: rejecting >= gate on the wire would change verify
// behavior (and the host wrap tests below pin the full wrap-aware range).
// TODO: if the app-side gate ever proves unenforceable, revisit a firmware
// ceiling + coordinated re-pair signal — never silently extend the range.
inline bool isFreshCounter(uint32_t lastSeen, uint32_t got) {
  return got != lastSeen && (got - lastSeen) < 0x80000000u;
}

// Robot alive freshness: true while the last valid bonded packet (ALIVE,
// DRIVE_V2, or REKEY_REQ — all funnel through markAlive) is within kAliveMs.
// Unsigned subtraction: wrap-safe across the ~49-day millis() rollover
// (same idiom as isFreshCounter above). The boundary counts as fresh.
inline bool isAliveFresh(uint32_t lastAliveMs, uint32_t nowMs) { return (nowMs - lastAliveMs) <= kAliveMs; }

// Telemetry TX gate (host-testable; sendTelemetry calls this): unicast only
// when bonded AND a peer is pinned AND the alive window is fresh; else
// silent (no broadcast fallback — deleted). Pre-bond is always silent.
inline bool shouldSendTelemetry(bool bonded, bool havePeer, uint32_t lastAliveMs, uint32_t nowMs) {
  return bonded && havePeer && isAliveFresh(lastAliveMs, nowMs);
}

// PAIR_OK redelivery predicate (RAM-only cache in PairingTask::lastPair_).
// True when the bonded robot should re-unicast the cached PAIR_OK: the cache
// is valid, the incoming code (6B) matches with a no-early-exit loop, the
// incoming client nonce (4B) matches, and the bond is younger than
// kRedeliveryMs (wrap-safe millis math). Null pointers -> false. Deliberately
// local (no manager/task dependency) so this top half stays stdint-only.
inline bool shouldRedeliver(bool valid, const uint8_t* storedCode, const uint8_t* storedNonce, const uint8_t* inCode,
                            const uint8_t* inNonce, uint32_t bondMs, uint32_t nowMs) {
  if (!valid) {
    return false;
  }
  if (storedCode == nullptr || storedNonce == nullptr || inCode == nullptr || inNonce == nullptr) {
    return false;
  }
  uint8_t codeDiff = 0;
  for (size_t i = 0; i < 6; ++i) {
    codeDiff |= static_cast<uint8_t>(storedCode[i] ^ inCode[i]);
  }
  if (codeDiff != 0) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (storedNonce[i] != inNonce[i]) {
      return false;
    }
  }
  return (nowMs - bondMs) < kRedeliveryMs;
}

// QR pack bridge (host-testable): valid window packs via qr::packBitmap;
// closed-window/null never touches `out` (fail-closed). The firmware
// PairingTask::pumpQrLink() packs via qr::packBitmap directly (same pure
// function), so the valid→pack→pump bridge is unit-covered without ARDUINO
// through this helper.
inline bool packQrBitmap(bool valid, const uint8_t* modules, uint8_t* out) {
  if (!valid || modules == nullptr || out == nullptr) {
    return false;
  }
  qr::packBitmap(modules, out);
  return true;
}

}  // namespace udpstatus

// Platform split 1.1: the telemetry decision + encode core. Included here
// (right after the udpstatus single-owner definitions it builds on) so the
// firmware class below and host tests share one header. Host tests include
// this file first, then services/telemetry_manager.h directly.
#define UDPSTATUS_TOP_HALF_INCLUDED
#include "services/telemetry_manager.h"

#ifdef ARDUINO
// ── Firmware: UDP server task (WiFi + WiFiUDP, Core 0) ────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <mbedtls/platform_util.h>

#include <utility>

#include "abilities/pairing/pairing_task.h"
#include "abilities/sensors/sensors_manager.h"
#include "behavior/coordinator.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "services/event_bus.h"
#include "transport/udp_codec.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as coordinator.h).
#ifndef CFG_WIFI_AP_SSID
#define CFG_WIFI_AP_SSID "desky"
#endif
#ifndef CFG_WIFI_AP_PASS
#define CFG_WIFI_AP_PASS "desky1234"
#endif
#ifndef CFG_UDP_PORT
#define CFG_UDP_PORT 3333
#endif
#ifndef CFG_TELEMETRY_HZ
#define CFG_TELEMETRY_HZ 15
#endif
#ifndef CFG_UDP_STACK_WORDS
#define CFG_UDP_STACK_WORDS 4096
#endif
#ifndef CFG_UDP_PRIORITY_OFFSET
#define CFG_UDP_PRIORITY_OFFSET 3
#endif
#ifndef CFG_UDP_CORE
#define CFG_UDP_CORE 0
#endif
#ifndef CFG_UDP_LOOP_MS
#define CFG_UDP_LOOP_MS 5
#endif
#ifndef CFG_WIFI_STA_RETRY_MS
#define CFG_WIFI_STA_RETRY_MS 5000
#endif
#ifndef DESKY_WIFI_STA
#define DESKY_WIFI_STA 0
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// desky-sta requires real credentials from .env (build via make so the
// values arrive shell-quoted; see platformio.ini). An empty SSID means the
// env never reached the compiler — fail the build here with a message, not
// on-chip in a 5s reconnect loop. AP builds skip this (their empty defines
// are never referenced).
#if DESKY_WIFI_STA
static_assert(sizeof(WIFI_SSID) > 1, "desky-sta needs WIFI_SSID from .env (use make build-sta)");
#endif

class UdpServer {
 public:
  static constexpr uint16_t kPort = CFG_UDP_PORT;
  static constexpr uint32_t kStackWords = CFG_UDP_STACK_WORDS;
  // Below Coordinator +4 / Motion +5, above idle: comms must never preempt
  // the Core 1 real-time slot, and sits below WiFi internals.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_UDP_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_UDP_CORE;
  static constexpr uint32_t kLoopMs = CFG_UDP_LOOP_MS;
  static constexpr uint32_t kTelemetryMs = 1000 / CFG_TELEMETRY_HZ;
  static constexpr uint32_t kStaleMs = CFG_COORDINATOR_STALE_MS;
  static constexpr uint32_t kStaRetryMs = CFG_WIFI_STA_RETRY_MS;
  static constexpr bool kStaMode = DESKY_WIFI_STA != 0;

  UdpServer()
      : coord_(nullptr),
        tof_(nullptr),
        mpu_(nullptr),
        task_(nullptr),
        peerPort_(0),
        conn_(udpstatus::kAliveMs),
        telem_(kTelemetryMs, kStaleMs),
        lastRetryMs_(0) {}

  // Starts WiFi (async both modes), binds the UDP port, and pins the server
  // task to Core 0. Call once in setup(), AFTER Coordinator::begin (the RX
  // path publishes to the bus from its first datagram) with logging up.
  // Logs ONE BOOT line: wifi mode + ssid + ip + udp port (STA pre-link logs
  // "connecting…"; the task logs the DHCP IP when the link comes up).
  bool begin(Coordinator* coord, Vl53l0xDriver* tof, Mpu6500Driver* mpu, pairing::PairingTask* pairing) {
    DESKY_ASSERT(coord != nullptr);
    DESKY_ASSERT(tof != nullptr);
    DESKY_ASSERT(mpu != nullptr);
    DESKY_ASSERT(pairing != nullptr);
    if (coord == nullptr || tof == nullptr || mpu == nullptr || pairing == nullptr) {
      return false;
    }
    coord_ = coord;
    tof_ = tof;
    mpu_ = mpu;
    if (kStaMode) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      // Non-blocking: the task loop retries + reports the link. PASS never logged.
      const char* ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "connecting…";
      LOG_I("BOOT", "wifi STA ssid=%s ip=%s udp=%u", WIFI_SSID, ip, kPort);
    } else {
      WiFi.mode(WIFI_AP);
      const bool apOk = WiFi.softAP(CFG_WIFI_AP_SSID, CFG_WIFI_AP_PASS);
      DESKY_ASSERT(apOk);
      if (!apOk) {
        return false;
      }
      LOG_I("BOOT", "wifi AP ssid=%s ip=%s udp=%u", CFG_WIFI_AP_SSID, WiFi.softAPIP().toString().c_str(), kPort);
    }
    const bool udpOk = udp_.begin(kPort);
    DESKY_ASSERT(udpOk);
    if (!udpOk) {
      return false;
    }
    // Pairing/auth v1 session lives in PairingTask (abilities/pairing): pin
    // the shared transport state and restore the bond before the task reads
    // the radio. ConfigStore::begin() already ran in setup(). The
    // coordinator's drive handler authenticates through the same session,
    // so pin it here too (UdpServer owns both pointers at this point).
    pairing_ = pairing;
    pairing_->attach(&conn_, &peerIp_, &peerPort_);
    pairing_->restoreFromStore();
    coord_->attachPairing(pairing);
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&UdpServer::taskEntry, "udp", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // ── Transport back-call API (Slice 6b + Slice 7) ─────────────────────────
  // Called by PairingTask handlers (pair/unbond/rekey) and the Coordinator
  // drive handler via the duck-typed `Transport& t` param: byte composition
  // lives in the ability/behavior, the gate-check + unicast stays here.
  // Public by necessity (template back-calls from pairing_task.h and
  // coordinator.h); nothing else calls these.

  // Robot alive funnel (single call site for every VALID bonded packet):
  // pins the telemetry unicast peer AND refreshes the alive timestamp.
  // Called for ALIVE, DRIVE_V2, and REKEY_REQ after verify passes and BEFORE
  // any opcode-specific effect (event publish, counter persist, ACK) — no
  // opcode holds a privilege here: a 1Hz ALIVE keeps the robot alive exactly
  // like 50Hz drive. DISCONNECT/UNBOND never call this: they run
  // refresh-then-clear instead (stamp refreshed, net peer dropped).
  // Platform split 1.1: consumes the Verified token (proves AuthVerifier ran
  // AFTER the HMAC) and returns the Pinned token the drive publish / rekey
  // ACK require — effect-before-verify no longer compiles. Invalid token ->
  // no state change (fail-closed). Peer slot + stamp live in conn_; the
  // IPAddress pin below is the socket residual (documented on conn_).
  // Slice 6b: the session era moved to PairingTask, so the re-pin log takes
  // it as a parameter (log line otherwise unchanged).
  connection::Pinned markAlive(connection::Verified&& verified, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                               uint16_t era) {
    // Correlation marker (log-only, no behavior change): re-pin changes are
    // rare (AP rejoin/DHCP), so log the new peer once per change; steady
    // 50 Hz traffic re-pins the same IP/port and stays silent. IP+era only —
    // never key/nonce material.
    const bool repinned = !conn_.havePeer() || !(peerIp_ == rip) || peerPort_ != rport;
    connection::Pinned pinned =
        conn_.markAlive(std::move(verified), connection::ipToU32(rip[0], rip[1], rip[2], rip[3]), nowMs);
    if (!pinned.valid()) {
      return pinned;
    }
    peerIp_ = rip;
    peerPort_ = rport;
    if (repinned) {
      LOG_I("UDP", "peer re-pinned ip=%s era=%u", rip.toString().c_str(), static_cast<unsigned>(era));
    }
    return pinned;
  }

  // Drive effect (Slice 6b drive split, Slice 7 transport half): runs ONLY
  // on a valid Pinned token (invalid -> silent no-op, fail-closed) — the
  // effect-before-verify proof survives the split: callers cannot publish
  // without the markAlive funnel. Publishes the coordinator command (never
  // deduped — the coordinator resume edge needs every fresh command); the
  // RX-observation triple left for services/TelemetryManager (fed via
  // noteDriveCommand below). The failsafe policy itself stays in
  // behavior/coordinator.h.
  void publishDrive(connection::Pinned pinned, uint32_t nowMs, uint8_t mode, uint8_t thr, uint8_t str, uint8_t flags) {
    if (!pinned.valid()) {
      return;
    }
    (void)nowMs;
    EventBus::publish(EVENT_UDP_COMMAND_RECEIVED, coordinator::packCommand(mode, thr, str, flags));
  }

  // Telemetry RX observation passthrough (Slice 7): the coordinator drive
  // handler reports every verified frame here; the triple lives in telem_
  // (services/TelemetryManager), never in transport or behavior.
  void noteDriveCommand(float v, float omega, uint32_t nowMs) { telem_.noteCommand(v, omega, nowMs); }

  // PAIR_OK redelivery unicast (lost-PAIR_OK race): the socket-send half of
  // the PairingTask redelivery check — failure accounting and logs identical
  // to the inline block it was extracted from (member->access change only).
  void sendPairRedeliver(const IPAddress& ip, uint16_t port, const uint8_t (&ok39)[pairing::kPairOkSize]) {
    if (!udpsend::udpSendChecked(udp_, ip, port, ok39, sizeof(ok39))) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_redeliver", static_cast<unsigned>(sizeof(ok39)));
    } else {
      LOG_I("UDP", "pair redeliver");
    }
  }

  // REKEY_ACK 10B [0x54|eraBE2|tag6|XOR] unicast with
  // tag = first6(HMAC-SHA256(newPsk, "REKEYACK"||eraBE2)). Composed by
  // PairingTask (psk_ lives there since Slice 6b) and passed in as bytes.
  // Platform split 1.1: the token-gated sendRekeyAck below is the only path
  // the verified switch takes (consumes Pinned by value — proves the
  // markAlive funnel ran BEFORE the ACK). The idempotent duplicate path
  // calls the Impl directly (no verify by design — the ACK leaks no secret).
  // Key material never logged.
  void sendRekeyAck(connection::Pinned pinned, const IPAddress& ip, uint16_t port,
                    const uint8_t (&ack10)[pairing::kRekeyAckSize]) {
    if (!pinned.valid()) {
      return;
    }
    sendRekeyAckImpl(ip, port, ack10);
  }

  void sendRekeyAckImpl(const IPAddress& ip, uint16_t port, const uint8_t (&ack10)[pairing::kRekeyAckSize]) {
    if (!udpsend::udpSendChecked(udp_, ip, port, ack10, sizeof(ack10))) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "rekey_ack", static_cast<unsigned>(sizeof(ack10)));
    }
  }

  // PAIR_ERR 4B unicast ([0x52|reason|retryS|XOR], composed by PairingTask):
  // failure replies ride no token gate (pre-verify paths by construction).
  void sendPairErr(const IPAddress& ip, uint16_t port, const uint8_t (&err4)[4]) {
    if (!udpsend::udpSendChecked(udp_, ip, port, err4, sizeof(err4))) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_err", static_cast<unsigned>(sizeof(err4)));
    }
  }

  // PAIR_OK 39B unicast: consumes the PersistReceipt by value (moved) —
  // proves ConfigStore::saveBond persisted BEFORE anything left the radio
  // (store-before-send contract). Invalid receipt -> silent no-op.
  void sendPairOk(connection::PersistReceipt receipt, const IPAddress& ip, uint16_t port,
                  const uint8_t ok39[pairing::kPairOkSize]) {
    if (!receipt.valid()) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, ip, port, ok39, pairing::kPairOkSize)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_ok", static_cast<unsigned>(pairing::kPairOkSize));
    }
  }

  // udpstatus predicate bridges (single owner stays in the top half):
  // the PairingTask handlers verify through these so this header keeps the
  // only textual dependency on the udpstatus names.
  bool isFreshCounter(uint32_t lastSeen, uint32_t got) { return udpstatus::isFreshCounter(lastSeen, got); }

  bool shouldRedeliver(bool valid, const uint8_t* storedCode, const uint8_t* storedNonce, const uint8_t* inCode,
                       const uint8_t* inNonce, uint32_t bondMs, uint32_t nowMs) {
    return udpstatus::shouldRedeliver(valid, storedCode, storedNonce, inCode, inNonce, bondMs, nowMs);
  }

  bool packQrBitmap(bool valid, const uint8_t* modules, uint8_t* out) {
    return udpstatus::packQrBitmap(valid, modules, out);
  }

  // Drive-drop first-of-boot log via the transport-owned firstDriveDrop_
  // flag (the flag stays here with the retained dropFirst infra; the moved
  // handleDriveV2 logs through this bridge).
  template <typename... Args>
  void driveDropFirst(const char* fmt, Args... args) {
    dropFirst(firstDriveDrop_, fmt, args...);
  }

  // Send-failure accounting: every UDP unicast goes through
  // udpsend::udpSendChecked; failures bump txFail_ + LOG_W (release-visible).
  // Benign tear-tolerant read like the LinkServer counters; task only.
  uint32_t txFail() const { return txFail_; }

 private:
  static void taskEntry(void* arg) { static_cast<UdpServer*>(arg)->loop(); }

  void loop() {
    DESKY_ASSERT(coord_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    uint32_t lastTxMs = 0;
    bool linkLogged = false;
    // Never returns / never halts by design (see coordinator.h: a halted
    // WDT-subscribed task becomes a panic-reboot loop).
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();

      if (kStaMode) {
        if (WiFi.status() != WL_CONNECTED) {
          // Unsigned subtraction: wrap-safe across millis() rollover.
          if (nowMs - lastRetryMs_ >= kStaRetryMs) {
            lastRetryMs_ = nowMs;
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            LOG_D("UDP", "STA reconnect ssid=%s", WIFI_SSID);
          }
        } else if (!linkLogged) {
          linkLogged = true;
          LOG_I("UDP", "STA link up ip=%s udp=%u", WiFi.localIP().toString().c_str(), kPort);
        }
      }

      // RX: drain every pending datagram; demux by (len, header) + XOR FIRST
      // (cheap), then per-kind handling. Each VALID DRIVE_V2 publishes ONE
      // event (never deduped — the coordinator resume rule needs the fresh
      // nonzero edge). Everything else drops with throttled logging.
      int pending = 0;
      while ((pending = udp_.parsePacket()) > 0) {
        const IPAddress rip = udp_.remoteIP();
        const uint16_t rport = udp_.remotePort();
        uint8_t buf[20];  // 16B DRIVE_V2 + slack so oversize is detectable.
        const int got = udp_.read(buf, sizeof(buf));
        if (got <= 0) {
          dropFirst(firstMalformedDrop_, "drop datagram pending=%d got=%d (first of boot)", pending, got);
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
          continue;
        }
        const size_t ulen = static_cast<size_t>(got);
        const udpstatus::RxKind kind = udpstatus::classifyRx(ulen, buf[0]);
        if (kind == udpstatus::RxKind::kDrop) {
          dropFirst(firstMalformedDrop_, "drop datagram pending=%d got=%d (first of boot)", pending, got);
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
          continue;
        }
        // XOR checksum over bytes 0..len-2 must equal the last byte.
        uint8_t xorSum = 0;
        for (size_t i = 0; i + 1 < ulen; ++i) {
          xorSum ^= buf[i];
        }
        if (xorSum != buf[ulen - 1]) {
          dropFirst(firstMalformedDrop_, "drop datagram pending=%d got=%d (first of boot)", pending, got);
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
          continue;
        }
        switch (kind) {
          case udpstatus::RxKind::kLegacy:
            handleLegacy(rip, rport);
            break;
          case udpstatus::RxKind::kDriveV2:
            // Slice 7: the coordinator owns drive intent; the token-gated
            // drive effects (EventBus publish + telemetry note) run through
            // the transport back-calls inside the handler, which require the
            // Pinned proof — effect-before-verify still fails to compile.
            coord_->handleDriveV2(buf, rip, rport, nowMs, *this);
            break;
          case udpstatus::RxKind::kPairReq:
            pairing_->handlePairReq(buf, rip, rport, nowMs, *this);
            break;
          case udpstatus::RxKind::kUnbond:
            pairing_->handleUnbond(buf, rip, rport, nowMs, *this);
            break;
          case udpstatus::RxKind::kRekeyReq:
            pairing_->handleRekey(buf, rip, rport, nowMs, *this);
            break;
          case udpstatus::RxKind::kAlive:
            handleAlive(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kDisconnect:
            handleDisconnect(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kDrop:
            dropFirst(firstMalformedDrop_, "drop datagram pending=%d got=%d (first of boot)", pending, got);
            LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
            break;
        }
      }

      // TX: telemetry at CFG_TELEMETRY_HZ from the live snapshot.
      if (nowMs - lastTxMs >= kTelemetryMs) {
        lastTxMs = nowMs;
        sendTelemetry(nowMs);
      }

      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000 / kLoopMs) == 0) {
        Diagnostics::logWatermarks("UDP");
      }
    }
  }

  // Security-drop logging (bench lesson, Task 10): LOG_EVERY_N alone hides
  // the first N-1 occurrences per call site, which blinded the bench run.
  // First occurrence per boot per class always logs at INFO; afterwards the
  // call-site LOG_EVERY_N throttle applies. No key/code/nonce material here.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("UDP", fmt, args...);
    }
  }

  // ALIVE layout [0x55|eraBE2 1..2|ctrBE4 3..6|tag6 7..12|XOR] (len +
  // header + XOR checked by the caller). Keepalive under the bonded PSK:
  // Slice 7 transport-owned (collapsed from PairingTask::handleAlive):
  // verify order mirrors drive (bonded? -> era is current -> counter fresh
  // -> atomic tryConsume). Pass refreshes the alive funnel WITHOUT any
  // drive effect. Fail = silent drop. Pre-bond ALIVEs drop like pre-bond
  // drive; old-key frames die via HMAC. The bonded/era/fresh reads are
  // LOGGING-ONLY pre-checks preserving the per-cause drops; tryConsume is
  // the AUTHORITATIVE gate (same thread, no interleaving).
  void handleAlive(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    DESKY_ASSERT(pairing_ != nullptr);
    if (buf == nullptr || pairing_ == nullptr) {
      return;
    }
    if (!pairing_->bonded()) {
      dropFirst(firstAliveDrop_, "drop alive unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop alive unbonded");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era != pairing_->era()) {
      dropFirst(firstAliveDrop_, "drop alive era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(pairing_->era()));
      LOG_EVERY_N(20, "UDP", "drop alive era=%u have=%u", static_cast<unsigned>(era),
                  static_cast<unsigned>(pairing_->era()));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!isFreshCounter(pairing_->lastCounter(), ctr)) {
      dropFirst(firstAliveDrop_, "drop alive replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop alive replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "ALIVE"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kAliveMsgLen];
    pairing::aliveTagMsg(era, ctr, msg);
    connection::Verified verified = pairing_->tryConsume(ctr, era == pairing_->era(), buf + 7, msg, sizeof(msg));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    if (!verified.valid()) {
      dropFirst(firstAliveDrop_, "drop alive auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop alive auth");
      return;
    }
    // Shared counter: a valid ALIVE consumes exactly like drive (same
    // holder app-side), so a replayed ALIVE can never refresh twice.
    // Alive only: no telemetry note, no publish (Pinned needs no effect).
    markAlive(std::move(verified), rip, rport, nowMs, pairing_->era());
  }

  // DISCONNECT layout [0x56|eraBE2 1..2|ctrBE4 3..6|tag6 7..12|XOR] (len +
  // header + XOR checked by the caller). Graceful goodbye under the bonded
  // PSK: Slice 7 transport-owned (collapsed from PairingTask::handleDisconnect):
  // verify mirrors ALIVE. Pass runs refresh-then-clear like UNBOND — the
  // signed goodbye proves the owner was alive at nowMs, then the net peer is
  // gone (conn_.havePeer() = false). Unlike UNBOND the bond is KEPT (no NVS/Core/
  // key clear, no pairing window): the next launch rejoins instantly.
  // Never drives. Fail = silent drop. Pre-bond DISCONNECTs drop like drive.
  void handleDisconnect(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    (void)rip;
    (void)rport;
    DESKY_ASSERT(buf != nullptr);
    DESKY_ASSERT(pairing_ != nullptr);
    if (buf == nullptr || pairing_ == nullptr) {
      return;
    }
    if (!pairing_->bonded()) {
      dropFirst(firstDisconnectDrop_, "drop disconnect unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop disconnect unbonded");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era != pairing_->era()) {
      dropFirst(firstDisconnectDrop_, "drop disconnect era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(pairing_->era()));
      LOG_EVERY_N(20, "UDP", "drop disconnect era=%u have=%u", static_cast<unsigned>(era),
                  static_cast<unsigned>(pairing_->era()));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!isFreshCounter(pairing_->lastCounter(), ctr)) {
      dropFirst(firstDisconnectDrop_, "drop disconnect replay ctr=%lu (first of boot)",
                static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop disconnect replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "DISCONNECT"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kDisconnectMsgLen];
    pairing::disconnectTagMsg(era, ctr, msg);
    connection::Verified verified = pairing_->tryConsume(ctr, era == pairing_->era(), buf + 7, msg, sizeof(msg));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    if (!verified.valid()) {
      dropFirst(firstDisconnectDrop_, "drop disconnect auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop disconnect auth");
      return;
    }
    // tryConsume consumed the counter (a replayed goodbye can never clear
    // twice) + ran the persist cadence — same as before.
    conn_.refreshThenClear(std::move(verified), nowMs);  // Refresh-then-clear: net gone, bond kept.
    LOG_I("UDP", "disconnect ok");
  }

  // Pre-bond pin-only: remember the peer for unicast replies (pair + tele-
  // metry) but NEVER publish to the coordinator — nothing drives pre-bond.
  // Post-bond legacy drive datagrams are dead: silent drop. Freshness
  // tracking (telem_.isDriving) is untouched on both paths, so the
  // telemetry driving bit never asserts pre-bond.
  void handleLegacy(const IPAddress& rip, uint16_t rport) {
    if (pairing_->bonded()) {
      dropFirst(firstLegacyDrop_, "drop legacy post-bond (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop legacy post-bond");
      return;
    }
    peerIp_ = rip;
    peerPort_ = rport;
    conn_.pinPeer(connection::ipToU32(rip[0], rip[1], rip[2], rip[3]));
  }

  void sendTelemetry(uint32_t nowMs) {
    // Coordinator-owned intent (mode/active/cliff level); defaults hold if
    // the snapshot ever fails (boot race): MANUAL, stopped, no cliff.
    SystemState snap;
    coord_->snapshot(snap);

    // Live sensors -> fusion on demand (same atan2f convention as fusion):
    // real tilt once the MPU is healthy (the sensor task polls it), 0 before.
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;
    const uint16_t distMm = tof_->distanceMm();
    if (mpu_->isHealthy()) {
      const MpuReading& r = mpu_->reading();
      fusion::SensorSnapshot fs;
      fs.ax = r.ax;
      fs.ay = r.ay;
      fs.az = r.az;
      fs.tofMm = distMm;
      fs.tofValid = true;
      fs.mpuHealthy = true;
      SystemState fused;
      fusion::Fusion{fusion::kBoardMounting}.evaluate(fs, fused);
      pitchDeg = fused.pitch;
      rollDeg = fused.roll;
    }

    // Driving = services-owned RX observation (last commanded stick nonzero
    // AND fresh): the coordinator drive handler feeds it via noteDriveCommand
    // on every verified frame; the coordinator owns no driving flag. The
    // cliff bit below mirrors the coordinator level, which the sensor task
    // keeps live.
    const bool driving = telem_.isDriving(nowMs);
    // Platform split 1.1: the aggregate snapshot fills here (verbatim
    // sources as above); pack + cadence/edge + encode live in
    // telemetry::TelemetryManager, the socket send stays here.
    telemetry::Snapshot tsnap;
    tsnap.mode = static_cast<uint8_t>(snap.mode);
    tsnap.cliff = snap.cliffDetected;
    tsnap.driving = driving;
    tsnap.pitchDeg = pitchDeg;
    tsnap.rollDeg = rollDeg;
    tsnap.distMm = distMm;
    udp::TelemetryPacket tp;
    telemetry::encodeSnapshot(tsnap, tp);

    // Telemetry gate: bonded && peer pinned && alive-fresh -> unicast;
    // else silent (broadcast fallback deleted). The peer stays pinned across
    // a stale window — the next ALIVE/drive re-freshes via markAlive (which
    // re-pins IP/port each time) with no re-registration. DISCONNECT/UNBOND
    // cleared the peer, so silence follows naturally there too. Gate
    // predicate single-owned by udpstatus::shouldSendTelemetry; logging here.
    const bool gateOk =
        udpstatus::shouldSendTelemetry(pairing_->bonded(), conn_.havePeer(), conn_.lastAliveMs(), nowMs);
    if (!gateOk) {
      // Correlation marker (log-only, no behavior change): the alive-stale
      // silence edge shares firstTelemetryDrop_ (one first-of-boot INFO) and
      // stays throttled afterwards — same pattern as the no-peer path.
      if (pairing_->bonded() && conn_.havePeer()) {
        dropFirst(firstTelemetryDrop_, "alive stale -> silent (first of boot)");
        LOG_EVERY_N(20, "UDP", "alive stale -> silent");
      } else {
        dropFirst(firstTelemetryDrop_, "drop telemetry no-peer/stale (first of boot)");
        LOG_EVERY_N(20, "UDP", "drop telemetry no-peer/stale");
      }
      return;
    }
    telem_.publish(tp, gateOk, nowMs, [this](const uint8_t* buf, size_t len) {
      if (!udpsend::udpSendChecked(udp_, peerIp_, peerPort_, buf, len)) {
        ++txFail_;
        LOG_W("UDP", "udp send fail what=%s len=%u", "telemetry", static_cast<unsigned>(len));
      }
    });
  }

  Coordinator* coord_;
  Vl53l0xDriver* tof_;
  Mpu6500Driver* mpu_;
  TaskHandle_t task_;
  WiFiUDP udp_;
  IPAddress peerIp_;
  uint16_t peerPort_;
  // Robot alive rule (platform split 1.1): the peer slot + alive window live
  // in conn_ (connection::ConnectionManager, Arduino-free core, host-pinned;
  // window = udpstatus::kAliveMs passthrough — the 5s literal is NEVER
  // repeated here). Refreshed by the markAlive funnel on every VALID bonded
  // packet (ALIVE, DRIVE_V2, REKEY_REQ); DISCONNECT/UNBOND refresh then drop
  // the net peer (havePeer() = false). Fresh while
  // udpstatus::isAliveFresh(lastAliveMs, nowMs) (kAliveMs = 5s window).
  // Never touched by ALIVE's no-drive path beyond the funnel itself, and
  // never by legacy/pair traffic. peerIp_/peerPort_ stay here (residual):
  // WiFiUDP::beginPacket needs an IPAddress; the u32 in conn_ is the
  // portable identity (adapter: connection::ipToU32/u32ToIp).
  connection::ConnectionManager conn_;
  // Telemetry cadence + encode core + RX observation
  // (services/telemetry_manager.h): the bonded-default rate is kTelemetryMs
  // passthrough and the stale window is kStaleMs passthrough (no new
  // logic — construction takes both, same pattern); the socket + gate
  // logging stay here in sendTelemetry.
  telemetry::TelemetryManager telem_;
  uint32_t lastRetryMs_;
  // Pairing/auth session (abilities/pairing, Slice 6b): owns bonded_/psk_/
  // counter/era/validCount_/pairCore_/LastPair/QR/dropFirst-for-moved-paths.
  // Set in begin() before the task reads the radio; never null afterwards.
  pairing::PairingTask* pairing_ = nullptr;
  // Checked-send failures (udpsend::udpSendChecked false): rare/backpressure
  // class, WARNed at every occurrence (release-visible). Task-written, benign
  // tear-tolerant read via txFail().
  uint32_t txFail_ = 0;
  // Cached QR bitmap of the CURRENT open window's code lives in PairingTask
  // (encoded on every window open); the transport never touches it.
  // First-drop flags for dropFirst() (bench lesson: first occurrence per
  // boot per class always logs). Reset only by reboot — never per window.
  // (firstPairDrop_/firstUnbondDrop_/firstRekeyDrop_ stay in PairingTask with
  // the handlers that log them; firstDriveDrop_ is logged through the
  // driveDropFirst bridge for the coordinator-owned drive handler.)
  bool firstMalformedDrop_ = true;
  bool firstLegacyDrop_ = true;
  bool firstDriveDrop_ = true;
  bool firstTelemetryDrop_ = true;
  bool firstAliveDrop_ = true;
  bool firstDisconnectDrop_ = true;
};

#endif  // ARDUINO
