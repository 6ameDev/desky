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

// UNPAIR (symmetric forget): App -> Robot, 12B [0xAD | counterBE4 | tag6 | XOR],
//   tag = first6(HMAC-SHA256(psk, "UNPAIR" || counterBE4)).
// Phone-wire home is udp:: (frozen table in transport/udp_codec.h).
constexpr uint8_t kUnpairHeader = udp::kUnpairHeader;
constexpr size_t kUnpairSize = udp::kUnpairSize;
constexpr size_t kUnpairTagLen = udp::kUnpairTagLen;
constexpr size_t kUnpairMsgLen = udp::kUnpairMsgLen;

// UNPAIR tag message (10B, host-testable): "UNPAIR" ASCII || counterBE4.
// Domain-separates the unpair tag from drive tags.
inline void unpairTagMsg(uint32_t counter, uint8_t out10[kUnpairMsgLen]) {
  if (out10 == nullptr) {
    return;
  }
  out10[0] = 'U';
  out10[1] = 'N';
  out10[2] = 'P';
  out10[3] = 'A';
  out10[4] = 'I';
  out10[5] = 'R';
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
// ── Tier-1 silent auto-pair-refresh (era + PSK rotation, fully silent) ───
// Wire (see shared/protocol/README.md):
//   PAIR_REFRESH_REQ  App -> Robot, 14B [0x53 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(oldPsk, "PAIRREFRESH" || eraBE2 || counterBE4))
//     era = requested new era (stored era + 1, u16 wrap mod 65536).
//   PAIR_REFRESH_ACK Robot -> App, 10B [0x54 | eraBE2 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(newPsk, "PAIRREFRESHACK" || eraBE2)).
// Key schedule (no key transport):
//   PSK_next = HKDF-SHA256(salt="desky-pair-refresh-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16)
// both sides derive; only the era needs sync + confirm. Counter resets to
// the pair baseline (0 persisted, first drive uses 1) so the session resumes
// gapless. Old PSK is dropped immediately on switch; a duplicate REQ with
// era == current era re-ACKs idempotently without re-deriving.
constexpr uint8_t kPairRefreshReqHeader = udp::kPairRefreshReqHeader;
constexpr size_t kPairRefreshReqSize = udp::kPairRefreshReqSize;
constexpr uint8_t kPairRefreshAckHeader = udp::kPairRefreshAckHeader;
constexpr size_t kPairRefreshAckSize = udp::kPairRefreshAckSize;
constexpr size_t kPairRefreshTagLen = 6;
constexpr size_t kPairRefreshMsgLen = 17;     // "PAIRREFRESH"(11) || eraBE2 || counterBE4
constexpr size_t kPairRefreshAckMsgLen = 16;  // "PAIRREFRESHACK"(14) || eraBE2

// PAIR_REFRESH_REQ tag message (17B, host-testable): "PAIRREFRESH" ASCII ||
// eraBE2 || counterBE4.
inline void pairRefreshTagMsg(uint16_t era, uint32_t counter, uint8_t out17[kPairRefreshMsgLen]) {
  if (out17 == nullptr) {
    return;
  }
  out17[0] = 'P';
  out17[1] = 'A';
  out17[2] = 'I';
  out17[3] = 'R';
  out17[4] = 'R';
  out17[5] = 'E';
  out17[6] = 'F';
  out17[7] = 'R';
  out17[8] = 'E';
  out17[9] = 'S';
  out17[10] = 'H';
  out17[11] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out17[12] = static_cast<uint8_t>(era & 0xFFu);
  out17[13] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out17[14] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out17[15] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out17[16] = static_cast<uint8_t>(counter & 0xFFu);
}

// PAIR_REFRESH_ACK tag message (16B, host-testable): "PAIRREFRESHACK" ASCII
// || eraBE2. Authenticated under the NEW PSK so the ACK proves the switch.
inline void pairRefreshAckTagMsg(uint16_t era, uint8_t out16[kPairRefreshAckMsgLen]) {
  if (out16 == nullptr) {
    return;
  }
  out16[0] = 'P';
  out16[1] = 'A';
  out16[2] = 'I';
  out16[3] = 'R';
  out16[4] = 'R';
  out16[5] = 'E';
  out16[6] = 'F';
  out16[7] = 'R';
  out16[8] = 'E';
  out16[9] = 'S';
  out16[10] = 'H';
  out16[11] = 'A';
  out16[12] = 'C';
  out16[13] = 'K';
  out16[14] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out16[15] = static_cast<uint8_t>(era & 0xFFu);
}

// ── ALIVE + DISCONNECT (robot alive rule) ───────────────
// Wire (see shared/protocol/README.md):
//   ALIVE      App -> Robot, 14B [0x55 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "ALIVE" || eraBE2 || counterBE4))
//     era = CURRENT paired era. Counter is the app's toRobotCounter; the
//     robot verifies against its fromAppCounter.
//   DISCONNECT Both, 14B [0x56 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "DISCONNECT" || eraBE2 || counterBE4)).
// App->Robot is the goodbye that keeps the pair (app's toRobotCounter);
// Robot->App is the pair-button eviction that clears it (robot's
// toAppCounter: starts at 1 per pair, strictly increasing, session-scoped
// with PSK+era; the app verifies against its fromRobotCounter). Same
// layout, fire-and-forget. Rule: every authenticated link holds
// from{Peer}/to{Peer} counter pairs scoped to that peer's session.
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

// Robot->app send-domain step: strictly increasing, 0 never goes on the
// wire (a wrapped step uses 1). Pure so host tests pin it.
inline uint32_t nextToAppCounter(uint32_t cur) {
  const uint32_t next = cur + 1;
  return (next == 0) ? 1 : next;
}

// Pair-button eviction burst: first frame at press, then this many resends
// of the byte-identical frame spaced by the gap below (non-blocking pump).
constexpr uint8_t kEvictBurstSends = 3;
constexpr uint8_t kEvictBurstResends = kEvictBurstSends - 1;
constexpr uint32_t kEvictBurstGapMs = 50;

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

// ── MANUAL session (explicit take/release, stale auto-exit) ─────────────
// Wire (see shared/protocol/README.md):
//   MANUAL_TAKE    App -> Robot, 14B [0x57 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "MANUALTAKE" || eraBE2 || counterBE4))
//     era = CURRENT paired era. Counter is the app's toRobotCounter; the
//     robot verifies against its fromAppCounter and consumes it on accept
//     exactly like drive/alive.
//   MANUAL_RELEASE App -> Robot, 14B [0x58 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "MANUALRELEASE" || eraBE2 || counterBE4)).
// Headers 0x57/0x58 are claimed in transport/udp_codec.h (frozen table) —
// never reuse 0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/0x55/0x56/0xAD.
constexpr uint8_t kManualTakeHeader = udp::kManualTakeHeader;
constexpr size_t kManualTakeSize = udp::kManualTakeSize;
constexpr uint8_t kManualReleaseHeader = udp::kManualReleaseHeader;
constexpr size_t kManualReleaseSize = udp::kManualReleaseSize;
constexpr size_t kManualTagLen = 6;
constexpr size_t kManualTakeMsgLen = 16;     // "MANUALTAKE"(10) || eraBE2 || counterBE4
constexpr size_t kManualReleaseMsgLen = 19;  // "MANUALRELEASE"(13) || eraBE2 || counterBE4

// MANUAL_TAKE tag message (16B, host-testable): "MANUALTAKE" ASCII ||
// eraBE2 || counterBE4.
inline void manualTakeTagMsg(uint16_t era, uint32_t counter, uint8_t out16[kManualTakeMsgLen]) {
  if (out16 == nullptr) {
    return;
  }
  out16[0] = 'M';
  out16[1] = 'A';
  out16[2] = 'N';
  out16[3] = 'U';
  out16[4] = 'A';
  out16[5] = 'L';
  out16[6] = 'T';
  out16[7] = 'A';
  out16[8] = 'K';
  out16[9] = 'E';
  out16[10] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out16[11] = static_cast<uint8_t>(era & 0xFFu);
  out16[12] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out16[13] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out16[14] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out16[15] = static_cast<uint8_t>(counter & 0xFFu);
}

// MANUAL_RELEASE tag message (19B, host-testable): "MANUALRELEASE" ASCII ||
// eraBE2 || counterBE4.
inline void manualReleaseTagMsg(uint16_t era, uint32_t counter, uint8_t out19[kManualReleaseMsgLen]) {
  if (out19 == nullptr) {
    return;
  }
  out19[0] = 'M';
  out19[1] = 'A';
  out19[2] = 'N';
  out19[3] = 'U';
  out19[4] = 'A';
  out19[5] = 'L';
  out19[6] = 'R';
  out19[7] = 'E';
  out19[8] = 'L';
  out19[9] = 'E';
  out19[10] = 'A';
  out19[11] = 'S';
  out19[12] = 'E';
  out19[13] = static_cast<uint8_t>((era >> 8) & 0xFFu);
  out19[14] = static_cast<uint8_t>(era & 0xFFu);
  out19[15] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
  out19[16] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
  out19[17] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
  out19[18] = static_cast<uint8_t>(counter & 0xFFu);
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
