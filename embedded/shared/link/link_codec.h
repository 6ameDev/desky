#pragma once
// desky v2 core<->head link frame codec (B-link) — header-only, Arduino-free.
//
// Pure byte math shared by firmware (future socket transport) and host Unity
// tests. Wire format (all multi-byte big-endian):
//   [magic 0x4C | type u8 | seq u16BE | frag u16BE | len u16BE | payload | CRC16BE]
// Frag packing: hi byte = fragment index, lo byte = fragment total, i.e.
//   frag = (idx << 8) | total. Unfragmented frames use idx=0, total=1.
// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over all preceding bytes.
//
// Phone-wire headers (0xAA/0xBB/0x50/0x51/0x52/0xAC/0xAD/0x53/0x54/0x55/0x56)
// are NEVER reused on this wire: the link magic is 0x4C ('L') and link type
// bytes live at offset 1, so no phone-wire parser can ever accept a link
// frame and vice versa. ESP-NOW is NOT a transport for this codec —
// sockets only, per the approved B-link decision.
//
// No heap, no Arduino headers (stdint/stddef only); fixed caller-owned
// buffers at every call site so host Unity tests include this directly.

#include <stddef.h>
#include <stdint.h>

namespace link {

// ── Wire constants ──────────────────────────────────────────────────────────
constexpr uint8_t kMagic = 0x4C;  // 'L': frame sync byte, never a phone-wire header.
constexpr size_t kHeaderSize = 8;
constexpr size_t kCrcSize = 2;
constexpr size_t kOverhead = kHeaderSize + kCrcSize;   // 10: header + trailing CRC.
constexpr size_t kMaxPayload = 1024;                   // Max single-frame payload bytes.
constexpr size_t kMaxFrame = kOverhead + kMaxPayload;  // 1034: largest single frame.

// ── Frame types ─────────────────────────────────────────────────────────────
// Directions: core->head display/control/pairing, head->core heartbeat,
// either ACK.
//
// BUMP RULE (additive only, both sides together): a new type takes the next
// free value, gets a length contract below, and joins isKnownType +
// isValidPayloadLen + both typeName tables (core link_server.h, head
// wifi_link.h). Existing values are NEVER renumbered and existing length
// contracts NEVER change — every golden vector below stays byte-identical.
// A peer that predates the bump rejects the new type as unknown (the frozen
// decode gate), so mixed fleets degrade to drop-as-unknown, never misparse.
constexpr uint8_t kTypeFace = 0x01;      // core->head, 1B: face id u8 (clamped downstream).
constexpr uint8_t kTypeOled = 0x02;      // core->head, 1B: mode 0=ON 1=DIM 2=OFF.
constexpr uint8_t kTypeCamCtl = 0x03;    // core->head, 1B: 0=off 1=on.
constexpr uint8_t kTypeQrBitmap = 0x04;  // RESERVED (superseded by PAIRSHOW/PAIRDONE — frozen table,
// never reuse the header): still 63B V1 21x21 packed rows, MSB-first.
constexpr uint8_t kTypeOta = 0x05;  // core->head, 1..128B: OTA pull URL bytes, no NUL.
// PAIRSHOW (Pre-2 pairing-display path): the core sends the 6 digits +
// validity window and the head renders them (replacing the 63B QRBITMAP
// push). Log names are wired in both typeName tables (core link_server.h,
// head wifi_link.h); length/known gates live here.
constexpr uint8_t kTypePairShow = 0x06;  // core->head, 8B: 6 ASCII digits + valid-seconds u16BE.
constexpr uint8_t kTypePairDone = 0x07;  // core->head, empty: pairing complete.
constexpr uint8_t kTypeJpeg = 0x10;      // direction-free framing, 0..1024B: raw JPEG bytes
                                         // (actual use: head->core stills over TCP; head drops
                                         // inbound JPEG on UDP).
constexpr uint8_t kTypeHb = 0x20;        // head->core, 10B: heartbeat + boot epoch.
constexpr uint8_t kTypeHbAck = 0x21;     // core->head, 5B: heartbeat ack.
constexpr uint8_t kTypeAck = 0x22;       // either, 2B: acked seq u16BE.

// Fixed payload lengths (variable: JPEG only).
constexpr size_t kFaceLen = 1;
constexpr size_t kOledLen = 1;
constexpr size_t kCamCtlLen = 1;
// QR bitmap: V1 21x21, 3 packed bytes/row x 21 rows = 63B, row-major
// MSB-first. Generic bytes only: the codec never includes qr_pairing.h —
// the 63B length check is the only coupling to the QR encoder.
constexpr size_t kQrBitmapLen = 63;
// OTA URL: raw URL bytes (no NUL terminator), 1..128B. The grammar above the
// length (printable ASCII, no '@'/whitespace) lives once in
// link/ota_manifest.h (otamanifest::isValidOtaUrlBytes) so both sides call
// it — never mirrored here. Range shape mirrors the JPEG contract (bounded
// variable length); 128B fits any http://host[:port]/path the carrier uses.
constexpr size_t kOtaUrlMinLen = 1;
constexpr size_t kOtaUrlMaxLen = 128;
constexpr size_t kHbLen = 10;  // rssi i8 | channel u8 | freeHeap u32BE | bootId u32BE.
// PAIRSHOW (core->head pairing-code display, 8B): 6 ASCII digits ('0'..'9')
// + valid-seconds u16BE (0 = already-expired display). The digit vocabulary
// lives once in packPairShow/unpackPairShow below so both sides call it —
// never mirrored. The frame codec above checks length only, never values.
constexpr size_t kPairShowLen = 8;
// PAIRDONE (core->head pairing complete, empty): no payload — the frame
// presence is the signal. A fixed-empty contract (not a 0..N range) so any
// stowaway byte fails the decode gate.
constexpr size_t kPairDoneLen = 0;
// bootId is a per-boot random epoch (esp_random at head startup): the core
// accepts ANY seq on a bootId change (fresh epoch resync) and gates
// strictly-greater only within one bootId. Without the epoch, a head reboot
// (or re-assoc with txSeq reset) replays low seqs the core drops as stale
// forever — an unrecoverable 9s flap loop (bench-proven 2026-10-18). Replays
// of an old epoch cost one idempotent HB_ACK (counters only); the transport
// stays keyless by design, same trust as the AP passphrase.
constexpr size_t kHbAckLen = 5;  // channel u8 | era/resync u32BE.
constexpr size_t kAckLen = 2;    // acked seq u16BE.

// OLED modes / camera states. Value validation lives in the pack/unpack
// helpers below (OLED/CAMCTL accept exactly their wire sets; FACE rides
// map-then-clamp with its range decision documented on unpackFace); the
// frame codec above checks lengths only, never values.
constexpr uint8_t kOledOn = 0;
constexpr uint8_t kOledDim = 1;
constexpr uint8_t kOledOff = 2;
constexpr uint8_t kCamOff = 0;
constexpr uint8_t kCamOn = 1;

// ── Scalar helpers (mirror the udp::checksum / putI16BE idiom) ──────────────
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

// CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, no refin/refout, no xorout.
// Null-safe: null input yields 0 (never dereferenced).
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
    case kTypeQrBitmap:
    case kTypeOta:
    case kTypePairShow:
    case kTypePairDone:
    case kTypeJpeg:
    case kTypeHb:
    case kTypeHbAck:
    case kTypeAck:
      return true;
    default:
      return false;
  }
}

// Per-type payload contract. Fixed types demand their exact length (so a
// 62B or 64B QRBITMAP is rejected); JPEG accepts 0..kMaxPayload. Unknown
// types are rejected — the contract is frozen, new types require a codec
// bump on both sides.
inline bool isValidPayloadLen(uint8_t type, uint16_t len) {
  switch (type) {
    case kTypeFace:
    case kTypeOled:
    case kTypeCamCtl:
      return len == 1;
    case kTypeQrBitmap:
      return len == kQrBitmapLen;
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

// ── Frame encode / decode ───────────────────────────────────────────────────
// Encodes one frame into the caller-owned `out` buffer. Returns the total
// encoded bytes (kOverhead + len), or 0 on any error: null out, null payload
// with len > 0, unknown type, per-type length violation, fragTotal == 0,
// fragIdx >= fragTotal, or `outLen` too small. Never allocates.
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

// Decoded view into the input buffer: `payload` points at `buf` (no copy),
// valid only while the input buffer lives.
struct Frame {
  uint8_t type = 0;
  uint16_t seq = 0;
  uint8_t fragIdx = 0;
  uint8_t fragTotal = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload = nullptr;
};

// Validates magic, exact length (no truncation, no trailing garbage), frag
// bounds, per-type length, then CRC. Null-safe. Returns false (leaving `out`
// untouched) on any failure.
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

// Wrap-aware strictly-greater for the u16 frame seq (mirrors the
// udpstatus::isFreshCounter idiom, but u16: the difference is cast back to
// uint16_t because C++ promotes u16 operands to int). A diff of exactly half
// the range is direction-ambiguous and rejected.
inline bool isFreshSeq(uint16_t lastSeen, uint16_t got) {
  if (got == lastSeen) {
    return false;
  }
  return static_cast<uint16_t>(got - lastSeen) < 0x8000u;
}

// ── MTU / fragmentation ─────────────────────────────────────────────────────
// The transport may impose an MTU smaller than kMaxFrame; per-frame payloads
// must then shrink. maxPayloadFor reports the largest payload that fits `mtu`
// (capped at kMaxPayload; 0 when even the header+CRC do not fit).
inline size_t maxPayloadFor(size_t mtu) {
  if (mtu <= kOverhead) {
    return 0;
  }
  const size_t room = mtu - kOverhead;
  return room > kMaxPayload ? kMaxPayload : room;
}

// True when `payloadLen` does not fit one `mtu`-sized frame and the transport
// must split it across fragTotal = fragCount(...) frames.
inline bool needsFrag(size_t payloadLen, size_t mtu) { return payloadLen > maxPayloadFor(mtu); }

// Number of frames needed to carry `payloadLen` bytes at `mtu` (0 when the
// MTU cannot carry even the header+CRC). fragTotal is a u8, so counts above
// 255 require larger per-frame payloads (up to kMaxPayload).
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
// bootId note above: NEVER drop it, never default it to a constant — a
// constant bootId reintroduces the stale wedge across head reboots.
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

// HB_ACK (core->head, 5B): channel u8 | era/resync u32BE.
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

// ACK (either direction, 2B): acked seq u16BE.
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

// PAIRSHOW (core->head, 8B): 6 ASCII pairing digits + valid-seconds u16BE.
// Digit validation is fail-closed both ways (a non-digit rejects, mirroring
// qr::encode6Digits); validSecs rides raw u16BE (0 = already-expired).
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

// ── Command value vocabulary (OLED/CAMCTL/FACE) ────────────────────────────
// The wire values ARE the vocabulary: pack/unpack are identity-with-
// validation (wire byte in, same byte out), the entity's job is single-point
// validation + documentation so no consumer hand-rolls the mapping again.
// Both sides MUST call these — the future core stageFace/stageOled/stageCam
// producers on the way out, the head mapCmd consumer on the way in. A
// renumber is a deliberate test-updating decision: literal-byte tests pin the
// values on both sides (core test_link_cmd_vocab.cpp, head
// test_command_literals.cpp).
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

// NOTE (documented once, here — do not re-derive elsewhere): the wire order
// 0=ON,1=DIM,2=OFF is the INVERSE of the head DisplayState order
// (OLED_OFF=0,OLED_DIM=1,OLED_ON=2 in head_context.h). unpackOled validates
// the wire byte only and returns it unchanged — the wire->DisplayState flip
// happens in the consumer (head mapCmd), never by re-deriving constants here.
// Passing a DisplayState raw puts inverted values on the wire.
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

// FACE ids ride the wire as the raw id byte: the full u8 domain maps
// (identity passthrough, clamped downstream by the head's clampFace). This
// preserves the map-then-clamp contract — mapCmd accepts every 1B FACE frame
// and apply resolves out-of-range ids to BOOT, so accept/reject here never
// drifts from the consumer. Range decision mirrored from head_context.h:
// FACE_BOOT(0)..FACE_ALERT(4) are the display ids; FACE_QR(5) is excluded (QR
// mode is entered only via showQr, never via a FACE id) and anything at/above
// FACE_COUNT(6) falls back to BOOT downstream. No head include here — the
// codec stays stdint-only; this comment is the single reference for the
// mirrored range. Always true: unlike OLED/CAMCTL there is no wire-vs-
// internal inversion for FACE (valid ids match 1:1), so no byte is
// rejectable at this seam.
inline bool unpackFace(uint8_t wire, uint8_t& idOut) {
  idOut = wire;
  return true;
}

// ── Fragment reassembly (append-only, caller-owned buffer) ──────────────────
// Strict in-order append. push() accepts a fragment only when its index is
// exactly the next expected one, its total matches the ongoing message, and
// its bytes fit the caller buffer. Everything else is rejected WITHOUT
// mutating state:
//   - duplicate (idx < next) or replayed tail: reject (already-written bytes
//     are never overwritten, so overlap is impossible by construction);
//   - gap / out-of-order (idx > next): reject (no sparse window);
//   - total mismatch vs the first fragment: reject;
//   - buffer overflow: reject.
// The first accepted fragment must carry idx 0; it latches the total.
// Single-frame messages (idx=0, total=1) complete after one push.
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

  uint8_t total() const { return total_; }

  uint8_t next() const { return next_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t len_;
  uint8_t total_;
  uint8_t next_;
};

}  // namespace link
