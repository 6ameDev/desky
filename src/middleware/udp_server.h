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
// Telemetry source (minimal but forward-compatible):
//   mode/active/cliff = Coordinator::snapshot() (coordinator-owned intent);
//   distanceMM = ToF distanceMm() live; pitch/roll = fusion::Fusion over the
//     live MPU reading on demand (same atan2f convention as fusion; TODO(C):
//     still 0 until the MPU is healthy — healthy on a live robot);
//   isDriving = tracked LOCALLY from RX (last commanded v/omega nonzero AND
//     fresh within CFG_COORDINATOR_STALE_MS), not from the coordinator.
//   cliff bit = coordinator cliffDetected LEVEL (no fusion task publishes
//     EVENT_CLIFF_DETECTED yet, so it stays 0 — Workstream C wires the veto).
// Destination: unicast to the last control peer (IP+port of the incoming
// datagram); before any peer is seen, subnet broadcast (AP /24 .255, STA
// ip|~mask) so telemetry flows with zero registration. scripts/ also send
// one hello datagram to pin the unicast path (see udp_telemetry.py --help).
//
// Rules: no heap/String in the RX/TX path (fixed 8/9-byte buffers), WDT fed
// every tick, Diagnostics::logWatermarks("UDP") every ~5s, task never halts.

// ── Arduino-free: statusFlags bit map + fixed-point helpers ──────────────
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

}  // namespace udpstatus

#ifdef ARDUINO
// ── Firmware: UDP server task (WiFi + WiFiUDP, Core 0) ────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "behavior/coordinator.h"
#include "config.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "middleware/sensor_fusion.h"
#include "middleware/udp_codec.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/logger.h"

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
        lastRetryMs_(0) {}

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
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&UdpServer::taskEntry, "udp", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
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

      // RX: drain every pending datagram; publish ONE event per VALID packet.
      // Never dedupe/coalesce: the coordinator resume rule needs the fresh
      // nonzero edge, so each datagram is its own event. Bad
      // header/checksum/length drops with throttled logging, nothing else.
      int pending = 0;
      while ((pending = udp_.parsePacket()) > 0) {
        const IPAddress rip = udp_.remoteIP();
        const uint16_t rport = udp_.remotePort();
        uint8_t buf[8];  // 6-byte control + slack so oversize is detectable.
        const int got = udp_.read(buf, sizeof(buf));
        udp::ControlPacket pkt;
        if (got == static_cast<int>(udp::kControlSize) && udp::decodeControl(buf, static_cast<size_t>(got), pkt)) {
          peerIp_ = rip;
          peerPort_ = rport;
          havePeer_ = true;
          lastRxMs_ = nowMs;
          lastV_ = coordinator::byteToUnit(pkt.throttle);
          lastOmega_ = coordinator::byteToUnit(pkt.steering);
          EventBus::publish(EVENT_UDP_COMMAND_RECEIVED,
                            coordinator::packCommand(pkt.mode, pkt.throttle, pkt.steering, pkt.flags));
        } else {
          LOG_EVERY_N(20, "UDP", "drop datagram pending=%d got=%d", pending, got);
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

  void sendTelemetry(uint32_t nowMs) {
    // Coordinator-owned intent (mode/active/cliff level); defaults hold if
    // the snapshot ever fails (boot race): MANUAL, stopped, no cliff.
    SystemState snap;
    coord_->snapshot(snap);

    // Live sensors -> fusion on demand (same atan2f convention as fusion).
    // No fusion task exists yet (Workstream C); pitch/roll are real tilt
    // once the MPU is healthy, 0 before that.
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
    // tracking; the coordinator owns no driving flag).
    // TODO(C): cliff bit mirrors the coordinator level, which stays 0 until
    // the fusion task publishes EVENT_CLIFF_DETECTED — no real veto yet.
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
      // No peer yet: subnet broadcast so listeners need zero registration.
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
};

#endif  // ARDUINO
