#pragma once
// Telemetry codec — abilities/telemetry (Arduino-free pure vocabulary).
//
// Aggregate Snapshot IN (filled by the telemetry task) + the snapshot encode.
// Byte math only: no heap, no sockets, no Arduino. Host tests pin the
// Snapshot->packet field map through the X-wire vectors.
//
// Include order: transport/udp_server.h top half (udpstatus — the single
// owner of packStatus/degToDecideg) before the encode below.

#include <stddef.h>
#include <stdint.h>

#include "transport/udp_codec.h"
#include "transport/udp_server.h"

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

}  // namespace telemetry
