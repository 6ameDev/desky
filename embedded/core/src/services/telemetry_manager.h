#pragma once
// Telemetry manager — header-only.
//
// Owns the telemetry TX decision (cadence + edge) + the snapshot encode.
// Arduino-free (stdint/stddef/string + udp_codec only) so host tests include
// it directly. No heap, no sockets: bytes leave through a UdpServer-provided
// send callback.
//
// Include order: transport/udp_server.h top half (udpstatus — the single
// owner of packStatus/degToDecideg) BEFORE this header.
//
// Split: UdpServer keeps the dumb pipes (gate + silence logging +
// WiFiUDP unicast); this header owns the aggregate Snapshot, encodeSnapshot,
// cadence + edge + encode state, and the RX-observation triple (noteCommand /
// isDriving: the driving bit is services-owned telemetry state, never
// coordinator state). The snapshot fill + outer TX cadence live in
// services/TelemetryTask, driven from the main loop.
// Subscription placeholder: today the rate is the bonded-default; a future
// SET_RATE lands here (setRateMs + per-peer state).

#ifndef UDPSTATUS_TOP_HALF_INCLUDED
#error "include transport/udp_server.h before services/telemetry_manager.h (udpstatus owns pack/gate)"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "transport/udp_codec.h"

namespace telemetry {

// Aggregate snapshot IN (filled by the telemetry task): mode/cliff/driving
// + pitch/roll/dist. Plain struct, no behavior.
struct Snapshot {
  uint8_t mode = 0;
  bool cliff = false;
  bool driving = false;
  float pitchDeg = 0.0f;
  float rollDeg = 0.0f;
  uint16_t distMm = 0;
};

// The UdpServer::sendTelemetry pack lines, verbatim (moved, not changed).
inline void encodeSnapshot(const Snapshot& s, udp::TelemetryPacket& pkt) {
  pkt.pitch = udpstatus::degToDecideg(s.pitchDeg);
  pkt.roll = udpstatus::degToDecideg(s.rollDeg);
  pkt.distanceMm = s.distMm;
  pkt.statusFlags = udpstatus::packStatus(s.cliff, s.driving, s.mode);
}

class TelemetryManager {
 public:
  // rateMs is the bonded-default cadence, owned by UdpServer::kTelemetryMs
  // — always pass that; never a literal here. staleMs is the RX-observation
  // freshness window, owned by UdpServer::kStaleMs — same pattern. It defaults
  // to 0 only so existing host tests keep compiling; firmware must pass the
  // real window.
  explicit TelemetryManager(uint32_t rateMs, uint32_t staleMs = 0)
      : rateMs_(rateMs),
        staleMs_(staleMs),
        lastTxMs_(0),
        haveSent_(false),
        lastV_(0.0f),
        lastOmega_(0.0f),
        lastRxMs_(0),
        haveCmd_(false) {
    for (size_t i = 0; i < sizeof(last_); ++i) {
      last_[i] = 0;
    }
  }

  // Decides due||edge -> encode + send via the UdpServer-provided send fn
  // (send(buf, len)), else silent. gateOk is the shouldSendTelemetry verdict
  // computed by UdpServer (which also owns the silence logging). First send
  // is an edge (no baseline yet). No heap: SendFn is a template
  // (lambda/fn-ptr, never std::function).
  template <typename SendFn>
  bool publish(const udp::TelemetryPacket& pkt, bool gateOk, uint32_t nowMs, SendFn send) {
    if (!gateOk) {
      return false;
    }
    uint8_t out[udp::kTelemetrySize];
    udp::encodeTelemetry(pkt, out);
    bool edge = !haveSent_;
    if (!edge) {
      for (size_t i = 0; i < sizeof(out); ++i) {
        if (out[i] != last_[i]) {
          edge = true;
          break;
        }
      }
    }
    // Unsigned subtraction: wrap-safe across millis() rollover.
    const bool due = !haveSent_ || (nowMs - lastTxMs_) >= rateMs_;
    if (!edge && !due) {
      return false;
    }
    send(out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); ++i) {
      last_[i] = out[i];
    }
    lastTxMs_ = nowMs;
    haveSent_ = true;
    return true;
  }

  // Subscription hook (SET_RATE): placeholder. Unused today (rate is fixed
  // at construction); the per-peer state lands here.
  void setRateMs(uint32_t rateMs) { rateMs_ = rateMs; }
  uint32_t rateMs() const { return rateMs_; }
  uint32_t lastTxMs() const { return lastTxMs_; }
  bool hasSent() const { return haveSent_; }

  // RX observation: the driving bit is a server-local observation (last
  // commanded stick nonzero AND fresh), NOT coordinator state. The
  // coordinator loop reports every verified drive command here directly; the
  // encode path reads isDriving(nowMs). Wrap-safe; the boundary counts as
  // fresh.
  void noteCommand(float v, float omega, uint32_t nowMs) {
    lastV_ = v;
    lastOmega_ = omega;
    lastRxMs_ = nowMs;
    haveCmd_ = true;
  }

  bool isDriving(uint32_t nowMs) const {
    return haveCmd_ && (lastV_ != 0.0f || lastOmega_ != 0.0f) && (nowMs - lastRxMs_ <= staleMs_);
  }

  uint32_t staleMs() const { return staleMs_; }

 private:
  uint32_t rateMs_;
  uint32_t staleMs_;
  uint32_t lastTxMs_;
  bool haveSent_;
  float lastV_;
  float lastOmega_;
  uint32_t lastRxMs_;
  bool haveCmd_;
  uint8_t last_[udp::kTelemetrySize];
};

}  // namespace telemetry
