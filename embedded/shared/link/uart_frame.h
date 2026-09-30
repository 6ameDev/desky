#pragma once
// UART POC frame codec — Arduino-free (stdint/stddef only), header-only.
//
// ESP<->ESP talk-wire transport for the poc-comm-link spike. Wire format:
//
//   [0..1]  magic 'D' 'S' (0x44 0x53)
//   [2]     MsgType: CMD=0x01 RESP=0x02 CHUNK=0x03 HB=0x04
//   [3]     Flags: LAST_CHUNK=1<<0, SYNTHETIC=1<<1
//   [4..5]  FrameID u16 LE, [6..7] ChunkIdx u16 LE, [8..9] PayloadLen u16 LE
//   [10..11] HeaderCRC16 u16 LE = CRC16-CCITT (poly 0x1021, init 0xFFFF) over [0..9]
//   [12..]  Payload[PayloadLen]
//   [..]    Tail u32 LE = CRC32-IEEE (poly 0xEDB88320, init/xor 0xFFFFFFFF) over payload only
//
// Endianness note: all multi-byte fields are LITTLE-ENDIAN. This is a
// deliberate deviation from middleware/udp_codec.h (big-endian) — the UDP
// codec is the robot<->app contract, this codec is ESP<->ESP only.
//
// Guarantees: no heap, no Arduino, no String. kMaxPayload = 1024.

#include <stddef.h>
#include <stdint.h>

namespace uartpoc {

constexpr uint8_t kMagic0 = 0x44;  // 'D'
constexpr uint8_t kMagic1 = 0x53;  // 'S'
constexpr size_t kHeaderLen = 12;
constexpr size_t kTailLen = 4;
constexpr size_t kMaxPayload = 1024;
constexpr size_t kMaxFrameLen = kHeaderLen + kMaxPayload + kTailLen;  // 1040

enum MsgType : uint8_t { MSG_CMD = 0x01, MSG_RESP = 0x02, MSG_CHUNK = 0x03, MSG_HB = 0x04 };

enum FrameFlag : uint8_t { FLAG_LAST_CHUNK = 0x01, FLAG_SYNTHETIC = 0x02 };

inline void putU16LE(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline uint16_t getU16LE(const uint8_t* in) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8));
}

inline void putU32LE(uint8_t* out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline uint32_t getU32LE(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

// CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF. Check: "123456789" -> 0x29B1.
inline uint16_t crc16Ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
    for (uint8_t b = 0; b < 8; ++b) {
      if ((crc & 0x8000) != 0) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

// CRC32-IEEE: poly 0xEDB88320 (reflected), init/xorout 0xFFFFFFFF.
// Check: "123456789" -> 0xCBF43926. Empty input -> 0x00000000.
inline uint32_t crc32Ieee(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      if ((crc & 1U) != 0) {
        crc = (crc >> 1) ^ 0xEDB88320UL;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

// Serialize one frame. Payload may be nullptr when payloadLen == 0.
// Returns false (outLen untouched) when payloadLen > kMaxPayload or out too small.
inline bool encodeFrame(uint8_t type, uint8_t flags, uint16_t frameId, uint16_t chunkIdx, const uint8_t* payload,
                        uint16_t payloadLen, uint8_t* out, size_t outCap, size_t& outLen) {
  if (out == nullptr || payloadLen > kMaxPayload) {
    return false;
  }
  if (payloadLen > 0 && payload == nullptr) {
    return false;
  }
  if (outCap < kHeaderLen + payloadLen + kTailLen) {
    return false;
  }
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = type;
  out[3] = flags;
  putU16LE(out + 4, frameId);
  putU16LE(out + 6, chunkIdx);
  putU16LE(out + 8, payloadLen);
  putU16LE(out + 10, crc16Ccitt(out, 10));
  for (uint16_t i = 0; i < payloadLen; ++i) {
    out[kHeaderLen + i] = payload[i];
  }
  // Empty payload: CRC over zero bytes (== 0); payload pointer never dereferenced.
  const uint8_t* payPtr = (payloadLen > 0) ? payload : out;
  putU32LE(out + kHeaderLen + payloadLen, crc32Ieee(payPtr, payloadLen));
  outLen = kHeaderLen + payloadLen + kTailLen;
  return true;
}

enum class DecodeStatus : uint8_t { NEED_MORE, OK, ERR_HDR_CRC, ERR_PAY_CRC };

struct DecodedFrame {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t frameId = 0;
  uint16_t chunkIdx = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload = nullptr;  // Into Decoder storage; valid until next feed()/reset().
};

// Streaming byte-frame decoder. Feed any slice (even 1 byte, even nullptr/0 to
// pump buffered bytes after an ERR); exactly one status per call. Garbage
// before magic and overlong length gates are dropped SILENTLY (resync by one
// byte, keep scanning); CRC failures surface as ERR_* once, with the bad
// candidate's first byte already dropped. On ERR_*, `consumed` may be < n —
// the caller must re-feed data + consumed with the remainder.
class Decoder {
 public:
  Decoder() : len_(0), frameTotal_(0), held_(0) {}

  void reset() {
    len_ = 0;
    frameTotal_ = 0;
    held_ = 0;
  }

  DecodeStatus feed(const uint8_t* data, size_t n, size_t& consumed, DecodedFrame& out) {
    consumed = 0;
    if (data == nullptr && n > 0) {
      return DecodeStatus::NEED_MORE;
    }
    if (held_ > 0) {  // Drop the previously delivered frame before touching new bytes.
      size_t drop = (held_ < len_) ? held_ : len_;
      shift_(drop);
      held_ = 0;
      frameTotal_ = 0;
    }
    for (;;) {
      if (frameTotal_ == 0) {
        while (len_ < kHeaderLen && consumed < n) {
          buf_[len_++] = data[consumed++];
        }
        if (len_ < kHeaderLen) {
          return DecodeStatus::NEED_MORE;
        }
        size_t pos = 0;
        bool found = false;
        for (; pos + 1 < len_; ++pos) {
          if (buf_[pos] == kMagic0 && buf_[pos + 1] == kMagic1) {
            found = true;
            break;
          }
        }
        if (!found) {  // No magic: drop all but a possible trailing partial magic.
          size_t keep = (len_ > 0 && buf_[len_ - 1] == kMagic0) ? 1 : 0;
          if (keep == 1) {
            buf_[0] = kMagic0;
          }
          len_ = keep;
          if (consumed >= n) {
            return DecodeStatus::NEED_MORE;
          }
          continue;
        }
        if (pos > 0) {  // Leading garbage: drop silently, re-scan.
          shift_(pos);
          continue;
        }
        const uint16_t plen = getU16LE(buf_ + 8);
        if (plen > kMaxPayload) {  // Overlong: silent drop, resync by one byte.
          shift_(1);
          continue;
        }
        if (crc16Ccitt(buf_, 10) != getU16LE(buf_ + 10)) {
          shift_(1);
          return DecodeStatus::ERR_HDR_CRC;
        }
        frameTotal_ = kHeaderLen + plen + kTailLen;
      }
      while (len_ < frameTotal_ && consumed < n) {
        buf_[len_++] = data[consumed++];
      }
      if (len_ < frameTotal_) {
        return DecodeStatus::NEED_MORE;
      }
      const uint16_t plen = getU16LE(buf_ + 8);
      const uint8_t* pay = buf_ + kHeaderLen;
      if (crc32Ieee(pay, plen) != getU32LE(buf_ + kHeaderLen + plen)) {
        shift_(1);
        frameTotal_ = 0;
        return DecodeStatus::ERR_PAY_CRC;
      }
      out.type = buf_[2];
      out.flags = buf_[3];
      out.frameId = getU16LE(buf_ + 4);
      out.chunkIdx = getU16LE(buf_ + 6);
      out.payloadLen = plen;
      out.payload = pay;
      held_ = frameTotal_;
      frameTotal_ = 0;
      return DecodeStatus::OK;
    }
  }

 private:
  void shift_(size_t drop) {
    if (drop >= len_) {
      len_ = 0;
      return;
    }
    for (size_t i = 0; i + drop < len_; ++i) {
      buf_[i] = buf_[i + drop];
    }
    len_ -= drop;
  }

  uint8_t buf_[kMaxFrameLen];
  size_t len_;         // Buffered bytes (undelivered).
  size_t frameTotal_;  // Validated header's full frame length, 0 = header pending.
  size_t held_;        // Delivered frame bytes, discarded on next feed()/reset().
};

// CHUNK reassembly over a caller-provided buffer (INTERNAL heap on device,
// stack on host). Non-LAST chunks are assumed uniform size (stride = first
// non-LAST length); out-of-order and duplicates tolerated; a missing chunk
// when LAST arrives (or cap overflow) drops the whole frame.
//
// STALE contract: a new frameId arriving mid-frame abandons the partial —
// push() returns STALE and the caller must count a drop and RE-PUSH the same
// chunk to start the new frame.
class Reassembler {
 public:
  enum class Push : uint8_t { ACCEPTED, DUPLICATE, COMPLETE, DROPPED, STALE };

  static constexpr size_t kMaxChunks = 1024;
  static constexpr size_t kBitmapWords = kMaxChunks / 32;

  Reassembler() : buf_(nullptr), cap_(0) { reset(); }

  void attach(uint8_t* buf, size_t cap) {
    buf_ = buf;
    cap_ = cap;
    reset();
  }

  void reset() {
    active_ = false;
    frameId_ = 0;
    haveLast_ = false;
    lastIdx_ = 0;
    stride_ = 0;
    total_ = 0;
    seen_ = 0;
    pend_ = false;
    pendLen_ = 0;
    pendIdx_ = 0;
    for (size_t i = 0; i < kBitmapWords; ++i) {
      bits_[i] = 0;
    }
  }

  bool active() const { return active_; }

  Push push(uint16_t frameId, uint16_t chunkIdx, uint8_t flags, const uint8_t* data, uint16_t len, size_t& outTotal) {
    outTotal = 0;
    if (buf_ == nullptr || cap_ == 0 || chunkIdx >= kMaxChunks) {
      return Push::DROPPED;
    }
    if (len > kMaxPayload || (len > 0 && data == nullptr)) {
      return Push::DROPPED;
    }
    const bool last = (flags & FLAG_LAST_CHUNK) != 0;
    if (len == 0 && !last) {
      return Push::DROPPED;  // Zero-length non-LAST chunk carries no stride info.
    }
    if (active_ && frameId != frameId_) {
      const bool hadPartial = (seen_ > 0);
      reset();
      if (hadPartial) {
        return Push::STALE;  // Caller counts a drop, then re-pushes this chunk.
      }
    }
    if (!active_) {
      active_ = true;
      frameId_ = frameId;
    }
    if (isSet_(chunkIdx)) {
      return Push::DUPLICATE;
    }
    if (haveLast_ && chunkIdx > lastIdx_) {
      return Push::DROPPED;
    }
    if (last && stride_ == 0 && chunkIdx != 0) {
      // Stride unknown (LAST arrived before any full-size chunk): stash it.
      if (pend_) {
        return Push::DROPPED;
      }
      for (uint16_t i = 0; i < len; ++i) {
        pendBuf_[i] = data[i];
      }
      pendLen_ = len;
      pendIdx_ = chunkIdx;
      pend_ = true;
      haveLast_ = true;
      lastIdx_ = chunkIdx;
      return Push::ACCEPTED;
    }
    if (!last && stride_ == 0) {
      stride_ = len;
    }
    if (last && chunkIdx == 0 && stride_ == 0) {
      stride_ = (len > 0) ? len : 1;  // Single-chunk frame; len 0 keeps stride nonzero.
    }
    const size_t off = static_cast<size_t>(chunkIdx) * stride_;
    if (off + len > cap_) {
      reset();  // Over the one-frame-slot cap: drop the whole frame.
      return Push::DROPPED;
    }
    place_(chunkIdx, off, data, len);
    if (last) {
      haveLast_ = true;
      lastIdx_ = chunkIdx;
    }
    if (pend_ && stride_ != 0) {  // Stride now known: resolve the stashed LAST chunk.
      const size_t poff = static_cast<size_t>(pendIdx_) * stride_;
      if (poff + pendLen_ > cap_) {
        reset();
        return Push::DROPPED;
      }
      place_(pendIdx_, poff, pendBuf_, pendLen_);
      pend_ = false;
    }
    if (haveLast_ && !pend_ && seen_ == static_cast<size_t>(lastIdx_) + 1) {
      outTotal = total_;
      const Push done = Push::COMPLETE;
      reset();  // State only; assembled bytes stay in the attached buffer.
      return done;
    }
    return Push::ACCEPTED;
  }

 private:
  bool isSet_(uint16_t idx) const { return (bits_[idx / 32] & (1UL << (idx % 32))) != 0; }

  void place_(uint16_t idx, size_t off, const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
      buf_[off + i] = data[i];
    }
    bits_[idx / 32] |= (1UL << (idx % 32));
    ++seen_;
    if (off + len > total_) {
      total_ = off + len;
    }
  }

  uint8_t* buf_;
  size_t cap_;
  bool active_;
  uint16_t frameId_;
  bool haveLast_;
  uint16_t lastIdx_;
  size_t stride_;
  size_t total_;
  size_t seen_;
  uint32_t bits_[kBitmapWords];
  uint8_t pendBuf_[kMaxPayload];  // Stash for a LAST chunk that arrives before stride is known.
  uint16_t pendLen_;
  uint16_t pendIdx_;
  bool pend_;
};

}  // namespace uartpoc
