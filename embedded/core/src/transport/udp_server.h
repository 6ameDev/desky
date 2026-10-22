#pragma once
// UDP server — header-only. Owns dual-mode WiFi + the binary
// control/telemetry sockets (wire format in transport/udp_codec.h) + one
// Core 0 FreeRTOS task. Control RX publishes EVENT_UDP_COMMAND_RECEIVED per
// datagram; telemetry TX is a gated unicast of consumer-composed bytes
// (snapshot + encode + pacing live in services/TelemetryTask).
//
// Top half (namespace udpstatus) is Arduino-free byte math (stdint only) so
// host tests include this header directly. Bottom half (class UdpServer) is
// firmware-only (#ifdef ARDUINO).
//
// WiFi (one decision at boot): default AP "desky" 192.168.4.1;
// DESKY_WIFI_STA=1 joins the home router via .env WIFI_SSID/WIFI_PASS (PASS
// never logged). Both modes async: setup never blocks; the task loop retries
// WiFi.begin() every CFG_WIFI_STA_RETRY_MS and logs the DHCP IP once.
//
// Telemetry source (services/TelemetryTask, not here): mode/active/cliff from
// Coordinator::snapshot(); distanceMM from ToF live; pitch/roll from fusion
// over the live MPU reading; isDriving from TelemetryManager::isDriving.
// This class only gates (bonded + peer pinned + alive-fresh) and unicasts the
// bytes handed to sendTelemetry. Unicast-only to the last alive peer; silent
// otherwise (no broadcast fallback, pre-bond silent).
//
// Rules: no heap/String in RX/TX path (fixed stack buffers); WDT fed every
// tick; logWatermarks("UDP") every ~5s; task never halts. Key, code and nonce
// bytes are never logged; drops log first-of-boot at INFO then throttled.

// ── Arduino-free: statusFlags bit map + fixed-point helpers ──────────────
#include <stddef.h>
#include <stdint.h>

#include "common/udp_send.h"
#include "link/qr_codec.h"
#include "transport/connection_manager.h"
#include "transport/udp_codec.h"

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
// literals below equal udp::kControlHeader/kControlSize (0xAA/6) but stay
// literal; pairing constants alias udp:: (single home in udp_codec.h).
constexpr uint8_t kDriveV2Header = 0xAC;
constexpr size_t kDriveV2Size = 16;
// Pairing demux spellings alias the frozen phone-wire table in
// transport/udp_codec.h (same layer, no abilities header needed).
constexpr uint8_t kPairReqHeader = udp::kPairReqHeader;
constexpr size_t kPairReqSize = udp::kPairReqSize;
constexpr uint8_t kUnbondHeader = udp::kUnbondHeader;
constexpr size_t kUnbondSize = udp::kUnbondSize;
constexpr size_t kUnbondTagLen = udp::kUnbondTagLen;
constexpr size_t kUnbondMsgLen = udp::kUnbondMsgLen;
// Tier-1 silent auto-rekey: REKEY_REQ 14B, REKEY_ACK 10B (TX only).
// Headers 0x53/0x54 are claimed in transport/udp_codec.h — never reuse 0xAA/
// 0xAC/0xBB/0x50/0x51/0x52/0xAD (Phase-2 SET_RATE must pick other values).
constexpr uint8_t kRekeyReqHeader = udp::kRekeyReqHeader;
constexpr size_t kRekeyReqSize = udp::kRekeyReqSize;
constexpr uint8_t kRekeyAckHeader = udp::kRekeyAckHeader;
constexpr size_t kRekeyAckSize = udp::kRekeyAckSize;
// ALIVE + DISCONNECT: ALIVE 14B, DISCONNECT 14B.
constexpr uint8_t kAliveHeader = udp::kAliveHeader;
constexpr size_t kAliveSize = udp::kAliveSize;
constexpr uint8_t kDisconnectHeader = udp::kDisconnectHeader;
constexpr size_t kDisconnectSize = udp::kDisconnectSize;
// Robot alive freshness window: a bonded peer counts as alive while its last
// valid packet is within kAliveMs. DISCONNECT/UNBOND clear the net peer
// immediately, so the window only matters for silent loss.
constexpr uint32_t kAliveMs = 5000;
constexpr uint32_t kRedeliveryMs = CFG_PAIR_REDELIVERY_MS;

// RX demux: classifyRx maps (len, header) -> kind; anything else is kDrop.
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

// Wrap-aware strictly-greater for the DRIVE_V2 32-bit BE counter (starts at
// 1; initial lastSeen=0). Unsigned subtraction: wrap-safe across the 2^32
// rollover. A diff of exactly half the range is rejected as ambiguous.
inline bool isFreshCounter(uint32_t lastSeen, uint32_t got) {
  return got != lastSeen && (got - lastSeen) < 0x80000000u;
}

// Robot alive freshness: true while the last valid bonded packet is within
// kAliveMs. Unsigned subtraction: wrap-safe across millis() rollover.
inline bool isAliveFresh(uint32_t lastAliveMs, uint32_t nowMs) { return (nowMs - lastAliveMs) <= kAliveMs; }

// Telemetry TX gate: unicast only when bonded AND peer pinned AND alive-fresh;
// else silent. Pre-bond always silent.
inline bool shouldSendTelemetry(bool bonded, bool havePeer, uint32_t lastAliveMs, uint32_t nowMs) {
  return bonded && havePeer && isAliveFresh(lastAliveMs, nowMs);
}

// PAIR_OK redelivery predicate (RAM-only cache in PairingTask::lastPair_).
// True when the cache is valid, code (6B) and client nonce (4B) match, and
// the bond is younger than kRedeliveryMs (wrap-safe). Nulls -> false.
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

// QR pack bridge: valid window packs via qr::packBitmap; closed-window/null
// never touches `out`.
inline bool packQrBitmap(bool valid, const uint8_t* modules, uint8_t* out) {
  if (!valid || modules == nullptr || out == nullptr) {
    return false;
  }
  qr::packBitmap(modules, out);
  return true;
}

}  // namespace udpstatus

// Order pin for abilities/telemetry/telemetry_manager.h (consumers include udp_server.h first).
#define UDPSTATUS_TOP_HALF_INCLUDED

#ifdef ARDUINO
// ── Firmware: UDP server task (WiFi + WiFiUDP, Core 0) ────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <mbedtls/platform_util.h>

#include <utility>

#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "transport/pairing_session.h"
#include "transport/udp_codec.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
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

class UdpServer : public transport::IUdpTransport {
 public:
  static constexpr uint16_t kPort = CFG_UDP_PORT;
  static constexpr uint32_t kStackWords = CFG_UDP_STACK_WORDS;
  // Below Coordinator +4 / Motion +5, above idle: comms must never preempt
  // the Core 1 real-time slot, and sits below WiFi internals.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_UDP_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_UDP_CORE;
  static constexpr uint32_t kLoopMs = CFG_UDP_LOOP_MS;
  // Telemetry cadence + RX-observation windows, single-sourced here for the
  // services consumer (passthrough only).
  static constexpr uint32_t kTelemetryMs = 1000 / CFG_TELEMETRY_HZ;
  static constexpr uint32_t kStaleMs = CFG_COORDINATOR_STALE_MS;
  static constexpr uint32_t kStaRetryMs = CFG_WIFI_STA_RETRY_MS;
  static constexpr bool kStaMode = DESKY_WIFI_STA != 0;

  UdpServer() : task_(nullptr), peerPort_(0), conn_(udpstatus::kAliveMs), lastRetryMs_(0) {}

  // Starts WiFi (async both modes), binds the UDP port, and pins the server
  // task to Core 0. Call once in setup() with logging up. Logs ONE BOOT
  // line: wifi mode + ssid + ip + udp port.
  bool begin() {
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
    // The pairing session is wired via attachPairing() below; the coordinator
    // subscribes at its own begin, so no ordering pin is needed here.
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&UdpServer::taskEntry, "udp", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // Pins the transport-owned shared state (peer slot + alive window) into the
  // pairing session and restores the bond. Call once from setup() right after
  // begin(), before the radio sees traffic. Every pairing_ read on the RX
  // path is null-safe until this lands.
  void attachPairing(transport::IPairingSession* pairing) {
    DESKY_ASSERT(pairing != nullptr);
    if (pairing == nullptr) {
      return;
    }
    pairing_ = pairing;
    pairing_->attach(&conn_, &peerIp_, &peerPort_);
    pairing_->restoreFromStore();
  }

  // Command-publish hook: the verified-drive fan-out without naming the bus.
  // Registered once from setup() before begin(); invoked synchronously for
  // every verified drive frame at the same point the direct publish ran.
  // Null until set (fail-closed skip).
  using CommandPublishFn = void (*)(uint32_t payload);
  void setCommandPublisher(CommandPublishFn fn) { publishCmd_ = fn; }

  // ── Transport back-call API (transport::IUdpTransport) ──────────
  // Called by the PairingTask handlers through the seam interface:
  // byte composition lives in the ability, gate-check + unicast stays here.

  // Alive funnel for every VALID bonded packet: pins the telemetry unicast
  // peer AND refreshes the alive timestamp. Called for ALIVE, DRIVE_V2, and
  // REKEY_REQ after verify passes and BEFORE any opcode-specific effect.
  // DISCONNECT/UNBOND never call this: they refresh-then-clear instead.
  // Consumes the Verified token (proves the HMAC ran) and returns the Pinned
  // token. Invalid token -> no state change.
  connection::Pinned markAlive(connection::Verified&& verified, const IPAddress& rip, uint16_t rport, uint32_t nowMs,
                               uint16_t era) override {
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

  // Telemetry TX gate: bonded && peer pinned && alive-fresh -> unicast; else
  // silent. The peer stays pinned across a stale window; the next valid
  // packet re-freshes via markAlive. Gate predicate: shouldSendTelemetry.
  bool telemetryGate(uint32_t nowMs) {
    DESKY_ASSERT(pairing_ != nullptr);
    if (pairing_ == nullptr) {
      return false;
    }
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
    }
    return gateOk;
  }

  // Dumb telemetry unicast: bytes arrive composed; this only unicasts to the
  // last alive peer. Call only when telemetryGate passed.
  void sendTelemetry(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len == 0) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, peerIp_, peerPort_, buf, len)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "telemetry", static_cast<unsigned>(len));
    }
  }

  // PAIR_OK redelivery unicast (lost-PAIR_OK race): failure accounting and
  // logs identical to the other senders.
  void sendPairRedeliver(const IPAddress& ip, uint16_t port, const uint8_t* ok, size_t len) override {
    if (!udpsend::udpSendChecked(udp_, ip, port, ok, len)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_redeliver", static_cast<unsigned>(len));
    } else {
      LOG_I("UDP", "pair redeliver");
    }
  }

  // REKEY_ACK 10B unicast, composed by PairingTask (psk_ lives there) and
  // passed in as bytes. The token-gated sendRekeyAck is the only path the
  // verified switch takes (consumes Pinned by value). The idempotent
  // duplicate path calls the Impl directly (the ACK leaks no secret).
  void sendRekeyAck(connection::Pinned pinned, const IPAddress& ip, uint16_t port, const uint8_t* ack,
                    size_t len) override {
    if (!pinned.valid()) {
      return;
    }
    sendRekeyAckImpl(ip, port, ack, len);
  }

  void sendRekeyAckImpl(const IPAddress& ip, uint16_t port, const uint8_t* ack, size_t len) override {
    if (!udpsend::udpSendChecked(udp_, ip, port, ack, len)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "rekey_ack", static_cast<unsigned>(len));
    }
  }

  // PAIR_ERR 4B unicast (composed by PairingTask): failure replies ride no
  // token gate (pre-verify paths).
  void sendPairErr(const IPAddress& ip, uint16_t port, const uint8_t* err, size_t len) override {
    if (!udpsend::udpSendChecked(udp_, ip, port, err, len)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_err", static_cast<unsigned>(len));
    }
  }

  // PAIR_OK 39B unicast: consumes the PersistReceipt by value — proves the
  // bond persisted BEFORE anything left the radio. Invalid receipt -> silent
  // no-op.
  void sendPairOk(connection::PersistReceipt receipt, const IPAddress& ip, uint16_t port, const uint8_t* ok,
                  size_t len) override {
    if (!receipt.valid()) {
      return;
    }
    if (!udpsend::udpSendChecked(udp_, ip, port, ok, len)) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_ok", static_cast<unsigned>(len));
    }
  }

  // udpstatus predicate bridges (single owner stays in the top half):
  // the PairingTask handlers verify through these so this header keeps the
  // only textual dependency on the udpstatus names.
  bool isFreshCounter(uint32_t lastSeen, uint32_t got) override { return udpstatus::isFreshCounter(lastSeen, got); }

  bool shouldRedeliver(bool valid, const uint8_t* storedCode, const uint8_t* storedNonce, const uint8_t* inCode,
                       const uint8_t* inNonce, uint32_t bondMs, uint32_t nowMs) override {
    return udpstatus::shouldRedeliver(valid, storedCode, storedNonce, inCode, inNonce, bondMs, nowMs);
  }

  // Send-failure accounting: every UDP unicast goes through
  // udpsend::udpSendChecked; failures bump txFail_ + LOG_W.
  uint32_t txFail() const { return txFail_; }

 private:
  static void taskEntry(void* arg) { static_cast<UdpServer*>(arg)->loop(); }

  void loop() {
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    bool linkLogged = false;
    // Never returns / never halts: a halted WDT-subscribed task becomes a
    // panic-reboot loop.
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

      // RX: drain every pending datagram; demux by (len, header) + XOR first,
      // then per-kind handling. Each valid DRIVE_V2 publishes ONE event.
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
            handleDrive(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kPairReq:
          case udpstatus::RxKind::kUnbond:
          case udpstatus::RxKind::kRekeyReq:
            // Pair-family dispatch owned by the pairing ability. Null-safe
            // for the begin/attachPairing setup micro-window.
            if (pairing_ != nullptr) {
              pairing_->handlePairFrame(buf[0], buf, rip, rport, nowMs, *this);
            }
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

      // TX: telemetry is composed + paced by the services consumer through
      // sendTelemetry() below — no snapshot reads here.

      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000 / kLoopMs) == 0) {
        Diagnostics::logWatermarks("UDP");
      }
    }
  }

  // First occurrence per boot per class always logs at INFO; afterwards the
  // call-site LOG_EVERY_N throttle applies. No key/code/nonce material here.
  template <typename... Args>
  void dropFirst(bool& first, const char* fmt, Args... args) {
    if (first) {
      first = false;
      LOG_I("UDP", fmt, args...);
    }
  }

  // DRIVE_V2 layout [0xAC|mode|thr|str|flags|ctrBE 4B|tag6|XOR] (len +
  // header + XOR checked by the caller). Verify order: bonded? -> counter
  // fresh -> atomic tryConsume. Pass runs the markAlive funnel, then
  // publishes ONE EVENT_UDP_COMMAND_RECEIVED. Fail = silent drop. Pre-bond
  // drive drops like pre-bond ALIVE. The bonded/fresh reads are logging-only
  // pre-checks; tryConsume is the authoritative gate.
  void handleDrive(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    DESKY_ASSERT(pairing_ != nullptr);
    if (buf == nullptr || pairing_ == nullptr) {
      return;
    }
    if (!pairing_->bonded()) {
      dropFirst(firstDriveDrop_, "drop drive unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop drive unbonded");
      return;
    }
    // Layout: mode=1 thr=2 str=3 flags=4 ctr=5..8 tag=9..14 xor=15.
    const uint32_t ctr = (static_cast<uint32_t>(buf[5]) << 24) | (static_cast<uint32_t>(buf[6]) << 16) |
                         (static_cast<uint32_t>(buf[7]) << 8) | static_cast<uint32_t>(buf[8]);
    if (!isFreshCounter(pairing_->lastCounter(), ctr)) {
      dropFirst(firstDriveDrop_, "drop drive replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop drive replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // HMAC msg (8B stack): counterBE||mode||thr||str||flags.
    uint8_t msg[8];
    msg[0] = buf[5];
    msg[1] = buf[6];
    msg[2] = buf[7];
    msg[3] = buf[8];
    msg[4] = buf[1];
    msg[5] = buf[2];
    msg[6] = buf[3];
    msg[7] = buf[4];
    // Atomic session gate: bonded + fresh + HMAC in one step. Consumes the
    // counter on success; zero state change on failure.
    connection::Verified verified = pairing_->tryConsume(ctr, true, buf + 9, msg, sizeof(msg));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    if (!verified.valid()) {
      dropFirst(firstDriveDrop_, "drop drive auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop drive auth");
      return;
    }
    // Alive funnel BEFORE the publish: a valid frame keeps the robot alive
    // exactly like ALIVE.
    markAlive(std::move(verified), rip, rport, nowMs, pairing_->era());
    DESKY_ASSERT(publishCmd_ != nullptr);
    if (publishCmd_ != nullptr) {
      publishCmd_(udp::packCommand(buf[1], buf[2], buf[3], buf[4]));
    }
  }

  // ALIVE layout [0x55|eraBE2|ctrBE4|tag6|XOR] (len + header + XOR checked by
  // the caller). Keepalive under the bonded PSK. Verify order mirrors drive
  // (bonded? -> era current -> counter fresh -> atomic tryConsume). Pass
  // refreshes the alive funnel with no drive effect. Fail = silent drop. The
  // bonded/era/fresh reads are logging-only pre-checks; tryConsume is the
  // authoritative gate.
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
    // Tag composition lives ability-side; the session folds the same
    // bonded/era/fresh/HMAC checks into one atomic step.
    connection::Verified verified = pairing_->tryConsumeAlive(era, ctr, buf + 7);
    if (!verified.valid()) {
      dropFirst(firstAliveDrop_, "drop alive auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop alive auth");
      return;
    }
    // A valid ALIVE consumes the shared counter exactly like drive, so a
    // replayed ALIVE can never refresh twice.
    markAlive(std::move(verified), rip, rport, nowMs, pairing_->era());
  }

  // DISCONNECT layout [0x56|eraBE2|ctrBE4|tag6|XOR] (len + header + XOR
  // checked by the caller). Graceful goodbye under the bonded PSK; verify
  // mirrors ALIVE. Pass runs refresh-then-clear: net peer gone, bond kept
  // (no key clear, no pairing window). Never drives. Fail = silent drop.
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
    // Tag composition lives ability-side; the session folds the same
    // bonded/era/fresh/HMAC checks into one atomic step.
    connection::Verified verified = pairing_->tryConsumeDisconnect(era, ctr, buf + 7);
    if (!verified.valid()) {
      dropFirst(firstDisconnectDrop_, "drop disconnect auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop disconnect auth");
      return;
    }
    // tryConsume consumed the counter (a replayed goodbye can never clear
    // twice). Refresh-then-clear: net gone, bond kept.
    conn_.refreshThenClear(std::move(verified), nowMs);
    LOG_I("UDP", "disconnect ok");
  }

  // Pre-bond pin-only: remember the peer for unicast replies but never
  // publish — nothing drives pre-bond. Post-bond legacy datagrams: drop.
  void handleLegacy(const IPAddress& rip, uint16_t rport) {
    DESKY_ASSERT(pairing_ != nullptr);
    if (pairing_ == nullptr) {
      return;  // begin/attachPairing setup micro-window: fail-closed drop.
    }
    if (pairing_->bonded()) {
      dropFirst(firstLegacyDrop_, "drop legacy post-bond (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop legacy post-bond");
      return;
    }
    peerIp_ = rip;
    peerPort_ = rport;
    conn_.pinPeer(connection::ipToU32(rip[0], rip[1], rip[2], rip[3]));
  }

  TaskHandle_t task_;
  WiFiUDP udp_;
  IPAddress peerIp_;
  uint16_t peerPort_;
  // Peer slot + alive window (connection::ConnectionManager, host-pinned).
  // Refreshed by the markAlive funnel on every valid bonded packet;
  // DISCONNECT/UNBOND refresh then drop the net peer. peerIp_/peerPort_ stay
  // here: WiFiUDP::beginPacket needs an IPAddress; the u32 in conn_ is the
  // portable identity.
  connection::ConnectionManager conn_;
  uint32_t lastRetryMs_;
  // Pairing/auth session: owns bonded/psk/counter/era. Pinned by
  // attachPairing(); every RX-path deref is null-safe until then. No
  // setBonded mirror: the session is the single source of truth.
  transport::IPairingSession* pairing_ = nullptr;
  // Verified-drive fan-out, registered by setup() before begin().
  CommandPublishFn publishCmd_ = nullptr;
  // Checked-send failures (udpSendChecked false), WARNed at every occurrence.
  uint32_t txFail_ = 0;
  // First-drop flags for dropFirst(): first occurrence per boot per class
  // always logs. Reset only by reboot.
  bool firstMalformedDrop_ = true;
  bool firstLegacyDrop_ = true;
  bool firstDriveDrop_ = true;
  bool firstTelemetryDrop_ = true;
  bool firstAliveDrop_ = true;
  bool firstDisconnectDrop_ = true;
};

#endif  // ARDUINO
