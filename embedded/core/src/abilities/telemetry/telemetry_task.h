#pragma once
// Telemetry task — abilities/telemetry (RTOS/main-loop glue).
//
// Firmware-only consumer binder: owns the telemetry snapshot fill + TX pacing.
// Transport (UdpServer) keeps the dumb pipes: peer/alive state, the gate
// verdict + silence logging, and the sendTelemetry(bytes) unicast.
// Policy/consumption lives here with the telemetry-ability-owned
// TelemetryManager; behavior owns drive intent (Coordinator::snapshot).
// Same 15 Hz cadence with same payloads.
//
// Driven from the main loop (poll, non-blocking): the outer cadence gate
// replicates the old transport loop tick exactly (the cadence clock advances
// on every due tick even when the gate is silent).

// udp_server.h first: its top half owns udpstatus (which
// telemetry_codec.h requires) and its bottom half is the UdpServer type.
#include "transport/udp_server.h"

#ifdef ARDUINO
// ── Firmware: telemetry consumer (snapshot + publish, main loop) ────────────

#include <Arduino.h>

#include "abilities/sensors/sensors_codec.h"
#include "abilities/telemetry/telemetry_manager.h"
#include "behavior/coordinator.h"
#include "common/fault_manager.h"
#include "hal/mpu6500_driver.h"
#include "hal/vl53l0x_driver.h"
#include "transport/udp_codec.h"

class TelemetryTask {
 public:
  TelemetryTask() : coord_(nullptr), tof_(nullptr), mpu_(nullptr), telem_(nullptr), udp_(nullptr), lastTxMs_(0) {}

  // Pins the snapshot sources + the setup-owned manager + the transport dumb
  // pipe. Call once in setup(), after UdpServer::begin, with logging up.
  bool begin(Coordinator* coord, Vl53l0xDriver* tof, Mpu6500Driver* mpu, telemetry::TelemetryManager* telem,
             UdpServer* udp) {
    DESKY_ASSERT(coord != nullptr);
    DESKY_ASSERT(tof != nullptr);
    DESKY_ASSERT(mpu != nullptr);
    DESKY_ASSERT(telem != nullptr);
    DESKY_ASSERT(udp != nullptr);
    if (coord == nullptr || tof == nullptr || mpu == nullptr || telem == nullptr || udp == nullptr) {
      return false;
    }
    coord_ = coord;
    tof_ = tof;
    mpu_ = mpu;
    telem_ = telem;
    udp_ = udp;
    return true;
  }

  // Non-blocking poll from the main loop (call every loop): snapshot fill ->
  // encode -> transport gate -> manager publish. At most one unicast per
  // CFG_TELEMETRY_HZ tick; silent otherwise.
  void poll(uint32_t nowMs) {
    if (coord_ == nullptr || tof_ == nullptr || mpu_ == nullptr || telem_ == nullptr || udp_ == nullptr) {
      return;
    }
    // Outer cadence (replicates the old transport loop tick): the clock
    // advances on every due tick even when the gate below stays silent.
    // Unsigned subtraction: wrap-safe across millis() rollover.
    if (nowMs - lastTxMs_ < telem_->rateMs()) {
      return;
    }
    lastTxMs_ = nowMs;

    // Coordinator-owned intent (mode/active/cliff level); defaults hold if
    // the snapshot ever fails (boot race).
    SystemState snap;
    coord_->snapshot(snap);

    // Live sensors -> fusion on demand: real tilt once the MPU is healthy, 0
    // before.
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

    // Driving = telemetry-ability-owned RX observation (last commanded stick nonzero
    // AND fresh). The cliff bit mirrors the coordinator level, which the
    // sensor task keeps live.
    const bool driving = telem_->isDriving(nowMs);
    telemetry::Snapshot tsnap;
    tsnap.mode = static_cast<uint8_t>(snap.mode);
    tsnap.cliff = snap.cliffDetected;
    tsnap.driving = driving;
    tsnap.pitchDeg = pitchDeg;
    tsnap.rollDeg = rollDeg;
    tsnap.distMm = distMm;
    udp::TelemetryPacket tp;
    telemetry::encodeSnapshot(tsnap, tp);

    // Transport gate (paired + peer pinned + alive-fresh, with the silence
    // logging) + manager cadence/edge; bytes leave through the transport's
    // dumb unicast.
    const bool gateOk = udp_->telemetryGate(nowMs);
    telem_->publish(tp, gateOk, nowMs, [this](const uint8_t* buf, size_t len) { udp_->sendTelemetry(buf, len); });
  }

 private:
  Coordinator* coord_;
  Vl53l0xDriver* tof_;
  Mpu6500Driver* mpu_;
  telemetry::TelemetryManager* telem_;
  UdpServer* udp_;
  uint32_t lastTxMs_;
};

#endif  // ARDUINO
