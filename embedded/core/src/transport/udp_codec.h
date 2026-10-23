#pragma once
// UDP packet codec — Arduino-independent pure byte math.
//
// Shared by firmware and host Unity tests.
// Wire format: big-endian multi-byte,
// XOR checksum over all preceding bytes. No heap, no Arduino headers.

#include <stddef.h>
#include <stdint.h>

// ── Frozen wire table ─────────────────────────
// Normative freeze (shared/protocol/README.md §"Pairing + authenticated
// drive v1"):
//   0xAA /  6  Control     App->Robot  [hdr|mode|thr|str|flags|XOR]
//   0xBB /  9  Telemetry   Robot->App  [hdr|pitchBE|rollBE|distBE|flags|XOR]
//   0x50 / 12  PAIR_REQ    App->Robot  [hdr|code6|cliNonce4|XOR]
//   0x51 / 38  PAIR_OK v1  Robot->App  [hdr|srvNonce4|ct16|tag16|XOR]
//   0x51 / 39  PAIR_OK v1.1 Robot->App [hdr|srvNonce4|ct17|tag16|XOR]
//   0x52 /  4  PAIR_ERR    Robot->App  [hdr|reason|retry_s|XOR]
//   0xAC / 16  DRIVE       App->Robot  [hdr|mode|thr|str|flags|ctrBE4|tag6|XOR]
//   0xAD / 12  UNPAIR      App->Robot  [hdr|ctrBE4|tag6|XOR]
//   0x53 / 14  PAIR_REFRESH_REQ App->Robot [hdr|eraBE2|ctrBE4|tag6|XOR]
//   0x54 / 10  PAIR_REFRESH_ACK Robot->App [hdr|eraBE2|tag6|XOR]
//   0x55 / 14  ALIVE       App->Robot  [hdr|eraBE2|ctrBE4|tag6|XOR]
//   0x56 / 14  DISCONNECT  Both        [hdr|eraBE2|ctrBE4|tag6|XOR]
//     (same layout; App->Robot goodbye keeps the pair, Robot->App
//     pair-button eviction clears it; fire-and-forget, no retry)
// Reserved: claimed headers are never reused or renumbered, sizes never
// change, reserved bytes never repurposed — new messages claim a fresh
// header. Deliberate stdint-only duplication: this table is mirrored (not
// included) in Codec.kt / README; pairing_codec.h and the udpstatus top half
// of udp_server.h alias these constants.

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

// Phone-wire pairing vocabulary, single home (frozen spec:
// shared/protocol/README.md §"Pairing + authenticated drive v1"):
//   PAIR_REQ   App->Robot 12B [0x50|code6|cliNonce4|XOR]
//   PAIR_OK v1.1 Robot->App 39B [0x51|srvNonce4|ct17|tag16|XOR]
//   UNPAIR     App->Robot 12B [0xAD|ctrBE4|tag6|XOR]
//   PAIR_REFRESH_REQ  App->Robot 14B [0x53|eraBE2|ctrBE4|tag6|XOR]
//   PAIR_REFRESH_ACK  Robot->App 10B [0x54|eraBE2|tag6|XOR]
//   ALIVE      App->Robot 14B [0x55|eraBE2|ctrBE4|tag6|XOR]
//   DISCONNECT Both 14B [0x56|eraBE2|ctrBE4|tag6|XOR] (same layout;
//     App->Robot goodbye keeps the pair, Robot->App eviction clears it)
// pairing_codec.h and the udpstatus top half of udp_server.h alias these;
// never re-literalize them elsewhere.
constexpr uint8_t kPairReqHeader = 0x50;
constexpr size_t kPairReqSize = 12;
constexpr size_t kPairOkSize = 39;
constexpr uint8_t kUnpairHeader = 0xAD;
constexpr size_t kUnpairSize = 12;
constexpr size_t kUnpairTagLen = 6;
constexpr size_t kUnpairMsgLen = 10;  // "UNPAIR" (6 ASCII) || counterBE4
constexpr uint8_t kPairRefreshReqHeader = 0x53;
constexpr size_t kPairRefreshReqSize = 14;
constexpr uint8_t kPairRefreshAckHeader = 0x54;
constexpr size_t kPairRefreshAckSize = 10;
constexpr uint8_t kAliveHeader = 0x55;
constexpr size_t kAliveSize = 14;
constexpr uint8_t kDisconnectHeader = 0x56;
constexpr size_t kDisconnectSize = 14;

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

// Command event payload vocabulary: EVENT_UDP_COMMAND_RECEIVED packs a
// ControlPacket into the SystemEvent uint32 big-endian:
//   payload = mode<<24 | throttle<<16 | steering<<8 | flags.
// Throttle/steering bytes are uint8 center-128:
//   signed = (byte - 128) / 127, clamped to -1..1. Zero stick = 128.
// Single home for the byte math shared by the transport publisher and the
// behavior drain.

// UDP stick byte (center-128) -> unit (-1..1). Exact: 128 maps to 0.0f,
// 255 to +1.0, 0 clamps to -1.0.
inline float byteToUnit(uint8_t raw) {
  float f = (static_cast<float>(static_cast<int>(raw) - 128)) / 127.0f;
  if (f > 1.0f) {
    return 1.0f;
  }
  if (f < -1.0f) {
    return -1.0f;
  }
  return f;
}

// Unit (-1..1) -> stick byte, inverse of byteToUnit (for tests/synthetic).
inline uint8_t unitToByte(float v) {
  if (v > 1.0f) {
    v = 1.0f;
  }
  if (v < -1.0f) {
    v = -1.0f;
  }
  int b = static_cast<int>(v * 127.0f + 128.0f + (v >= 0.0f ? 0.5f : -0.5f));
  if (b > 255) {
    return 255;
  }
  if (b < 0) {
    return 0;
  }
  return static_cast<uint8_t>(b);
}

// Big-endian pack: payload = mode<<24|throttle<<16|steering<<8|flags.
inline uint32_t packCommand(uint8_t mode, uint8_t throttle, uint8_t steering, uint8_t flags) {
  return (static_cast<uint32_t>(mode) << 24) | (static_cast<uint32_t>(throttle) << 16) |
         (static_cast<uint32_t>(steering) << 8) | static_cast<uint32_t>(flags);
}

inline void unpackCommand(uint32_t payload, uint8_t& mode, float& v, float& omega, uint8_t& flags) {
  mode = static_cast<uint8_t>((payload >> 24) & 0xFF);
  v = byteToUnit(static_cast<uint8_t>((payload >> 16) & 0xFF));
  omega = byteToUnit(static_cast<uint8_t>((payload >> 8) & 0xFF));
  flags = static_cast<uint8_t>(payload & 0xFF);
}

}  // namespace udp
