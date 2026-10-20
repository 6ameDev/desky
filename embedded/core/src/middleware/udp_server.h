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
//   isDriving = tracked LOCALLY from RX (last commanded v/omega nonzero AND
//     fresh within CFG_COORDINATOR_STALE_MS), not from the coordinator.
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

// PAIR_OK redelivery predicate (RAM-only cache in UdpServer::lastPair_).
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
// UdpServer::qrBitmapPacked() below delegates here so the valid→pack→stage
// bridge is unit-covered without ARDUINO.
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
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include <utility>

#include "abilities/pairing/pairing_task.h"
#include "behavior/coordinator.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "middleware/sensor_fusion.h"
#include "services/config_store.h"
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
        lastV_(0.0f),
        lastOmega_(0.0f),
        lastRxMs_(0),
        conn_(udpstatus::kAliveMs),
        telem_(kTelemetryMs),
        lastRetryMs_(0),
        pairCore_(),
        psk_{},
        lastCounter_(0),
        bonded_(false),
        era_(0),
        validCount_(0),
        maxVerifyUs_(0) {
    lastPair_.valid = false;
  }

  // Starts WiFi (async both modes), binds the UDP port, and pins the server
  // task to Core 0. Call once in setup(), AFTER Coordinator::begin (the RX
  // path publishes to the bus from its first datagram) with logging up.
  // Logs ONE BOOT line: wifi mode + ssid + ip + udp port (STA pre-link logs
  // "connecting…"; the task logs the DHCP IP when the link comes up).
  bool begin(Coordinator* coord, Vl53l0xDriver* tof, Mpu6500Driver* mpu) {
    DESKY_ASSERT(coord != nullptr);
    DESKY_ASSERT(tof != nullptr);
    DESKY_ASSERT(mpu != nullptr);
    if (coord == nullptr || tof == nullptr || mpu == nullptr) {
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
    // Pairing/auth v1: restore the bond before the task reads the radio.
    // Bonded -> DRIVE_V2 verifies against the stored PSK; unbonded -> open
    // a pairing window (the code LOG_I line IS the display for now).
    // ConfigStore::begin() already ran in setup(), so loadBond is safe here.
    // The PAIR_OK redelivery cache stays invalid across reboot on both paths
    // (RAM-only: a rebooted robot answers already_bonded until a fresh pair).
    lastPair_.valid = false;
    uint8_t loadedPsk[16] = {};
    uint32_t storedCtr = 0;
    bool storedBond = false;
    if (ConfigStore::loadBond(loadedPsk, storedCtr, storedBond) && storedBond) {
      bonded_ = true;
      lastCounter_ = storedCtr;
      era_ = ConfigStore::loadEra();
      for (size_t i = 0; i < sizeof(psk_); ++i) {
        psk_[i] = loadedPsk[i];
      }
      LOG_I("UDP", "bond loaded counter=%lu era=%u", static_cast<unsigned long>(storedCtr),
            static_cast<unsigned>(era_));
    } else {
      bonded_ = false;
      lastCounter_ = 0;
      era_ = 0;
      if (!openPairingWindow()) {
        LOG_W("UDP", "pair window open failed");
      }
    }
    mbedtls_platform_zeroize(loadedPsk, sizeof(loadedPsk));
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&UdpServer::taskEntry, "udp", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // Opens a pairing window showing a fresh esp_random 6-digit code (the
  // LOG_I code line IS the display for now). Refused while bonded (single
  // owner: clearBond() first — rotation never evicts). Public for the Task
  // 10 bench procedure (10s-hold factory reset path).
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  bool openPairingWindow() {
    if (bonded_) {
      return false;
    }
    uint8_t code[pairing::kCodeLen];
    for (size_t i = 0; i < pairing::kCodeLen; ++i) {
      code[i] = static_cast<uint8_t>('0' + (esp_random() % 10u));
    }
    if (!pairCore_.openWindow(millis(), code)) {
      return false;
    }
    LOG_I("UDP", "pair window open code=%c%c%c%c%c%c", code[0], code[1], code[2], code[3], code[4], code[5]);
    // QR bitmap for this window's code: encoded in ALL builds (release
    // included) into qrBitmap_ for future consumers (head link); the serial
    // row dump below stays DEBUG-only (Tier-A regression path, zero-cost in
    // release). No heap/String; fixed stack buffers only. Encode failure is
    // impossible for validated digits (openWindow above accepted them) but
    // the bitmap is still gated on success by construction.
    qrBitmapValid_ = qr::encode6Digits(code, qrBitmap_);
#if LOG_LEVEL <= LOG_LEVEL_DEBUG
    // Tier-A hex dump of the V1-L bitmap for this code, one `QR row=NN:HHHHHH`
    // line per row for the serial capture script. Same call site covers both
    // window-open paths (boot begin() + post-UNBOND reopen, both funnel here).
    if (qrBitmapValid_) {
      for (int y = 0; y < qr::kSize; ++y) {
        uint8_t row[qr::kRowBytes];
        qr::rowBytes(qrBitmap_, y, row);
        char hex[2 * qr::kRowBytes + 1];
        for (int i = 0; i < qr::kRowBytes; ++i) {
          hex[2 * i] = "0123456789ABCDEF"[row[i] >> 4];
          hex[2 * i + 1] = "0123456789ABCDEF"[row[i] & 0x0Fu];
        }
        hex[2 * qr::kRowBytes] = '\0';
        LOG_D("UDP", "QR row=%02d:%s", y, hex);
      }
    }
#endif
    return true;
  }

  // Read-only access to the cached window QR bitmap. Two views:
  // - qrBitmap(): the UNPACKED 441B module array (0 = light, 1 = dark,
  //   row-major) — the serial `QR row=` dump packs it per row. This is NOT
  //   the wire format: staging these raw bytes as a 63B QRBITMAP renders
  //   two dotted columns on the head (bench-proven 2026-10-18).
  // - qrBitmapPacked(): the 63B wire payload (packed MSB-first rows) for
  //   LinkServer::stageQrBitmap. Valid only while a pairing window is open;
  //   check qrBitmapValid() first. False on null out or closed window.
  bool qrBitmapValid() const { return qrBitmapValid_; }

  const uint8_t* qrBitmap() const { return qrBitmap_; }

  bool qrBitmapPacked(uint8_t out[qr::kPackedLen]) const {
    return udpstatus::packQrBitmap(qrBitmapValid_, qrBitmap_, out);
  }

  // Send-failure accounting: every UDP unicast goes through
  // udpsend::udpSendChecked; failures bump txFail_ + LOG_W (release-visible).
  // Benign tear-tolerant read like the LinkServer counters; task only.
  uint32_t txFail() const { return txFail_; }

  // Explicit unpair / 10s-hold factory reset hook (Task 10 bench procedure;
  // no button wired yet): clears the NVS bond + in-memory Core + key
  // material. The next begin() (or openPairingWindow()) starts fresh.
  // Also invalidates the PAIR_OK redelivery cache (+zeroizes its secrets).
  // TODO(PHYSICAL-TRIGGER): button-vs-power-cycle decision pending — implements the 10s-hold promise; see
  // DECISION-ISSUE.
  bool clearBond() {
    pairCore_.clearBond();
    bonded_ = false;
    lastCounter_ = 0;
    conn_.clear();
    era_ = 0;
    validCount_ = 0;
    mbedtls_platform_zeroize(psk_, sizeof(psk_));
    lastPair_.valid = false;
    mbedtls_platform_zeroize(lastPair_.code6, sizeof(lastPair_.code6));
    mbedtls_platform_zeroize(lastPair_.cliNonce4, sizeof(lastPair_.cliNonce4));
    mbedtls_platform_zeroize(lastPair_.srvNonce4, sizeof(lastPair_.srvNonce4));
    mbedtls_platform_zeroize(lastPair_.ok39, sizeof(lastPair_.ok39));
    lastPair_.bondMs = 0;
    qrBitmapValid_ = false;  // No open window after an unpair/reset.
    return ConfigStore::clearBond();
  }

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
            handleDriveV2(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kPairReq:
            handlePairReq(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kUnbond:
            handleUnbond(buf, rip, rport, nowMs);
            break;
          case udpstatus::RxKind::kRekeyReq:
            handleRekey(buf, rip, rport, nowMs);
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
  connection::Pinned markAlive(connection::Verified&& verified, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
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
      LOG_I("UDP", "peer re-pinned ip=%s era=%u", rip.toString().c_str(), static_cast<unsigned>(era_));
    }
    return pinned;
  }

  // Pre-bond pin-only: remember the peer for unicast replies (pair + tele-
  // metry) but NEVER publish to the coordinator — nothing drives pre-bond.
  // Post-bond legacy drive datagrams are dead: silent drop. Freshness
  // tracking (lastRxMs_/lastV_/lastOmega_) is untouched on both paths, so
  // the telemetry driving bit never asserts pre-bond.
  void handleLegacy(const IPAddress& rip, uint16_t rport) {
    if (bonded_) {
      dropFirst(firstLegacyDrop_, "drop legacy post-bond (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop legacy post-bond");
      return;
    }
    peerIp_ = rip;
    peerPort_ = rport;
    conn_.pinPeer(connection::ipToU32(rip[0], rip[1], rip[2], rip[3]));
  }

  // DRIVE_V2 verify order (pre-publish): bonded? -> counter fresh? -> HMAC
  // constant-time. Fail = silent drop (the 500ms failsafe stops safely).
  // PASS follows the identical publish path legacy used to own. No heap, no
  // String, fixed stack buffers only; key/code/nonce material never logged.
  void handleDriveV2(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!bonded_) {
      dropFirst(firstDriveDrop_, "drop drive unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop drive unbonded");
      return;
    }
    // Layout [0xAC|mode|thr|str|flags|ctrBE 4B|tag 6B|XOR]: mode=1 thr=2
    // str=3 flags=4 ctr=5..8 tag=9..14 xor=15 (len + header + XOR checked by
    // the caller).
    const uint32_t ctr = (static_cast<uint32_t>(buf[5]) << 24) | (static_cast<uint32_t>(buf[6]) << 16) |
                         (static_cast<uint32_t>(buf[7]) << 8) | static_cast<uint32_t>(buf[8]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
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
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const uint32_t t0 = micros();
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    const uint32_t dt = micros() - t0;  // Unsigned subtraction: wrap-safe.
    if (dt > maxVerifyUs_) {
      maxVerifyUs_ = dt;
    }
    LOG_EVERY_N(128, "UDP", "drive verify dt=%luus max=%luus", static_cast<unsigned long>(dt),
                static_cast<unsigned long>(maxVerifyUs_));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 9, 6);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstDriveDrop_, "drop drive auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop drive auth");
      return;
    }
    // Platform split 1.1 (AuthVerifier threading): fold the passed
    // prechecks into the Verified token — bonded/fresh/hmac re-evaluated
    // fail-closed here (all true on this path; the per-cause drops+logs
    // above stay verbatim). The drive effect below REQUIREs the token.
    const connection::Checks driveChecks{bonded_, true, udpstatus::isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified verified = connection::AuthVerifier::verify(driveChecks, ctr, era_);
    lastCounter_ = ctr;
    ++validCount_;
    // Robot-side persist cadence matches the interval advertised in PAIR_OK
    // (pairing::kPairPersistDefault); the app mirrors it per negotiated byte.
    if ((validCount_ % pairing::kPairPersistDefault) == 0) {
      ConfigStore::saveCounter(lastCounter_);  // Periodic persist, never per-packet (flash wear).
    }
    // Alive funnel BEFORE the drive effect below.
    connection::Pinned pinned = markAlive(std::move(verified), rip, rport, nowMs);
    publishDrive(std::move(pinned), nowMs, buf[1], buf[2], buf[3], buf[4]);
  }

  // Drive effect (extracted VERBATIM from handleDriveV2 — DriveSession
  // absorb minimum): runs ONLY on a valid Pinned token (invalid -> silent
  // no-op, fail-closed). The failsafe policy itself stays in
  // behavior/coordinator.h; the HMAC verify above stays middleware here.
  void publishDrive(connection::Pinned pinned, uint32_t nowMs, uint8_t mode, uint8_t thr, uint8_t str, uint8_t flags) {
    if (!pinned.valid()) {
      return;
    }
    lastRxMs_ = nowMs;
    lastV_ = coordinator::byteToUnit(thr);
    lastOmega_ = coordinator::byteToUnit(str);
    EventBus::publish(EVENT_UDP_COMMAND_RECEIVED, coordinator::packCommand(mode, thr, str, flags));
  }

  // PAIR_REQ layout [0x50|code6 1..6|clientNonce4 7..10|XOR] (len + header +
  // XOR checked by the caller). Bonded -> PAIR_ERR already_bonded. Else
  // tryCode; kOk mints a fresh PSK with store-before-send ordering
  // (saveBond FIRST, then KEK->wrap->PAIR_OK unicast). Crypto/store failures
  // after kOk answer PAIR_ERR internal_retry_fresh (4, M11 additive): the
  // code is already burned, so the client must pair again with a fresh
  // display code. Genuine not_in_window (tryCode != kOk) still sends reason
  // 2. Every reply is unicast to the requester.
  void handlePairReq(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    // Lost-PAIR_OK race: while bonded, an identical retry (same code +
    // client nonce) inside the redelivery window re-unicasts the cached
    // PAIR_OK. Checked BEFORE the already_bonded reply below.
    if (bonded_ && udpstatus::shouldRedeliver(lastPair_.valid, lastPair_.code6, lastPair_.cliNonce4, buf + 1, buf + 7,
                                              lastPair_.bondMs, nowMs)) {
      if (!udpsend::udpSendChecked(udp_, rip, rport, lastPair_.ok39, sizeof(lastPair_.ok39))) {
        ++txFail_;
        LOG_W("UDP", "udp send fail what=%s len=%u", "pair_redeliver", static_cast<unsigned>(sizeof(lastPair_.ok39)));
      } else {
        LOG_I("UDP", "pair redeliver");
      }
      return;
    }
    if (bonded_) {
      sendPairErr(rip, rport, pairing::toWireReason(pairing::Result::kAlreadyBonded), 0);
      dropFirst(firstPairDrop_, "pair req while bonded (first of boot)");
      LOG_EVERY_N(10, "UDP", "pair req while bonded");
      return;
    }
    const pairing::Result r = pairCore_.tryCode(buf + 1, pairing::kCodeLen, nowMs);
    if (r != pairing::Result::kOk) {
      const uint8_t retryS = (r == pairing::Result::kLocked) ? static_cast<uint8_t>(pairing::kLockoutMs / 1000u) : 0;
      sendPairErr(rip, rport, pairing::toWireReason(r), retryS);
      dropFirst(firstPairDrop_, "pair err reason=%u (first of boot)", static_cast<unsigned>(pairing::toWireReason(r)));
      LOG_EVERY_N(10, "UDP", "pair err reason=%u", static_cast<unsigned>(pairing::toWireReason(r)));
      return;
    }
    // Code verified + burned inside Core. Mint the bond: 16 PSK bytes + 4
    // server-nonce bytes from esp_random (CSPRNG) into stack buffers.
    uint8_t newPsk[pairing::kPskLen];
    for (size_t i = 0; i < pairing::kPskLen / 4; ++i) {
      const uint32_t rnd = esp_random();
      newPsk[4 * i] = static_cast<uint8_t>((rnd >> 24) & 0xFFu);
      newPsk[4 * i + 1] = static_cast<uint8_t>((rnd >> 16) & 0xFFu);
      newPsk[4 * i + 2] = static_cast<uint8_t>((rnd >> 8) & 0xFFu);
      newPsk[4 * i + 3] = static_cast<uint8_t>(rnd & 0xFFu);
    }
    uint8_t serverNonce[pairing::kPairNonceLen];
    {
      const uint32_t rnd = esp_random();
      serverNonce[0] = static_cast<uint8_t>((rnd >> 24) & 0xFFu);
      serverNonce[1] = static_cast<uint8_t>((rnd >> 16) & 0xFFu);
      serverNonce[2] = static_cast<uint8_t>((rnd >> 8) & 0xFFu);
      serverNonce[3] = static_cast<uint8_t>(rnd & 0xFFu);
    }
    const uint8_t* code6 = buf + 1;  // Verified match — the HKDF IKM.
    const uint8_t* clientNonce = buf + 7;
    bool sent = false;
    uint8_t out[pairing::kPairOkSize] = {};  // Function scope: cached into lastPair_ on send.
    // Bond baseline is counter 0 (never valid on the wire): the client's
    // first DRIVE_V2 uses counter 1, matching the spec's "starts 1".
    // Platform split 1.1: the receipt overload mints the store-before-ACK
    // proof consumed by sendPairOk below (invalid until the NVS save lands).
    connection::PersistReceipt receipt;
    if (ConfigStore::saveBond(newPsk, 0, receipt)) {
      uint8_t kek[pairing::kKekLen];
      uint8_t ct[pairing::kPairCtLen];
      uint8_t tag[pairing::kPairTagLen];
      if (pairing::deriveKek(code6, clientNonce, serverNonce, kek) &&
          pairing::wrapPsk(newPsk, kek, clientNonce, serverNonce, pairing::kPairPersistDefault, ct, tag)) {
        // PAIR_OK 39B [0x51|server_nonce4|ct17|tag16|XOR]: ct covers
        // psk16||persist_u8 (v1.1 addendum; old 38B frames have no byte).
        out[0] = pairing::kPairOkHeader;
        for (size_t i = 0; i < pairing::kPairNonceLen; ++i) {
          out[1 + i] = serverNonce[i];
        }
        for (size_t i = 0; i < pairing::kPairCtLen; ++i) {
          out[5 + i] = ct[i];
        }
        for (size_t i = 0; i < pairing::kPairTagLen; ++i) {
          out[5 + pairing::kPairCtLen + i] = tag[i];
        }
        uint8_t outXor = 0;
        for (size_t i = 0; i < sizeof(out) - 1; ++i) {
          outXor ^= out[i];
        }
        out[sizeof(out) - 1] = outXor;
        sendPairOk(std::move(receipt), rip, rport, out);
        sent = true;
      }
      mbedtls_platform_zeroize(kek, sizeof(kek));
      mbedtls_platform_zeroize(ct, sizeof(ct));
      mbedtls_platform_zeroize(tag, sizeof(tag));
    }
    if (sent) {
      for (size_t i = 0; i < sizeof(psk_); ++i) {
        psk_[i] = newPsk[i];
      }
      lastCounter_ = 0;
      era_ = 0;  // Fresh pair starts era 0 (saveBond persists it).
      bonded_ = true;
      qrBitmapValid_ = false;         // Code burned: the window is consumed.
      pairCore_.onPairBonded(nowMs);  // PairingExchange forward (same point, same order; idempotent).
      // Cache the reply BEFORE the stack zeroizes below: an identical retry
      // inside kRedeliveryMs re-unicasts these bytes (RAM-only redelivery).
      for (size_t i = 0; i < sizeof(lastPair_.code6); ++i) {
        lastPair_.code6[i] = code6[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.cliNonce4); ++i) {
        lastPair_.cliNonce4[i] = clientNonce[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.srvNonce4); ++i) {
        lastPair_.srvNonce4[i] = serverNonce[i];
      }
      for (size_t i = 0; i < sizeof(lastPair_.ok39); ++i) {
        lastPair_.ok39[i] = out[i];
      }
      lastPair_.bondMs = nowMs;
      lastPair_.valid = true;
      peerIp_ = rip;  // The new owner pins the telemetry unicast path.
      peerPort_ = rport;
      conn_.pinPeer(connection::ipToU32(rip[0], rip[1], rip[2], rip[3]));
      LOG_I("UDP", "pair ok");
    } else {
      // M11 additive reason 4 (internal_retry_fresh): the code is already
      // burned (single-use), so an NVS or crypto failure after kOk
      // (saveBond/deriveKek/wrapPsk) sends 4, not 2. The client maps 4 to
      // "robot error, try a fresh code": open a new window and pair again
      // with the fresh display code. Reasons 0-3 are frozen (never renumber;
      // genuine not_in_window still sends 2 via the tryCode path above).
      sendPairErr(rip, rport, pairing::toWireReason(pairing::Result::kInternal), 0);
      dropFirst(firstPairDrop_, "pair err internal (first of boot)");
      LOG_EVERY_N(10, "UDP", "pair err internal");
    }
    mbedtls_platform_zeroize(newPsk, sizeof(newPsk));
    mbedtls_platform_zeroize(serverNonce, sizeof(serverNonce));
  }

  // UNBOND layout [0xAD|ctrBE4 1..4|tag6 5..10|XOR] (len + header + XOR
  // checked by the caller). Symmetric forget: a signed fire-and-forget from
  // the bonded owner. Fail = silent drop (stale/replay/bad-tag). Pass clears
  // the NVS bond + Core + key material + redelivery cache and opens a fresh
  // pairing window so a new phone can pair immediately. No reply packet.
  void handleUnbond(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    (void)rip;
    (void)rport;
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!bonded_) {
      dropFirst(firstUnbondDrop_, "drop unbond unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop unbond unbonded");
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[1]) << 24) | (static_cast<uint32_t>(buf[2]) << 16) |
                         (static_cast<uint32_t>(buf[3]) << 8) | static_cast<uint32_t>(buf[4]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstUnbondDrop_, "drop unbond replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop unbond replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "UNBOND"||counterBE4)).
    uint8_t msg[udpstatus::kUnbondMsgLen];
    udpstatus::unbondTagMsg(ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 5, udpstatus::kUnbondTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstUnbondDrop_, "drop unbond auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop unbond auth");
      return;
    }
    // Platform split 1.1: fold the passed prechecks into Verified (same
    // pattern as drive; per-cause drops+logs above stay verbatim), consumed
    // by the refresh-then-clear below.
    const connection::Checks unbondChecks{bonded_, true, udpstatus::isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified unbondVerified = connection::AuthVerifier::verify(unbondChecks, ctr, era_);
    ConfigStore::clearBond();
    pairCore_.onUnbonded();  // PairingExchange forward (superset of clearBond: +era/bondMs reset).
    mbedtls_platform_zeroize(psk_, sizeof(psk_));
    bonded_ = false;
    lastCounter_ = 0;
    era_ = 0;
    // Refresh-then-clear: the signed goodbye still proves the owner was
    // alive at nowMs; the net peer is gone (no more unicast to a dead app).
    conn_.refreshThenClear(std::move(unbondVerified), nowMs);
    lastPair_.valid = false;
    mbedtls_platform_zeroize(lastPair_.code6, sizeof(lastPair_.code6));
    mbedtls_platform_zeroize(lastPair_.cliNonce4, sizeof(lastPair_.cliNonce4));
    mbedtls_platform_zeroize(lastPair_.srvNonce4, sizeof(lastPair_.srvNonce4));
    mbedtls_platform_zeroize(lastPair_.ok39, sizeof(lastPair_.ok39));
    lastPair_.bondMs = 0;
    LOG_I("UDP", "unbond ok");
    if (!openPairingWindow()) {
      LOG_W("UDP", "pair window open failed (post-unbond)");
    }
  }

  // REKEY_REQ layout [0x53|eraBE2 1..2|ctrBE4 3..6|tag6 7..12|XOR] (len +
  // header + XOR checked by the caller). Silent Tier-1 rotation: the app
  // derives PSK_next under the OLD psk well before EXHAUSTION_GATE and asks
  // to move to era+1; the robot verifies (bonded -> era is exactly next ->
  // counter fresh -> HMAC constant-time under the OLD psk), persists
  // (era+1/psk_next/counter 0) BEFORE the ACK, then switches atomically and
  // replies REKEY_ACK. Next drive uses counter 1 under the new PSK.
  // Store-before-ACK ordering mirrors pairing (a failed NVS save answers
  // nothing — the app retries, then falls back to the Tier-2 repair copy).
  // Old PSK is dropped immediately on switch (no reorder window): stale
  // in-flight drive frames under the old key drop via HMAC — documented in
  // shared/protocol/README.md. Duplicate REQ with era == current era
  // re-ACKs idempotently WITHOUT re-deriving or re-verifying (the lost-ACK
  // retry reuses the same era; the ACK carries no secret — era + HMAC tag
  // under the new PSK — so the unverified re-ACK leaks nothing). Any other
  // era (replay or future jump) is a silent drop. No reply on failure.
  void handleRekey(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!bonded_) {
      dropFirst(firstRekeyDrop_, "drop rekey unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop rekey unbonded");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era == era_) {
      // Idempotent duplicate: re-ACK WITHOUT re-deriving or re-verifying
      // (the lost-ACK retry reuses the same era; the ACK carries no secret).
      // Explicitly bypasses the Pinned gate below — documented, by design.
      sendRekeyAckImpl(rip, rport, era_);
      LOG_EVERY_N(20, "UDP", "rekey redeliver era=%u", static_cast<unsigned>(era_));
      return;
    }
    if (era != static_cast<uint16_t>(era_ + 1)) {
      dropFirst(firstRekeyDrop_, "drop rekey era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(era_));
      LOG_EVERY_N(20, "UDP", "drop rekey era=%u have=%u", static_cast<unsigned>(era), static_cast<unsigned>(era_));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstRekeyDrop_, "drop rekey replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop rekey replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(oldPsk, "REKEY"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kRekeyMsgLen];
    pairing::rekeyTagMsg(era, ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 7, pairing::kRekeyTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstRekeyDrop_, "drop rekey auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop rekey auth");
      return;
    }
    // Platform split 1.1: fold the passed prechecks into Verified (same
    // pattern as drive; per-cause drops+logs above stay verbatim).
    const connection::Checks rekeyChecks{bonded_, era == static_cast<uint16_t>(era_ + 1),
                                         udpstatus::isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified rekeyVerified = connection::AuthVerifier::verify(rekeyChecks, ctr, era);
    uint8_t nextPsk[pairing::kPskLen];
    if (!pairing::deriveRekeyPsk(psk_, era, nextPsk)) {
      dropFirst(firstRekeyDrop_, "drop rekey derive (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop rekey derive");
      return;
    }
    if (!ConfigStore::saveRekey(nextPsk, era, 0)) {
      mbedtls_platform_zeroize(nextPsk, sizeof(nextPsk));
      dropFirst(firstRekeyDrop_, "drop rekey persist (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop rekey persist");
      return;
    }
    for (size_t i = 0; i < sizeof(psk_); ++i) {
      psk_[i] = nextPsk[i];
    }
    mbedtls_platform_zeroize(nextPsk, sizeof(nextPsk));
    era_ = era;
    pairCore_.onRekeyApplied(era);  // PairingExchange forward (same point, same order).
    lastCounter_ = 0;
    validCount_ = 0;
    // Alive funnel BEFORE the ACK below.
    connection::Pinned rekeyPinned = markAlive(std::move(rekeyVerified), rip, rport, nowMs);
    sendRekeyAck(std::move(rekeyPinned), rip, rport, era_);
    LOG_I("UDP", "rekey ok era=%u", static_cast<unsigned>(era_));
  }

  // ALIVE layout [0x55|eraBE2 1..2|ctrBE4 3..6|tag6 7..12|XOR] (len +
  // header + XOR checked by the caller). Keepalive under the bonded PSK:
  // verify order mirrors drive (bonded? -> era is current -> counter fresh
  // -> HMAC constant-time). Pass refreshes the alive funnel WITHOUT any
  // drive effect (lastV_/lastOmega_/lastRxMs_ untouched — only DRIVE moves
  // the sticks and feeds the failsafe). Fail = silent drop. Pre-bond ALIVEs
  // drop like pre-bond drive; old-key frames die via HMAC.
  void handleAlive(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!bonded_) {
      dropFirst(firstAliveDrop_, "drop alive unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop alive unbonded");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era != era_) {
      dropFirst(firstAliveDrop_, "drop alive era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(era_));
      LOG_EVERY_N(20, "UDP", "drop alive era=%u have=%u", static_cast<unsigned>(era), static_cast<unsigned>(era_));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstAliveDrop_, "drop alive replay ctr=%lu (first of boot)", static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop alive replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "ALIVE"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kAliveMsgLen];
    pairing::aliveTagMsg(era, ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 7, pairing::kAliveTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstAliveDrop_, "drop alive auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop alive auth");
      return;
    }
    // Platform split 1.1: fold the passed prechecks into Verified (same
    // pattern as drive; per-cause drops+logs above stay verbatim).
    const connection::Checks aliveChecks{bonded_, era == era_, udpstatus::isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified aliveVerified = connection::AuthVerifier::verify(aliveChecks, ctr, era);
    // Shared counter: a valid ALIVE consumes exactly like drive (same
    // holder app-side), so a replayed ALIVE can never refresh twice.
    lastCounter_ = ctr;
    ++validCount_;
    if ((validCount_ % pairing::kPairPersistDefault) == 0) {
      ConfigStore::saveCounter(lastCounter_);  // Periodic persist, never per-packet (flash wear).
    }
    markAlive(std::move(aliveVerified), rip, rport,
              nowMs);  // Alive only: no lastV_/lastOmega_/lastRxMs_, no publish (Pinned needs no effect).
  }

  // DISCONNECT layout [0x56|eraBE2 1..2|ctrBE4 3..6|tag6 7..12|XOR] (len +
  // header + XOR checked by the caller). Graceful goodbye under the bonded
  // PSK: verify mirrors ALIVE (bonded? -> era is current -> counter fresh ->
  // HMAC constant-time). Pass runs refresh-then-clear like UNBOND — the
  // signed goodbye proves the owner was alive at nowMs, then the net peer is
  // gone (conn_.havePeer() = false). Unlike UNBOND the bond is KEPT (no NVS/Core/
  // key clear, no pairing window): the next launch rejoins instantly.
  // Never drives. Fail = silent drop. Pre-bond DISCONNECTs drop like drive.
  void handleDisconnect(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    (void)rip;
    (void)rport;
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (!bonded_) {
      dropFirst(firstDisconnectDrop_, "drop disconnect unbonded (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop disconnect unbonded");
      return;
    }
    const uint16_t era = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 8) | static_cast<uint16_t>(buf[2]));
    if (era != era_) {
      dropFirst(firstDisconnectDrop_, "drop disconnect era=%u have=%u (first of boot)", static_cast<unsigned>(era),
                static_cast<unsigned>(era_));
      LOG_EVERY_N(20, "UDP", "drop disconnect era=%u have=%u", static_cast<unsigned>(era), static_cast<unsigned>(era_));
      return;
    }
    const uint32_t ctr = (static_cast<uint32_t>(buf[3]) << 24) | (static_cast<uint32_t>(buf[4]) << 16) |
                         (static_cast<uint32_t>(buf[5]) << 8) | static_cast<uint32_t>(buf[6]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
      dropFirst(firstDisconnectDrop_, "drop disconnect replay ctr=%lu (first of boot)",
                static_cast<unsigned long>(ctr));
      LOG_EVERY_N(20, "UDP", "drop disconnect replay ctr=%lu", static_cast<unsigned long>(ctr));
      return;
    }
    // tag = first6(HMAC-SHA256(psk, "DISCONNECT"||eraBE2||counterBE4)).
    uint8_t msg[pairing::kDisconnectMsgLen];
    pairing::disconnectTagMsg(era, ctr, msg);
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    bool ok = (rc == 0);
    if (ok) {
      ok = pairing::ctEqual(mac, buf + 7, pairing::kDisconnectTagLen);
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (!ok) {
      dropFirst(firstDisconnectDrop_, "drop disconnect auth (first of boot)");
      LOG_EVERY_N(20, "UDP", "drop disconnect auth");
      return;
    }
    // Platform split 1.1: fold the passed prechecks into Verified (same
    // pattern as drive; per-cause drops+logs above stay verbatim), consumed
    // by the refresh-then-clear below.
    const connection::Checks discChecks{bonded_, era == era_, udpstatus::isFreshCounter(lastCounter_, ctr), ok};
    connection::Verified discVerified = connection::AuthVerifier::verify(discChecks, ctr, era);
    lastCounter_ = ctr;  // Consume: a replayed goodbye can never clear twice.
    ++validCount_;
    if ((validCount_ % pairing::kPairPersistDefault) == 0) {
      ConfigStore::saveCounter(lastCounter_);
    }
    conn_.refreshThenClear(std::move(discVerified), nowMs);  // Refresh-then-clear: net gone, bond kept.
    LOG_I("UDP", "disconnect ok");
  }

  // REKEY_ACK 10B [0x54|eraBE2|tag6|XOR] with
  // tag = first6(HMAC-SHA256(newPsk, "REKEYACK"||eraBE2)). Called only after
  // the switch (psk_ IS the new key here) or for the idempotent duplicate
  // path. Key material never logged.
  // Platform split 1.1: the Impl holds the wire bytes VERBATIM; the
  // token-gated sendRekeyAck below is the only path the verified switch
  // takes (consumes Pinned by value — proves the markAlive funnel ran
  // BEFORE the ACK). The idempotent duplicate path calls the Impl directly
  // (no verify by design — the ACK leaks no secret).
  void sendRekeyAck(connection::Pinned pinned, const IPAddress& ip, uint16_t port, uint16_t era) {
    if (!pinned.valid()) {
      return;
    }
    sendRekeyAckImpl(ip, port, era);
  }

  void sendRekeyAckImpl(const IPAddress& ip, uint16_t port, uint16_t era) {
    uint8_t msg[pairing::kRekeyAckMsgLen];
    pairing::rekeyAckTagMsg(era, msg);
    uint8_t mac[32];
    uint8_t out[udpstatus::kRekeyAckSize];
    out[0] = udpstatus::kRekeyAckHeader;
    out[1] = static_cast<uint8_t>((era >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>(era & 0xFFu);
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = -1;
    if (md != nullptr) {
      rc = mbedtls_md_hmac(md, psk_, sizeof(psk_), msg, sizeof(msg), mac);
    }
    mbedtls_platform_zeroize(msg, sizeof(msg));
    if (rc != 0) {
      mbedtls_platform_zeroize(mac, sizeof(mac));
      return;
    }
    for (size_t i = 0; i < pairing::kRekeyTagLen; ++i) {
      out[3 + i] = mac[i];
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    uint8_t xorSum = 0;
    for (size_t i = 0; i + 1 < sizeof(out); ++i) {
      xorSum ^= out[i];
    }
    out[sizeof(out) - 1] = xorSum;
    if (!udpsend::udpSendChecked(udp_, ip, port, out, sizeof(out))) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "rekey_ack", static_cast<unsigned>(sizeof(out)));
    }
  }

  void sendPairErr(const IPAddress& ip, uint16_t port, uint8_t reason, uint8_t retryS) {
    uint8_t out[4];
    out[0] = 0x52;
    out[1] = reason;
    out[2] = retryS;
    out[3] = static_cast<uint8_t>(out[0] ^ out[1] ^ out[2]);
    if (!udpsend::udpSendChecked(udp_, ip, port, out, sizeof(out))) {
      ++txFail_;
      LOG_W("UDP", "udp send fail what=%s len=%u", "pair_err", static_cast<unsigned>(sizeof(out)));
    }
  }

  // PAIR_OK 39B unicast (moved VERBATIM out of handlePairReq): consumes the
  // PersistReceipt by value (moved) — proves ConfigStore::saveBond persisted
  // BEFORE anything left the radio (store-before-send contract). Invalid
  // receipt -> silent no-op (fail-closed).
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

    // Driving = last commanded stick nonzero AND fresh (server-local RX
    // tracking; the coordinator owns no driving flag). The cliff bit below
    // mirrors the coordinator level, which the sensor task keeps live.
    const bool driving = (lastV_ != 0.0f || lastOmega_ != 0.0f) && (nowMs - lastRxMs_ <= kStaleMs);
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
    const bool gateOk = udpstatus::shouldSendTelemetry(bonded_, conn_.havePeer(), conn_.lastAliveMs(), nowMs);
    if (!gateOk) {
      // Correlation marker (log-only, no behavior change): the alive-stale
      // silence edge shares firstTelemetryDrop_ (one first-of-boot INFO) and
      // stays throttled afterwards — same pattern as the no-peer path.
      if (bonded_ && conn_.havePeer()) {
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
  float lastV_;
  float lastOmega_;
  uint32_t lastRxMs_;
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
  // Telemetry cadence + encode core (services/telemetry_manager.h): the
  // bonded-default rate is kTelemetryMs passthrough (no new logic); the
  // socket + gate logging stay here in sendTelemetry.
  telemetry::TelemetryManager telem_;
  uint32_t lastRetryMs_;
  pairing::Core pairCore_;
  uint8_t psk_[16];
  uint32_t lastCounter_;
  bool bonded_;
  // Tier-1 rekey era (u16, NVS "bera", fresh pair resets to 0). DRIVE_V2
  // carries no era — post-switch old-key frames drop via HMAC, so no drive
  // layout change was needed.
  uint16_t era_;
  uint32_t validCount_;
  uint32_t maxVerifyUs_;
  // Checked-send failures (udpsend::udpSendChecked false): rare/backpressure
  // class, WARNed at every occurrence (release-visible). Task-written, benign
  // tear-tolerant read via txFail().
  uint32_t txFail_ = 0;
  // Cached QR bitmap of the CURRENT open window's code (V1 21x21,
  // qr::kModules; see the normative payload contract in
  // shared/protocol/README.md §"Pairing QR"). Encoded on every window open
  // in ALL builds (release included) so future consumers (head link) read it
  // without re-encoding; the serial row dump stays DEBUG-only. Invalid
  // whenever no window is open (bonded, burned, or cleared).
  uint8_t qrBitmap_[qr::kModules] = {};
  bool qrBitmapValid_ = false;
  // Last PAIR_OK redelivery cache (RAM-only, never persisted): an identical
  // PAIR_REQ (same code + client nonce) inside kRedeliveryMs re-unicasts the
  // cached bytes. Invalidated on reboot (begin both paths), clearBond, and
  // signed UNBOND; secrets zeroized alongside psk_.
  struct LastPair {
    bool valid = false;
    uint8_t code6[6] = {};
    uint8_t cliNonce4[4] = {};
    uint8_t srvNonce4[4] = {};
    uint8_t ok39[pairing::kPairOkSize] = {};
    uint32_t bondMs = 0;
  };
  // Static assert #3 (platform split 1.1): the redelivery cache holds
  // exactly one PAIR_OK frame — a size drift here breaks the unicast on the
  // lost-PAIR_OK race. Host-side size pins live in test_udp_server.cpp.
  static_assert(sizeof(LastPair::ok39) == pairing::kPairOkSize, "LastPair cache must hold exactly one PAIR_OK frame");
  LastPair lastPair_;
  // First-drop flags for dropFirst() (bench lesson: first occurrence per
  // boot per class always logs). Reset only by reboot — never per window.
  bool firstMalformedDrop_ = true;
  bool firstLegacyDrop_ = true;
  bool firstDriveDrop_ = true;
  bool firstPairDrop_ = true;
  bool firstUnbondDrop_ = true;
  bool firstRekeyDrop_ = true;
  bool firstAliveDrop_ = true;
  bool firstDisconnectDrop_ = true;
  bool firstTelemetryDrop_ = true;
};

#endif  // ARDUINO
