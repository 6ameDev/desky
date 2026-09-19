#pragma once
// desky v2 UDP packet codec — Arduino-independent pure byte math.
//
// Shared by firmware (Phase 4 UDP server) and host Unity tests.
// Wire format (docs/architecture/v2.md §G): big-endian multi-byte,
// XOR checksum over all preceding bytes. No heap, no Arduino headers.

#include <stddef.h>
#include <stdint.h>

namespace udp {

// Control packet: App -> Robot @30-50Hz, 6 bytes.
struct ControlPacket {
  uint8_t mode = 0;
  uint8_t throttle = 0;
  uint8_t steering = 0;
  uint8_t flags = 0;
};

// Telemetry packet: Robot -> App @10-20Hz, 9 bytes.
// pitch/roll in decidegrees (0.1 deg), distance in mm.
struct TelemetryPacket {
  int16_t pitch = 0;
  int16_t roll = 0;
  uint16_t distanceMm = 0;
  uint8_t statusFlags = 0;
};

constexpr uint8_t kControlHeader = 0xAA;
constexpr uint8_t kTelemetryHeader = 0xBB;
constexpr size_t kControlSize = 6;
constexpr size_t kTelemetrySize = 9;

inline uint8_t checksum(const uint8_t* data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; ++i) {
    sum ^= data[i];
  }
  return sum;
}

inline bool encodeControl(const ControlPacket& pkt, uint8_t out[kControlSize]) {
  out[0] = kControlHeader;
  out[1] = pkt.mode;
  out[2] = pkt.throttle;
  out[3] = pkt.steering;
  out[4] = pkt.flags;
  out[5] = checksum(out, kControlSize - 1);
  return true;
}

inline bool decodeControl(const uint8_t* in, size_t len, ControlPacket& pkt) {
  if (in == nullptr || len != kControlSize) {
    return false;
  }
  if (in[0] != kControlHeader || checksum(in, kControlSize - 1) != in[kControlSize - 1]) {
    return false;
  }
  pkt.mode = in[1];
  pkt.throttle = in[2];
  pkt.steering = in[3];
  pkt.flags = in[4];
  return true;
}

inline void putI16BE(uint8_t* out, int16_t v) {
  out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[1] = static_cast<uint8_t>(v & 0xFF);
}

inline int16_t getI16BE(const uint8_t* in) { return static_cast<int16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]); }

inline bool encodeTelemetry(const TelemetryPacket& pkt, uint8_t out[kTelemetrySize]) {
  out[0] = kTelemetryHeader;
  putI16BE(out + 1, pkt.pitch);
  putI16BE(out + 3, pkt.roll);
  out[5] = static_cast<uint8_t>((pkt.distanceMm >> 8) & 0xFF);
  out[6] = static_cast<uint8_t>(pkt.distanceMm & 0xFF);
  out[7] = pkt.statusFlags;
  out[8] = checksum(out, kTelemetrySize - 1);
  return true;
}

inline bool decodeTelemetry(const uint8_t* in, size_t len, TelemetryPacket& pkt) {
  if (in == nullptr || len != kTelemetrySize) {
    return false;
  }
  if (in[0] != kTelemetryHeader || checksum(in, kTelemetrySize - 1) != in[kTelemetrySize - 1]) {
    return false;
  }
  pkt.pitch = getI16BE(in + 1);
  pkt.roll = getI16BE(in + 3);
  pkt.distanceMm = static_cast<uint16_t>((static_cast<uint16_t>(in[5]) << 8) | in[6]);
  pkt.statusFlags = in[7];
  return true;
}

}  // namespace udp
