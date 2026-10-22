#pragma once
// Pairing wire codec — abilities/pairing (6-digit pairing).
//
// Tag-message builders, the persist-interval byte, PAIR_OK/ERR framing
// (0x51/0x52), and pairing-session policy constants. Phone-wire
// headers/sizes live once in transport/udp_codec.h (frozen table) and are
// aliased here, so every pairing:: spelling keeps working. The QR bitmap
// encoder lives in shared/link/qr_codec.h (included below). Frozen spec:
// shared/protocol/README.md. Arduino-free (stdint/stddef only) so host tests
// include it directly.

#include <stddef.h>
#include <stdint.h>

#include "link/qr_codec.h"
#include "transport/udp_codec.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
#ifndef CFG_PAIR_CODE_LEN
#define CFG_PAIR_CODE_LEN 6
#endif
#ifndef CFG_PAIR_WINDOW_MS
#define CFG_PAIR_WINDOW_MS 300000
#endif
#ifndef CFG_PAIR_MAX_TRIES
#define CFG_PAIR_MAX_TRIES 5
#endif
#ifndef CFG_PAIR_LOCKOUT_MS
#define CFG_PAIR_LOCKOUT_MS 30000
#endif
namespace pairing {

constexpr size_t kCodeLen = CFG_PAIR_CODE_LEN;
constexpr uint32_t kWindowMs = CFG_PAIR_WINDOW_MS;
constexpr uint32_t kMaxTries = CFG_PAIR_MAX_TRIES;
constexpr uint32_t kLockoutMs = CFG_PAIR_LOCKOUT_MS;

// PAIR_REQ (enrolment): App -> Robot, 12B [0x50 | code6 | clientNonce4 | XOR].
// Phone-wire home is udp:: (frozen table in transport/udp_codec.h).
constexpr uint8_t kPairReqHeader = udp::kPairReqHeader;
constexpr size_t kPairReqSize = udp::kPairReqSize;

// PAIR_OK persist-interval byte: the GCM plaintext grows psk16 ->
// psk16||persist_u8 (17B), so PAIR_OK grows 38B -> 39B. The byte rides inside
// the AES-GCM ct (never a bare field), is accepted once at pair only, and
// seeds the app's persist cadence. Guard is minimal: the wire holds 8..255;
// anything else maps to the default. A v1 38B frame (no byte) defaults to
// 128.
constexpr uint8_t kPairPersistDefault = 128;  // == COUNTER_PERSIST_EVERY both sides
constexpr uint16_t kPairPersistMin = 8;
constexpr uint16_t kPairPersistMax = 1024;  // App-domain max; wire holds the u8 subset (see below).
constexpr size_t kPairWrapPlainLen = 17;    // psk16 || persist_u8
constexpr size_t kPairCtLen = 17;
constexpr size_t kPairOkSize = udp::kPairOkSize;
constexpr size_t kPairOkLegacySize = 38;  // v1 robots without the interval byte

// Minimal guard for the negotiated persist cadence: 8..255 passes, anything
// else -> 128 (kills div-zero 0 + absurd values). Accepted once at pair only.
inline uint8_t sanitizePersistEvery(uint16_t v) {
  if (v < kPairPersistMin || v > kPairPersistMax || v > 255) {
    return kPairPersistDefault;
  }
  return static_cast<uint8_t>(v);
}

// UNBOND (symmetric forget): App -> Robot, 12B [0xAD | counterBE4 | tag6 | XOR],
//   tag = first6(HMAC-SHA256(psk, "UNBOND" || counterBE4)).
// Phone-wire home is udp:: (frozen table in transport/udp_codec.h).
constexpr uint8_t kUnbondHeader = udp::kUnbondHeader;
constexpr size_t kUnbondSize = udp::kUnbondSize;
constexpr size_t kUnbondTagLen = udp::kUnbondTagLen;
constexpr size_t kUnpairMsgLen = udp::kUnpairMsgLen;

// UNBOND tag message (10B, host-testable): "UNBOND" ASCII || counterBE4.
// Domain-separates the unbond tag from drive tags.
inline void unbondTagMsg(uint32_t counter, uint8_t out10[kUnpairMsgLen]) {
  if (out10 == nullptr) {
    return;
  }
  out10[0] = 'U';
  out10[1] = 'N';
  out10[2] = 'B';
  out10[3] = 'O';
  out10[4] = 'N';
  out10[5] = 'D';
  out10[6] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out10[7] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out10[8] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out10[9] = static_cast<uint8_t>(counter & 0xFFu);
}

// Sizes frozen by shared/protocol/README.md. Local constexprs (not
// ConfigStore) so this header stays includable without the Preferences stack.
constexpr size_t kPskLen = 16;
constexpr size_t kKekLen = 16;
constexpr size_t kPairNonceLen = 4;
constexpr size_t kPairIvLen = 12;
constexpr size_t kPairAadLen = 9;  // 0x51 || client_nonce(4) || server_nonce(4)
constexpr size_t kPairTagLen = 16;
constexpr uint8_t kPairOkHeader = 0x51;
// ── Tier-1 silent auto-refresh (era + PSK rotation, fully silent) ──────────
// Wire (see shared/protocol/README.md):
//   REKEY_REQ  App -> Robot, 14B [0x53 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(oldPsk, "REKEY" || eraBE2 || counterBE4))
//     era = requested new era (stored era + 1, u16 wrap mod 65536).
//   REKEY_ACK Robot -> App, 10B [0x54 | eraBE2 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(newPsk, "REKEYACK" || eraBE2)).
// Key schedule (no key transport):
//   PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16)
// both sides derive; only the era needs sync + confirm. Counter resets to
// the pair baseline (0 persisted, first drive uses 1) so the session resumes
// gapless. Old PSK is dropped immediately on switch; a duplicate REQ with
// era == current era re-ACKs idempotently without re-deriving.
constexpr uint8_t kRekeyReqHeader = udp::kRekeyReqHeader;
constexpr size_t kRekeyReqSize = udp::kRekeyReqSize;
constexpr uint8_t kRekeyAckHeader = udp::kRekeyAckHeader;
constexpr size_t kRekeyAckSize = udp::kRekeyAckSize;
constexpr size_t kRekeyTagLen = 6;
constexpr size_t kRekeyMsgLen = 11;     // "REKEY"(5) || eraBE2 || counterBE4
constexpr size_t kRekeyAckMsgLen = 10;  // "REKEYACK"(8) || eraBE2

// REKEY_REQ tag message (11B, host-testable): "REKEY" ASCII || eraBE2 ||
// counterBE4.
inline void refreshTagMsg(uint16_t era, uint32_t counter, uint8_t out11[kRekeyMsgLen]) {
  if (out11 == nullptr) {
    return;
  }
  out11[0] = 'R';
  out11[1] = 'E';
  out11[2] = 'K';
  out11[3] = 'E';
  out11[4] = 'Y';
  out11[5] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out11[6] = static_cast<uint8_t>(era & 0xFFu);
  out11[7] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out11[8] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out11[9] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out11[10] = static_cast<uint8_t>(counter & 0xFFu);
}

// REKEY_ACK tag message (10B, host-testable): "REKEYACK" ASCII || eraBE2.
// Authenticated under the NEW PSK so the ACK proves the switch.
inline void refreshAckTagMsg(uint16_t era, uint8_t out10[kRekeyAckMsgLen]) {
  if (out10 == nullptr) {
    return;
  }
  out10[0] = 'R';
  out10[1] = 'E';
  out10[2] = 'K';
  out10[3] = 'E';
  out10[4] = 'Y';
  out10[5] = 'A';
  out10[6] = 'C';
  out10[7] = 'K';
  out10[8] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out10[9] = static_cast<uint8_t>(era & 0xFFu);
}

// ── ALIVE + DISCONNECT (robot alive rule) ───────────────
// Wire (see shared/protocol/README.md):
//   ALIVE      App -> Robot, 14B [0x55 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "ALIVE" || eraBE2 || counterBE4))
//     era = CURRENT paired era.
//   DISCONNECT App -> Robot, 14B [0x56 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "DISCONNECT" || eraBE2 || counterBE4)).
// Both ride the shared strictly-increasing counter: a valid frame consumes
// lastCounter_ exactly like drive. Era must equal the stored era; old-key
// frames die via HMAC with no layout change.
// Headers 0x55/0x56 are claimed in transport/udp_codec.h (frozen table) —
// never reuse 0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/0xAD.
constexpr uint8_t kAliveHeader = udp::kAliveHeader;
constexpr size_t kAliveSize = udp::kAliveSize;
constexpr uint8_t kDisconnectHeader = udp::kDisconnectHeader;
constexpr size_t kDisconnectSize = udp::kDisconnectSize;
constexpr size_t kAliveTagLen = 6;
constexpr size_t kAliveMsgLen = 11;  // "ALIVE"(5) || eraBE2 || counterBE4
constexpr size_t kDisconnectTagLen = 6;
constexpr size_t kDisconnectMsgLen = 16;  // "DISCONNECT"(10) || eraBE2 || counterBE4

// ALIVE tag message (11B, host-testable): "ALIVE" ASCII || eraBE2 ||
// counterBE4.
inline void aliveTagMsg(uint16_t era, uint32_t counter, uint8_t out11[kAliveMsgLen]) {
  if (out11 == nullptr) {
    return;
  }
  out11[0] = 'A';
  out11[1] = 'L';
  out11[2] = 'I';
  out11[3] = 'V';
  out11[4] = 'E';
  out11[5] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out11[6] = static_cast<uint8_t>(era & 0xFFu);
  out11[7] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out11[8] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out11[9] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out11[10] = static_cast<uint8_t>(counter & 0xFFu);
}

// DISCONNECT tag message (16B, host-testable): "DISCONNECT" ASCII || eraBE2
// || counterBE4.
inline void disconnectTagMsg(uint16_t era, uint32_t counter, uint8_t out16[kDisconnectMsgLen]) {
  if (out16 == nullptr) {
    return;
  }
  out16[0] = 'D';
  out16[1] = 'I';
  out16[2] = 'S';
  out16[3] = 'C';
  out16[4] = 'O';
  out16[5] = 'N';
  out16[6] = 'N';
  out16[7] = 'E';
  out16[8] = 'C';
  out16[9] = 'T';
  out16[10] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out16[11] = static_cast<uint8_t>(era & 0xFFu);
  out16[12] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out16[13] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out16[14] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out16[15] = static_cast<uint8_t>(counter & 0xFFu);
}

// Wire reasons (PAIR_ERR reason byte). kOk is transport-internal (PAIR_OK);
// it never appears on the wire. Reasons 0-3 are frozen (never renumber);
// kInternal (4 = internal_retry_fresh) covers burn-failure after tryCode kOk
// (code already burned). Old peers show generic for unknown reasons.
enum class Result : uint8_t {
  kOk = 0xFF,
  kBadCode = 0,
  kLocked = 1,
  kNotInWindow = 2,
  kAlreadyPaired = 3,
  kInternal = 4,
};

inline uint8_t toWireReason(Result r) { return static_cast<uint8_t>(r); }

inline bool isDigitByte(uint8_t b) { return b >= '0' && b <= '9'; }

// Constant-time 6-byte compare: accumulates the full diff, no early exit.
// Prefer mbedtls_ct_memcmp where mbedTLS is linked; this loop is the portable
// semantic the host tests pin.
inline bool constantTimeEqual6(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kCodeLen; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace pairing
