#pragma once
// desky v2 pairing wire codec — abilities/pairing (merged 6-digit + QR).
//
// Single owner of the pairing wire vocabulary: PAIR_REQ (0x50), PAIR_OK/ERR
// (0x51/0x52), UNBOND (0xAD), REKEY (0x53/0x54), ALIVE/DISCONNECT (0x55/0x56),
// the persist-interval byte, tag-message builders, and the QR V1-L bitmap
// (bare 6 digits, numeric mode, mask 0 — provenance: Nayuki MIT port, see
// git history of qr_pairing.h). Frozen spec: shared/protocol/README.md.
// Arduino-free (stdint/stddef only) so host Unity tests include it directly.
// State machine lives in pairing_manager.h, firmware crypto glue + transport
// binding in pairing_task.h.

#include <stddef.h>
#include <stdint.h>

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as coordinator.h / udp_server.h).
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
// Single owner of the 0x50 vocabulary (transport mirrors via alias in
// udpstatus; equality pinned by test_udp_server).
constexpr uint8_t kPairReqHeader = 0x50;
constexpr size_t kPairReqSize = 12;

// PAIR_OK persist-interval byte (v1.1 addendum to the frozen v1 vectors):
// the GCM plaintext grows psk16 -> psk16||persist_u8 (17B), so PAIR_OK
// grows 38B -> 39B [0x51|srvNonce4|ct17|tag16|XOR]. The byte rides inside
// the AES-GCM ct (auth by tag+AAD like the PSK itself — never a bare
// field), is accepted once at pair only, and seeds the app's DriveCounter
// persist cadence. Guard is minimal (not an allowlist): the wire holds the
// u8 subset (8..255) of the 8..1024 app-domain guard — 0..7 (incl.
// div-zero 0) and anything above 255 (incl. the app-reserved 256..1024,
// a future u16 field) map to the default. An old (v1) robot sends 38B with
// no byte — the app defaults to 128 in that case.
constexpr uint8_t kPairPersistDefault = 128;  // == COUNTER_PERSIST_EVERY both sides
constexpr uint16_t kPairPersistMin = 8;
constexpr uint16_t kPairPersistMax = 1024;  // App-domain max; wire holds the u8 subset (see below).
constexpr size_t kPairWrapPlainLen = 17;    // psk16 || persist_u8
constexpr size_t kPairCtLen = 17;
constexpr size_t kPairOkSize = 39;
constexpr size_t kPairOkLegacySize = 38;  // v1 robots without the interval byte

// Minimal guard for the negotiated persist cadence: N in 8..255 passes
// (the wire-representable subset of the 8..1024 app-domain guard),
// anything else -> 128. Kills 0 (div-zero in the % cadence) + absurd
// values; accepted once at pair only (see the wrapPsk call site).
inline uint8_t sanitizePersistEvery(uint16_t v) {
  if (v < kPairPersistMin || v > kPairPersistMax || v > 255) {
    return kPairPersistDefault;
  }
  return static_cast<uint8_t>(v);
}

// UNBOND (symmetric forget): App -> Robot, 12B [0xAD | counterBE4 | tag6 | XOR],
//   tag = first6(HMAC-SHA256(psk, "UNBOND" || counterBE4)).
// Single owner of the 0xAD vocabulary (transport mirrors via alias in
// udpstatus; moved verbatim from udpstatus::unbondTagMsg).
constexpr uint8_t kUnbondHeader = 0xAD;
constexpr size_t kUnbondSize = 12;
constexpr size_t kUnbondTagLen = 6;
constexpr size_t kUnbondMsgLen = 10;  // "UNBOND" (6 ASCII) || counterBE4

// UNBOND tag message (10B, host-testable): "UNBOND" ASCII || counterBE4.
// Domain-separates the unbond tag from drive tags (drive covers
// counter||mode||thr||str||flags with no ASCII prefix).
inline void unbondTagMsg(uint32_t counter, uint8_t out10[kUnbondMsgLen]) {
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

// Sizes frozen by shared/protocol/README.md §"Pairing + authenticated drive
// v1". Local constexprs (not ConfigStore) so this header stays includable
// without the Arduino Preferences stack.
constexpr size_t kPskLen = 16;
constexpr size_t kKekLen = 16;
constexpr size_t kPairNonceLen = 4;
constexpr size_t kPairIvLen = 12;
constexpr size_t kPairAadLen = 9;  // 0x51 || client_nonce(4) || server_nonce(4)
constexpr size_t kPairTagLen = 16;
constexpr uint8_t kPairOkHeader = 0x51;
// ── Tier-1 silent auto-rekey (era + PSK rotation, fully silent) ──────────
// Wire (see shared/protocol/README.md §"Tier-1 silent auto-rekey"):
//   REKEY_REQ  App -> Robot, 14B [0x53 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(oldPsk, "REKEY" || eraBE2 || counterBE4))
//     era = requested new era (stored era + 1, u16 wrap mod 65536).
//   REKEY_ACK Robot -> App, 10B [0x54 | eraBE2 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(newPsk, "REKEYACK" || eraBE2)).
// Key schedule (no key transport — mirrors TLS KeyUpdate / IPsec rekey):
//   PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16)
// both sides derive; only the era needs sync + confirm. Counter resets to
// the bond baseline (0 persisted, first drive uses 1 — same as fresh pair)
// so the session resumes gapless before EXHAUSTION_GATE. Old PSK is dropped
// immediately on switch (no reorder window — documented); a duplicate REQ
// with era == current era re-ACKs idempotently without re-deriving.
constexpr uint8_t kRekeyReqHeader = 0x53;
constexpr size_t kRekeyReqSize = 14;
constexpr uint8_t kRekeyAckHeader = 0x54;
constexpr size_t kRekeyAckSize = 10;
constexpr size_t kRekeyTagLen = 6;
constexpr size_t kRekeyMsgLen = 11;     // "REKEY"(5) || eraBE2 || counterBE4
constexpr size_t kRekeyAckMsgLen = 10;  // "REKEYACK"(8) || eraBE2

// REKEY_REQ tag message (11B, host-testable): "REKEY" ASCII || eraBE2 ||
// counterBE4. Domain-separates the rekey tag from drive tags (no prefix)
// and unbond tags ("UNBOND" prefix).
inline void rekeyTagMsg(uint16_t era, uint32_t counter, uint8_t out11[kRekeyMsgLen]) {
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
inline void rekeyAckTagMsg(uint16_t era, uint8_t out10[kRekeyAckMsgLen]) {
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

// ── ALIVE + DISCONNECT (robot alive rule, Phase-2 keepalive) ───────────────
// Wire (see shared/protocol/README.md §"ALIVE + DISCONNECT + robot alive
// rule"):
//   ALIVE      App -> Robot, 14B [0x55 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "ALIVE" || eraBE2 || counterBE4))
//     era = CURRENT bonded era (stored era; a rekey moves it, see below).
//   DISCONNECT App -> Robot, 14B [0x56 | eraBE2 | counterBE4 | tag6 | XOR]
//     tag = first6(HMAC-SHA256(psk, "DISCONNECT" || eraBE2 || counterBE4)).
// Both ride the shared strictly-increasing counter (same holder as DRIVE_V2
// + REKEY_REQ, allocated app-side): freshness is isFreshCounter, and a valid
// frame consumes lastCounter_ exactly like drive. Era must equal the stored
// era (current session); old-key frames die via HMAC with no layout change.
// Headers 0x55/0x56 are claimed here — never reuse 0xAA/0xAC/0xBB/0x50/
// 0x51/0x52/0x53/0x54/0xAD.
constexpr uint8_t kAliveHeader = 0x55;
constexpr size_t kAliveSize = 14;
constexpr uint8_t kDisconnectHeader = 0x56;
constexpr size_t kDisconnectSize = 14;
constexpr size_t kAliveTagLen = 6;
constexpr size_t kAliveMsgLen = 11;  // "ALIVE"(5) || eraBE2 || counterBE4
constexpr size_t kDisconnectTagLen = 6;
constexpr size_t kDisconnectMsgLen = 16;  // "DISCONNECT"(10) || eraBE2 || counterBE4

// ALIVE tag message (11B, host-testable): "ALIVE" ASCII || eraBE2 ||
// counterBE4. Domain-separates the alive tag from drive tags (no prefix),
// unbond tags ("UNBOND" prefix), and rekey tags ("REKEY" prefix).
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
// || counterBE4. Same domain-separation rule as ALIVE above.
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

// Wire reasons in shared/protocol/README.md §v1 (PAIR_ERR reason byte).
// kOk is transport-internal (PAIR_OK); it never appears on the wire.
// M11 additive reason 4 (kInternal = internal_retry_fresh): reasons 0-3 are
// frozen (never renumber; 0=bad_code keeps explicit 0 semantics, 2 keeps
// not_in_window for genuine window expiry). Burn-failure after tryCode kOk
// (saveBond/deriveKek/wrapPsk fail, code already burned) sends 4. Old peers
// show generic for unknown reasons.
enum class Result : uint8_t {
  kOk = 0xFF,
  kBadCode = 0,
  kLocked = 1,
  kNotInWindow = 2,
  kAlreadyBonded = 3,
  kInternal = 4,
};

inline uint8_t toWireReason(Result r) { return static_cast<uint8_t>(r); }

inline bool isDigitByte(uint8_t b) { return b >= '0' && b <= '9'; }

// Constant-time 6-byte compare: accumulates the full diff, no early exit.
// (Call-site rule in Task 4: prefer mbedtls_ct_memcmp where mbedTLS is
// linked; this loop is the portable semantic the host tests pin.)
inline bool constantTimeEqual6(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kCodeLen; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace pairing

namespace qr {

// QR Version 1 edge length in modules (no quiet zone included).
constexpr int kSize = 21;
// Packed row width: ceil(21/8) bytes, MSB-first (module x=0 -> bit 7).
constexpr int kRowBytes = 3;
// Flat bitmap length: 21*21 modules, one byte per module (0 = light, 1 = dark).
constexpr int kModules = kSize * kSize;
// Fixed code parameters for V1-L (single block, no interleave).
constexpr int kDataCodewords = 19;
constexpr int kEcCodewords = 7;
constexpr int kTotalCodewords = kDataCodewords + kEcCodewords;

namespace detail {

// GF(2^8/0x11D) multiply (Nayuki Russian-peasant form, no lookup tables).
inline uint8_t rsMul(uint8_t x, uint8_t y) {
  uint16_t z = 0;
  for (int i = 7; i >= 0; --i) {
    z = static_cast<uint16_t>((z << 1) ^ ((z >> 7) * 0x11Du));
    z ^= static_cast<uint16_t>(((y >> i) & 1u) * x);
  }
  return static_cast<uint8_t>(z);
}

inline bool getBit(uint16_t x, int i) { return ((x >> i) & 1u) != 0; }

inline void setModule(uint8_t* modules, bool* isFunc, int x, int y, bool dark) {
  modules[y * kSize + x] = dark ? 1u : 0u;
  isFunc[y * kSize + x] = true;
}

// 9x9 finder block (7x7 pattern + white separator ring) centred at (cx, cy).
// Out-of-bounds cells are skipped, so the three corner calls clip cleanly.
inline void drawFinder(uint8_t* modules, bool* isFunc, int cx, int cy) {
  for (int dy = -4; dy <= 4; ++dy) {
    for (int dx = -4; dx <= 4; ++dx) {
      const int xx = cx + dx;
      const int yy = cy + dy;
      if (xx < 0 || xx >= kSize || yy < 0 || yy >= kSize) {
        continue;
      }
      int dist = dx < 0 ? -dx : dx;
      const int ady = dy < 0 ? -dy : dy;
      if (ady > dist) {
        dist = ady;
      }
      setModule(modules, isFunc, xx, yy, dist != 2 && dist != 4);
    }
  }
}

}  // namespace detail

// Encodes 6 ASCII digits ('0'..'9') into a 21x21 bitmap (0 = light,
// 1 = dark, row-major `modules[y * kSize + x]`). Returns false on
// null input or any non-digit byte (mirror pairing::openWindow: embedded
// NULs reject, so short NUL-terminated strings fail closed). True on
// success; the output is fully deterministic for a given input.
inline bool encode6Digits(const uint8_t code6[6], uint8_t modules[kModules]) {
  if (code6 == nullptr || modules == nullptr) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    if (code6[i] < '0' || code6[i] > '9') {
      return false;
    }
  }

  // ── Data codewords (numeric mode) ───────────────────────────────────
  uint8_t data[kDataCodewords];
  for (int i = 0; i < kDataCodewords; ++i) {
    data[i] = 0;
  }
  int bitLen = 0;
  auto appendBits = [&](uint16_t val, int n) {
    for (int i = n - 1; i >= 0; --i) {
      if (detail::getBit(val, i)) {
        data[bitLen >> 3] |= static_cast<uint8_t>(1u << (7 - (bitLen & 7)));
      }
      ++bitLen;
    }
  };
  appendBits(0x1, 4);  // Numeric mode indicator 0001.
  appendBits(6, 10);   // Character count (V1-9 numeric width is 10 bits).
  for (int g = 0; g < 2; ++g) {
    const int d0 = code6[g * 3] - '0';
    const int d1 = code6[g * 3 + 1] - '0';
    const int d2 = code6[g * 3 + 2] - '0';
    appendBits(static_cast<uint16_t>(d0 * 100 + d1 * 10 + d2), 10);
  }
  const int capacityBits = kDataCodewords * 8;
  int term = capacityBits - bitLen;
  if (term > 4) {
    term = 4;
  }
  appendBits(0, term);           // Terminator (up to 4 zero bits).
  appendBits(0, -bitLen & 7);    // Pad to the next byte boundary.
  const int used = bitLen >> 3;  // 5 bytes at this point.
  for (int i = used; i < kDataCodewords; ++i) {
    data[i] = ((i - used) % 2 == 0) ? 0xEC : 0x11;  // Standard pad bytes.
  }

  // ── Reed-Solomon EC (degree 7, single block) ────────────────────────
  uint8_t divisor[kEcCodewords];  // Generator poly sans leading term.
  for (int i = 0; i < kEcCodewords; ++i) {
    divisor[i] = 0;
  }
  divisor[kEcCodewords - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < kEcCodewords; ++i) {
    for (int j = 0; j < kEcCodewords; ++j) {
      divisor[j] = detail::rsMul(divisor[j], root);
      if (j + 1 < kEcCodewords) {
        divisor[j] ^= divisor[j + 1];
      }
    }
    root = detail::rsMul(root, 0x02);
  }
  uint8_t ec[kEcCodewords];
  for (int i = 0; i < kEcCodewords; ++i) {
    ec[i] = 0;
  }
  for (int i = 0; i < kDataCodewords; ++i) {
    const uint8_t factor = static_cast<uint8_t>(data[i] ^ ec[0]);
    for (int j = 0; j < kEcCodewords - 1; ++j) {
      ec[j] = ec[j + 1];
    }
    ec[kEcCodewords - 1] = 0;
    for (int j = 0; j < kEcCodewords; ++j) {
      ec[j] ^= detail::rsMul(divisor[j], factor);
    }
  }
  uint8_t all[kTotalCodewords];
  for (int i = 0; i < kDataCodewords; ++i) {
    all[i] = data[i];
  }
  for (int i = 0; i < kEcCodewords; ++i) {
    all[kDataCodewords + i] = ec[i];
  }

  // ── Matrix: function patterns ───────────────────────────────────────
  bool isFunc[kModules];
  for (int i = 0; i < kModules; ++i) {
    modules[i] = 0;
    isFunc[i] = false;
  }
  for (int i = 0; i < kSize; ++i) {  // Timing row 6 / column 6 …
    detail::setModule(modules, isFunc, i, 6, (i % 2) == 0);
    detail::setModule(modules, isFunc, 6, i, (i % 2) == 0);
  }
  // …overwritten by the finders where they overlap (Nayuki order).
  detail::drawFinder(modules, isFunc, 3, 3);
  detail::drawFinder(modules, isFunc, kSize - 4, 3);
  detail::drawFinder(modules, isFunc, 3, kSize - 4);

  // Format bits for ECC-L (01) + mask 0: data=01000, BCH(15,5) remainder,
  // XOR mask 101010000010010 (Nayuki _draw_format_bits verbatim, mask=0).
  const uint16_t fmtData = 0x08;  // 01 (L) || 000 (mask 0).
  uint16_t rem = fmtData;
  for (int i = 0; i < 10; ++i) {
    rem = static_cast<uint16_t>((rem << 1) ^ (((rem >> 9) & 1u) * 0x537u));
  }
  const uint16_t fmtBits = static_cast<uint16_t>((fmtData << 10 | rem) ^ 0x5412u);
  for (int i = 0; i <= 5; ++i) {
    detail::setModule(modules, isFunc, 8, i, detail::getBit(fmtBits, i));
  }
  detail::setModule(modules, isFunc, 8, 7, detail::getBit(fmtBits, 6));
  detail::setModule(modules, isFunc, 8, 8, detail::getBit(fmtBits, 7));
  detail::setModule(modules, isFunc, 7, 8, detail::getBit(fmtBits, 8));
  for (int i = 9; i <= 14; ++i) {
    detail::setModule(modules, isFunc, 14 - i, 8, detail::getBit(fmtBits, i));
  }
  for (int i = 0; i <= 7; ++i) {
    detail::setModule(modules, isFunc, kSize - 1 - i, 8, detail::getBit(fmtBits, i));
  }
  for (int i = 8; i <= 14; ++i) {
    detail::setModule(modules, isFunc, 8, kSize - 15 + i, detail::getBit(fmtBits, i));
  }
  detail::setModule(modules, isFunc, 8, kSize - 8, true);  // Dark module.

  // ── Codeword zigzag (Nayuki _draw_codewords, V1 has no remainder bits) ─
  int bit = 0;
  const int totalBits = kTotalCodewords * 8;
  for (int right = kSize - 1; right >= 1; right -= 2) {
    int r = right;
    if (r <= 6) {
      r -= 1;  // Skip the vertical timing column.
    }
    for (int vert = 0; vert < kSize; ++vert) {
      for (int j = 0; j < 2; ++j) {
        const int x = r - j;
        const bool upward = ((r + 1) & 2) == 0;
        const int y = upward ? (kSize - 1 - vert) : vert;
        if (!isFunc[y * kSize + x] && bit < totalBits) {
          modules[y * kSize + x] = detail::getBit(all[bit >> 3], 7 - (bit & 7)) ? 1u : 0u;
          ++bit;
        }
      }
    }
  }

  // ── Fixed mask 0: (x + y) % 2 == 0 flips data modules only ──────────
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (!isFunc[y * kSize + x] && ((x + y) % 2 == 0)) {
        modules[y * kSize + x] ^= 1u;
      }
    }
  }
  return true;
}

// Reads one module (0 = light, 1 = dark). Returns 0 for null bitmap or
// out-of-range coordinates (fail-closed, never crashes the call site).
inline uint8_t getModule(const uint8_t modules[kModules], int x, int y) {
  if (modules == nullptr || x < 0 || x >= kSize || y < 0 || y >= kSize) {
    return 0;
  }
  return static_cast<uint8_t>(modules[y * kSize + x] & 1u);
}

// Packs row y into 3 bytes MSB-first (module x -> bit (7 - x % 8) of
// byte x / 8; unused low bits of byte 2 are zero). No-op on null/out.
// Serial-dump shape for the future Tier-A hook: one `QR row=NN:HHHHHH`
// line per row, 6 uppercase hex chars from these 3 bytes.
inline void rowBytes(const uint8_t modules[kModules], int y, uint8_t out[kRowBytes]) {
  if (modules == nullptr || out == nullptr || y < 0 || y >= kSize) {
    return;
  }
  for (int i = 0; i < kRowBytes; ++i) {
    out[i] = 0;
  }
  for (int x = 0; x < kSize; ++x) {
    if (modules[y * kSize + x] & 1u) {
      out[x >> 3] |= static_cast<uint8_t>(1u << (7 - (x & 7)));
    }
  }
}

// Wire payload length: 21 rows x 3 packed bytes = 63B (the QRBITMAP
// contract — link_codec.h kQrBitmapLen; kept in sync by
// test_qr_packed_matches_golden_rows, never redefined here).
constexpr int kPackedLen = kSize * kRowBytes;

// Packs the whole 21x21 bitmap into the 63B wire payload (row-major
// MSB-first: payload[y * kRowBytes + i] is row y's ith packed byte).
// No-op on null input/output. Null-safe, no heap — the caller owns `out`.
inline void packBitmap(const uint8_t modules[kModules], uint8_t out[kPackedLen]) {
  if (modules == nullptr || out == nullptr) {
    return;
  }
  for (int y = 0; y < kSize; ++y) {
    rowBytes(modules, y, out + y * kRowBytes);
  }
}

}  // namespace qr
