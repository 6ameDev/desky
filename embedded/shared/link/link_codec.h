#pragma once
// B-link frame codec — header-only, Arduino-free, no heap (caller-owned buffers).
// Wire: [magic 0x4C | type u8 | seq u16BE | frag u16BE | len u16BE | payload | CRC16-CCITT-FALSE u16BE].
// Magic 0x4C is disjoint from all phone-wire headers; link type bytes live at offset 1.

#include <stddef.h>
#include <stdint.h>

#include "link/expression_catalog.h"

namespace link {

// ── Wire constants ──────────────────────────────────────────────────────────
constexpr uint8_t kMagic = 0x4C;
constexpr size_t kHeaderSize = 8;
constexpr size_t kCrcSize = 2;
constexpr size_t kOverhead = kHeaderSize + kCrcSize;
constexpr size_t kMaxPayload = 1024;
constexpr size_t kMaxFrame = kOverhead + kMaxPayload;

// ── Frame types ─────────────────────────────────────────────────────────────
// BUMP RULE (additive only): a new type takes the next free value. Never renumber, never change lengths.
constexpr uint8_t kTypeFace = 0x01;    // core->head, 1B face id u8.
constexpr uint8_t kTypeOled = 0x02;    // core->head, 1B mode 0=ON 1=DIM 2=OFF.
constexpr uint8_t kTypeCamCtl = 0x03;  // core->head, 1B 0=off 1=on.
// 0x04 QRBITMAP retired (never sent on the wire): do not reuse.
constexpr uint8_t kTypeOta = 0x05;       // core->head, 1..128B OTA pull URL bytes, no NUL.
constexpr uint8_t kTypePairShow = 0x06;  // core->head, 8B: 6 ASCII digits + valid-seconds u16BE.
constexpr uint8_t kTypePairDone = 0x07;  // core->head, empty: presence is the signal.
constexpr uint8_t kTypeExpr = 0x08;      // core->head, 1B expression id u8.
constexpr uint8_t kTypeJpeg = 0x10;      // 0..1024B raw JPEG bytes.
constexpr uint8_t kTypeHb = 0x20;        // head->core, 10B heartbeat + boot epoch.
constexpr uint8_t kTypeHbAck = 0x21;     // core->head, 5B heartbeat ack.
constexpr uint8_t kTypeAck = 0x22;       // either, 2B acked seq u16BE.

// Fixed payload lengths (variable: JPEG only).
constexpr size_t kFaceLen = 1;
constexpr size_t kOledLen = 1;
constexpr size_t kCamCtlLen = 1;
// URL grammar lives in ota_manifest.h; length only here.
constexpr size_t kOtaUrlMinLen = 1;
constexpr size_t kOtaUrlMaxLen = 128;
constexpr size_t kHbLen = 10;  // rssi i8 | channel u8 | freeHeap u32BE | bootId u32BE.
// 6 ASCII digits ('0'..'9') + valid-seconds u16BE (0 = already-expired).
constexpr size_t kPairShowLen = 8;
constexpr size_t kPairDoneLen = 0;
// Catalog ids only (QR excluded): unknown ids fail closed at unpack, the
// head gate resolves them to neutral downstream.
constexpr size_t kExprLen = 1;
constexpr size_t kHbAckLen = 5;  // channel u8 | era/resync u32BE.
constexpr size_t kAckLen = 2;    // acked seq u16BE.

constexpr uint8_t kOledOn = 0;
constexpr uint8_t kOledDim = 1;
constexpr uint8_t kOledOff = 2;
constexpr uint8_t kCamOff = 0;
constexpr uint8_t kCamOn = 1;

// ── Scalar helpers ──────────────
inline void putU16BE(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  out[1] = static_cast<uint8_t>(v & 0xFFu);
}

inline uint16_t getU16BE(const uint8_t* in) {
  return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | static_cast<uint16_t>(in[1]));
}

inline void putU32BE(uint8_t* out, uint32_t v) {
  out[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
  out[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  out[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  out[3] = static_cast<uint8_t>(v & 0xFFu);
}

inline uint32_t getU32BE(const uint8_t* in) {
  return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF); null input yields 0.
inline uint16_t crc16Ccitt(const uint8_t* data, size_t len) {
  if (data == nullptr) {
    return 0;
  }
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
    for (int b = 0; b < 8; ++b) {
      if ((crc & 0x8000u) != 0) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

// ── Type / length gate ──────────────────────────────────────────────────────
inline bool isKnownType(uint8_t type) {
  switch (type) {
    case kTypeFace:
    case kTypeOled:
    case kTypeCamCtl:
    case kTypeOta:
    case kTypePairShow:
    case kTypePairDone:
    case kTypeExpr:
    case kTypeJpeg:
    case kTypeHb:
    case kTypeHbAck:
    case kTypeAck:
      return true;
    default:
      return false;
  }
}

inline bool isValidPayloadLen(uint8_t type, uint16_t len) {
  switch (type) {
    case kTypeFace:
    case kTypeOled:
    case kTypeCamCtl:
    case kTypeExpr:
      return len == 1;
    case kTypeOta:
      return len >= kOtaUrlMinLen && len <= kOtaUrlMaxLen;
    case kTypePairShow:
      return len == kPairShowLen;
    case kTypePairDone:
      return len == kPairDoneLen;
    case kTypeJpeg:
      return len <= kMaxPayload;
    case kTypeHb:
      return len == kHbLen;
    case kTypeHbAck:
      return len == kHbAckLen;
    case kTypeAck:
      return len == kAckLen;
    default:
      return false;
  }
}

// Encodes one frame; returns encoded bytes, or 0 on any error.
inline size_t encodeFrame(uint8_t type, uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t fragIdx,
                          uint8_t fragTotal, uint8_t* out, size_t outLen) {
  if (out == nullptr) {
    return 0;
  }
  if (len > 0 && payload == nullptr) {
    return 0;
  }
  if (!isValidPayloadLen(type, len)) {
    return 0;
  }
  if (fragTotal == 0 || fragIdx >= fragTotal) {
    return 0;
  }
  const size_t total = kOverhead + len;
  if (outLen < total) {
    return 0;
  }
  out[0] = kMagic;
  out[1] = type;
  putU16BE(out + 2, seq);
  out[4] = fragIdx;
  out[5] = fragTotal;
  putU16BE(out + 6, len);
  for (uint16_t i = 0; i < len; ++i) {
    out[kHeaderSize + i] = payload[i];
  }
  const uint16_t crc = crc16Ccitt(out, kHeaderSize + len);
  putU16BE(out + kHeaderSize + len, crc);
  return total;
}

// View into the input buffer; valid only while it lives.
struct Frame {
  uint8_t type = 0;
  uint16_t seq = 0;
  uint8_t fragIdx = 0;
  uint8_t fragTotal = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload = nullptr;
};

inline bool decodeFrame(const uint8_t* buf, size_t len, Frame& out) {
  if (buf == nullptr || len < kOverhead) {
    return false;
  }
  if (buf[0] != kMagic) {
    return false;
  }
  const uint8_t type = buf[1];
  const uint16_t seq = getU16BE(buf + 2);
  const uint8_t fragIdx = buf[4];
  const uint8_t fragTotal = buf[5];
  const uint16_t payloadLen = getU16BE(buf + 6);
  if (len != kOverhead + payloadLen) {
    return false;
  }
  if (fragTotal == 0 || fragIdx >= fragTotal) {
    return false;
  }
  if (!isValidPayloadLen(type, payloadLen)) {
    return false;
  }
  const uint16_t want = getU16BE(buf + kHeaderSize + payloadLen);
  if (crc16Ccitt(buf, kHeaderSize + payloadLen) != want) {
    return false;
  }
  out.type = type;
  out.seq = seq;
  out.fragIdx = fragIdx;
  out.fragTotal = fragTotal;
  out.payloadLen = payloadLen;
  out.payload = buf + kHeaderSize;
  return true;
}

// Wrap-aware strictly-greater for u16 seq; a half-range diff is ambiguous and rejected.
inline bool isFreshSeq(uint16_t lastSeen, uint16_t got) {
  if (got == lastSeen) {
    return false;
  }
  return static_cast<uint16_t>(got - lastSeen) < 0x8000u;
}

// ── MTU / fragmentation ─────────────────────────────────────────────────────
inline size_t maxPayloadFor(size_t mtu) {
  if (mtu <= kOverhead) {
    return 0;
  }
  const size_t room = mtu - kOverhead;
  return room > kMaxPayload ? kMaxPayload : room;
}

inline bool needsFrag(size_t payloadLen, size_t mtu) { return payloadLen > maxPayloadFor(mtu); }

// fragTotal is u8: counts above 255 need larger per-frame payloads.
inline size_t fragCount(size_t payloadLen, size_t mtu) {
  const size_t per = maxPayloadFor(mtu);
  if (per == 0) {
    return 0;
  }
  size_t n = payloadLen / per;
  if (payloadLen % per != 0) {
    ++n;
  }
  return n == 0 ? 1 : n;  // An empty payload still rides one frame.
}

// ── Typed payload pack/unpack ───────────────────────────────────────────────
// HB (head->core, 10B): rssi i8 | channel u8 | freeHeap u32BE | bootId u32BE.
// bootId resyncs the seq epoch: never drop it, never default it to a constant.
inline bool packHb(int8_t rssi, uint8_t channel, uint32_t freeHeap, uint32_t bootId, uint8_t out[kHbLen]) {
  if (out == nullptr) {
    return false;
  }
  out[0] = static_cast<uint8_t>(rssi);
  out[1] = channel;
  putU32BE(out + 2, freeHeap);
  putU32BE(out + 6, bootId);
  return true;
}

inline bool unpackHb(const uint8_t* in, size_t len, int8_t& rssi, uint8_t& channel, uint32_t& freeHeap,
                     uint32_t& bootId) {
  if (in == nullptr || len != kHbLen) {
    return false;
  }
  rssi = static_cast<int8_t>(in[0]);
  channel = in[1];
  freeHeap = getU32BE(in + 2);
  bootId = getU32BE(in + 6);
  return true;
}

inline bool packHbAck(uint8_t channel, uint32_t era, uint8_t out[kHbAckLen]) {
  if (out == nullptr) {
    return false;
  }
  out[0] = channel;
  putU32BE(out + 1, era);
  return true;
}

inline bool unpackHbAck(const uint8_t* in, size_t len, uint8_t& channel, uint32_t& era) {
  if (in == nullptr || len != kHbAckLen) {
    return false;
  }
  channel = in[0];
  era = getU32BE(in + 1);
  return true;
}

inline bool packAck(uint16_t ackedSeq, uint8_t out[kAckLen]) {
  if (out == nullptr) {
    return false;
  }
  putU16BE(out, ackedSeq);
  return true;
}

inline bool unpackAck(const uint8_t* in, size_t len, uint16_t& ackedSeq) {
  if (in == nullptr || len != kAckLen) {
    return false;
  }
  ackedSeq = getU16BE(in);
  return true;
}

// 6 ASCII digits + valid-secs u16BE (0 = already-expired).
inline bool packPairShow(const uint8_t digits[6], uint16_t validSecs, uint8_t out[kPairShowLen]) {
  if (digits == nullptr || out == nullptr) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (digits[i] < '0' || digits[i] > '9') {
      return false;
    }
    out[i] = digits[i];
  }
  putU16BE(out + 6, validSecs);
  return true;
}

inline bool unpackPairShow(const uint8_t* in, size_t len, uint8_t digitsOut[6], uint16_t& validSecs) {
  if (in == nullptr || digitsOut == nullptr || len != kPairShowLen) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (in[i] < '0' || in[i] > '9') {
      return false;
    }
    digitsOut[i] = in[i];
  }
  validSecs = getU16BE(in + 6);
  return true;
}

inline bool packOled(uint8_t mode, uint8_t& wireOut) {
  switch (mode) {
    case kOledOn:
    case kOledDim:
    case kOledOff:
      wireOut = mode;
      return true;
    default:
      return false;
  }
}

// Wire 0=ON,1=DIM,2=OFF inverts head DisplayState order: never pass DisplayState raw.
inline bool unpackOled(uint8_t wire, uint8_t& modeOut) {
  switch (wire) {
    case kOledOn:
    case kOledDim:
    case kOledOff:
      modeOut = wire;
      return true;
    default:
      return false;
  }
}

inline bool packCamCtl(uint8_t mode, uint8_t& wireOut) {
  switch (mode) {
    case kCamOff:
    case kCamOn:
      wireOut = mode;
      return true;
    default:
      return false;
  }
}

inline bool unpackCamCtl(uint8_t wire, uint8_t& modeOut) {
  switch (wire) {
    case kCamOff:
    case kCamOn:
      modeOut = wire;
      return true;
    default:
      return false;
  }
}

// FACE ids pass through; out-of-range clamps downstream.
inline bool unpackFace(uint8_t wire, uint8_t& idOut) {
  idOut = wire;
  return true;
}

// EXPR ids validate against the shared catalog; unknown ids (including QR)
// fail closed here and resolve to neutral at the head gate.
inline bool packExpr(uint8_t id, uint8_t& wireOut) {
  if (exprcat::lookup(id) == nullptr) {
    return false;
  }
  wireOut = id;
  return true;
}

inline bool unpackExpr(uint8_t wire, uint8_t& idOut) {
  if (exprcat::lookup(wire) == nullptr) {
    return false;
  }
  idOut = wire;
  return true;
}

// Strict in-order append into a caller buffer; rejects leave state untouched.
class Reassembler {
 public:
  Reassembler() : buf_(nullptr), cap_(0), len_(0), total_(0), next_(0) {}

  void reset(uint8_t* buf, size_t cap) {
    buf_ = buf;
    cap_ = cap;
    len_ = 0;
    total_ = 0;
    next_ = 0;
  }

  bool push(uint8_t fragIdx, uint8_t fragTotal, const uint8_t* payload, uint16_t payloadLen) {
    if (buf_ == nullptr) {
      return false;
    }
    if (payloadLen > 0 && payload == nullptr) {
      return false;
    }
    if (fragTotal == 0 || fragIdx >= fragTotal) {
      return false;
    }
    if (next_ == 0) {
      if (fragIdx != 0) {
        return false;
      }
      total_ = fragTotal;
      len_ = 0;
    } else {
      if (fragTotal != total_) {
        return false;
      }
      if (fragIdx != next_) {
        return false;
      }
    }
    if (len_ + payloadLen > cap_) {
      return false;
    }
    for (uint16_t i = 0; i < payloadLen; ++i) {
      buf_[len_ + i] = payload[i];
    }
    len_ += payloadLen;
    ++next_;
    return true;
  }

  bool complete() const { return next_ > 0 && next_ == total_; }

  size_t len() const { return len_; }

  uint8_t next() const { return next_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t len_;
  uint8_t total_;
  uint8_t next_;
};

}  // namespace link
