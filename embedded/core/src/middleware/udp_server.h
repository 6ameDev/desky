#pragma once
// desky v2 UDP server — header-only, Phase 4 Workstream B.
//
// Owns dual-mode WiFi + the binary control/telemetry sockets (wire format in
// middleware/udp_codec.h, FROZEN) + one Core 0 FreeRTOS task. Control RX
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
// Destination: unicast to the last control peer (IP+port of the incoming
// datagram); before any peer is seen, subnet broadcast (AP /24 .255, STA
// ip|~mask) so telemetry flows with zero registration. scripts/ also send
// one hello datagram to pin the unicast path (see udp_telemetry.py --help).
//
// Rules: no heap/String in the RX/TX path (fixed stack buffers: 20B RX,
// 9B telemetry, 38B PAIR_OK, 8B HMAC msg), WDT fed every tick,
// Diagnostics::logWatermarks("UDP") every ~5s, task never halts. Key, code
// and nonce bytes are never logged (the pair-code LOG_I line is the
// display); drops and HMAC timing use throttled LOG_EVERY_N.

// ── Arduino-free: statusFlags bit map + fixed-point helpers ──────────────
#include <stddef.h>
#include <stdint.h>

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
constexpr uint8_t kPairReqHeader = 0x50;
constexpr size_t kPairReqSize = 12;

// RX demux result: classifyRx maps (len, header) -> kind; anything else
// (wrong size, wrong header, oversize truncation) is kDrop.
enum class RxKind : uint8_t { kLegacy, kDriveV2, kPairReq, kDrop };

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
  return RxKind::kDrop;
}

// Wrap-aware strictly-greater for the DRIVE_V2 32-bit BE counter (starts at
// 1; initial lastSeen=0). Unsigned subtraction throughout: wrap-safe across
// the 2^32 rollover (same idiom as coordinator/pairing millis math). A diff
// of exactly half the range is direction-ambiguous and rejected.
inline bool isFreshCounter(uint32_t lastSeen, uint32_t got) {
  return got != lastSeen && (got - lastSeen) < 0x80000000u;
}

}  // namespace udpstatus

#ifdef ARDUINO
// ── Firmware: UDP server task (WiFi + WiFiUDP, Core 0) ────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "behavior/coordinator.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "middleware/pairing.h"
#include "middleware/sensor_fusion.h"
#include "middleware/udp_codec.h"
#include "services/config_store.h"
#include "services/event_bus.h"

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
        havePeer_(false),
        lastV_(0.0f),
        lastOmega_(0.0f),
        lastRxMs_(0),
        lastRetryMs_(0),
        pairCore_(),
        psk_{},
        lastCounter_(0),
        bonded_(false),
        validCount_(0),
        maxVerifyUs_(0) {}

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
    uint8_t loadedPsk[16] = {};
    uint32_t storedCtr = 0;
    bool storedBond = false;
    if (ConfigStore::loadBond(loadedPsk, storedCtr, storedBond) && storedBond) {
      bonded_ = true;
      lastCounter_ = storedCtr;
      for (size_t i = 0; i < sizeof(psk_); ++i) {
        psk_[i] = loadedPsk[i];
      }
      LOG_I("UDP", "bond loaded counter=%lu", static_cast<unsigned long>(storedCtr));
    } else {
      bonded_ = false;
      lastCounter_ = 0;
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
    return true;
  }

  // Explicit unpair / 10s-hold factory reset hook (Task 10 bench procedure;
  // no button wired yet): clears the NVS bond + in-memory Core + key
  // material. The next begin() (or openPairingWindow()) starts fresh.
  bool clearBond() {
    pairCore_.clearBond();
    bonded_ = false;
    lastCounter_ = 0;
    validCount_ = 0;
    mbedtls_platform_zeroize(psk_, sizeof(psk_));
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
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
          continue;
        }
        const size_t ulen = static_cast<size_t>(got);
        const udpstatus::RxKind kind = udpstatus::classifyRx(ulen, buf[0]);
        if (kind == udpstatus::RxKind::kDrop) {
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
          continue;
        }
        // XOR checksum over bytes 0..len-2 must equal the last byte.
        uint8_t xorSum = 0;
        for (size_t i = 0; i + 1 < ulen; ++i) {
          xorSum ^= buf[i];
        }
        if (xorSum != buf[ulen - 1]) {
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
          case udpstatus::RxKind::kDrop:
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

  // Pre-bond pin-only: remember the peer for unicast replies (pair + tele-
  // metry) but NEVER publish to the coordinator — nothing drives pre-bond.
  // Post-bond legacy drive datagrams are dead: silent drop. Freshness
  // tracking (lastRxMs_/lastV_/lastOmega_) is untouched on both paths, so
  // the telemetry driving bit never asserts pre-bond.
  void handleLegacy(const IPAddress& rip, uint16_t rport) {
    if (bonded_) {
      LOG_EVERY_N(20, "UDP", "drop legacy post-bond");
      return;
    }
    peerIp_ = rip;
    peerPort_ = rport;
    havePeer_ = true;
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
      LOG_EVERY_N(20, "UDP", "drop drive unbonded");
      return;
    }
    // Layout [0xAC|mode|thr|str|flags|ctrBE 4B|tag 6B|XOR]: mode=1 thr=2
    // str=3 flags=4 ctr=5..8 tag=9..14 xor=15 (len + header + XOR checked by
    // the caller).
    const uint32_t ctr = (static_cast<uint32_t>(buf[5]) << 24) | (static_cast<uint32_t>(buf[6]) << 16) |
                         (static_cast<uint32_t>(buf[7]) << 8) | static_cast<uint32_t>(buf[8]);
    if (!udpstatus::isFreshCounter(lastCounter_, ctr)) {
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
      LOG_EVERY_N(20, "UDP", "drop drive auth");
      return;
    }
    lastCounter_ = ctr;
    ++validCount_;
    if ((validCount_ % 128u) == 0) {
      ConfigStore::saveCounter(lastCounter_);  // Periodic persist, never per-packet (flash wear).
    }
    peerIp_ = rip;
    peerPort_ = rport;
    havePeer_ = true;
    lastRxMs_ = nowMs;
    lastV_ = coordinator::byteToUnit(buf[2]);
    lastOmega_ = coordinator::byteToUnit(buf[3]);
    EventBus::publish(EVENT_UDP_COMMAND_RECEIVED, coordinator::packCommand(buf[1], buf[2], buf[3], buf[4]));
  }

  // PAIR_REQ layout [0x50|code6 1..6|clientNonce4 7..10|XOR] (len + header +
  // XOR checked by the caller). Bonded -> PAIR_ERR already_bonded. Else
  // tryCode; kOk mints a fresh PSK with store-before-send ordering
  // (saveBond FIRST, then KEK->wrap->PAIR_OK unicast). Crypto/store failures
  // after kOk answer PAIR_ERR not_in_window: the code is already burned, so
  // the client must start over. Every reply is unicast to the requester.
  void handlePairReq(const uint8_t* buf, const IPAddress& rip, uint16_t rport, uint32_t nowMs) {
    DESKY_ASSERT(buf != nullptr);
    if (buf == nullptr) {
      return;
    }
    if (bonded_) {
      sendPairErr(rip, rport, pairing::toWireReason(pairing::Result::kAlreadyBonded), 0);
      LOG_EVERY_N(10, "UDP", "pair req while bonded");
      return;
    }
    const pairing::Result r = pairCore_.tryCode(buf + 1, pairing::kCodeLen, nowMs);
    if (r != pairing::Result::kOk) {
      const uint8_t retryS =
          (r == pairing::Result::kLocked) ? static_cast<uint8_t>(pairing::kLockoutMs / 1000u) : 0;
      sendPairErr(rip, rport, pairing::toWireReason(r), retryS);
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
    // Bond baseline is counter 0 (never valid on the wire): the client's
    // first DRIVE_V2 uses counter 1, matching the spec's "starts 1".
    if (ConfigStore::saveBond(newPsk, 0)) {
      uint8_t kek[pairing::kKekLen];
      uint8_t ct[pairing::kPskLen];
      uint8_t tag[pairing::kPairTagLen];
      if (pairing::deriveKek(code6, clientNonce, serverNonce, kek) &&
          pairing::wrapPsk(newPsk, kek, clientNonce, serverNonce, ct, tag)) {
        // PAIR_OK 38B [0x51|server_nonce4|ct16|tag16|XOR].
        uint8_t out[38];
        out[0] = pairing::kPairOkHeader;
        for (size_t i = 0; i < pairing::kPairNonceLen; ++i) {
          out[1 + i] = serverNonce[i];
        }
        for (size_t i = 0; i < pairing::kPskLen; ++i) {
          out[5 + i] = ct[i];
        }
        for (size_t i = 0; i < pairing::kPairTagLen; ++i) {
          out[21 + i] = tag[i];
        }
        uint8_t outXor = 0;
        for (size_t i = 0; i < sizeof(out) - 1; ++i) {
          outXor ^= out[i];
        }
        out[sizeof(out) - 1] = outXor;
        udp_.beginPacket(rip, rport);
        udp_.write(out, sizeof(out));
        udp_.endPacket();
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
      bonded_ = true;
      peerIp_ = rip;  // The new owner pins the telemetry unicast path.
      peerPort_ = rport;
      havePeer_ = true;
      LOG_I("UDP", "pair ok");
    } else {
      sendPairErr(rip, rport, pairing::toWireReason(pairing::Result::kNotInWindow), 0);
      LOG_EVERY_N(10, "UDP", "pair err internal");
    }
    mbedtls_platform_zeroize(newPsk, sizeof(newPsk));
    mbedtls_platform_zeroize(serverNonce, sizeof(serverNonce));
  }

  void sendPairErr(const IPAddress& ip, uint16_t port, uint8_t reason, uint8_t retryS) {
    uint8_t out[4];
    out[0] = 0x52;
    out[1] = reason;
    out[2] = retryS;
    out[3] = static_cast<uint8_t>(out[0] ^ out[1] ^ out[2]);
    udp_.beginPacket(ip, port);
    udp_.write(out, sizeof(out));
    udp_.endPacket();
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
    udp::TelemetryPacket tp;
    tp.pitch = udpstatus::degToDecideg(pitchDeg);
    tp.roll = udpstatus::degToDecideg(rollDeg);
    tp.distanceMm = distMm;
    tp.statusFlags = udpstatus::packStatus(snap.cliffDetected, driving, static_cast<uint8_t>(snap.mode));
    uint8_t out[udp::kTelemetrySize];
    udp::encodeTelemetry(tp, out);

    if (havePeer_) {
      udp_.beginPacket(peerIp_, peerPort_);
    } else {
      if (!bonded_) {
        return;  // Pre-bond: unicast-only, never subnet broadcast.
      }
      // Post-bond without a peer yet (reboot before the first drive packet):
      // keep the legacy subnet-broadcast fallback so telemetry still flows.
      const IPAddress bcast = broadcastIP();
      if (bcast == IPAddress(0, 0, 0, 0)) {
        return;  // STA with no link: nothing to send on.
      }
      udp_.beginPacket(bcast, kPort);
    }
    udp_.write(out, sizeof(out));
    udp_.endPacket();
  }

  // Subnet broadcast for the pre-peer path: AP /24 .255, STA ip|~mask.
  // Returns 0.0.0.0 when there is no usable interface (STA pre-link).
  IPAddress broadcastIP() const {
    if (kStaMode) {
      if (WiFi.status() != WL_CONNECTED) {
        return IPAddress(0, 0, 0, 0);
      }
      const uint32_t ip = static_cast<uint32_t>(WiFi.localIP());
      const uint32_t mask = static_cast<uint32_t>(WiFi.subnetMask());
      return IPAddress(ip | ~mask);
    }
    const IPAddress ap = WiFi.softAPIP();
    return IPAddress(ap[0], ap[1], ap[2], 255);
  }

  Coordinator* coord_;
  Vl53l0xDriver* tof_;
  Mpu6500Driver* mpu_;
  TaskHandle_t task_;
  WiFiUDP udp_;
  IPAddress peerIp_;
  uint16_t peerPort_;
  bool havePeer_;
  float lastV_;
  float lastOmega_;
  uint32_t lastRxMs_;
  uint32_t lastRetryMs_;
  pairing::Core pairCore_;
  uint8_t psk_[16];
  uint32_t lastCounter_;
  bool bonded_;
  uint32_t validCount_;
  uint32_t maxVerifyUs_;
};

#endif  // ARDUINO
