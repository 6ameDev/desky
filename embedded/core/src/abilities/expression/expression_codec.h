#pragma once
// Expression codec — abilities/expression (core-side face ownership).
//
// Thin alias over the shared expression catalog (link/expression_catalog.h):
// core-side id constants plus the neutral-family predicate. Arduino-free
// (stdint only) so host tests include it directly.

#include <stdint.h>

#include "behavior/mode_manager.h"
#include "link/expression_catalog.h"
#include "transport/udp_codec.h"

namespace expression {

constexpr uint8_t kBoot = exprcat::kBoot;
constexpr uint8_t kHappy = exprcat::kHappy;
constexpr uint8_t kSad = exprcat::kSad;
constexpr uint8_t kBlink = exprcat::kBlink;
constexpr uint8_t kAlert = exprcat::kAlert;
constexpr uint8_t kCurious = exprcat::kCurious;
constexpr uint8_t kSurprised = exprcat::kSurprised;
constexpr uint8_t kCount = exprcat::kCount;

// Neutral family: requests for these always release the active id, from any
// state (mirrors the head gate, where BOOT/BLINK bypass the priority check).
inline bool isNeutral(uint8_t id) { return id == exprcat::kBoot || id == exprcat::kBlink; }

// App-trigger gate: a verified EXPR_TRIGGER reaches the expression owner only
// while sysmode MANUAL is held (the app holds the drive session); every other
// mode drops it with no reply.
inline bool triggerAllowed(sysmode::Mode mode) { return mode == sysmode::Mode::kManual; }

// EXPR_LIST_RESP layout (see shared/protocol/README.md):
//   Robot -> App, variable [0x5A | ver | count | (id, flags, name[10]) x count
//   | XOR], ver 1, count 0..12, names blank-padded to exactly 10B (never
//   NUL-terminated). Flags bit 0 = play-once (vs hold); sound/motion stay off
//   the wire. The response header lives once in transport/udp_codec.h
//   (frozen table) and is aliased here.
constexpr uint8_t kExprListRespHeader = udp::kExprListRespHeader;
constexpr uint8_t kExprListRespVer = 1;
constexpr size_t kExprListRespMaxCount = 12;
constexpr size_t kExprNameLen = 10;
constexpr size_t kExprEntryLen = 12;  // id + flags + name[10]

// EXPR_LIST_RESP composer (host-testable): serves ver/count/entries straight
// from the shared catalog (exprcat::kTable, never a second copy). Returns the
// total frame length, or 0 when out is null, the catalog outgrows the wire
// cap, or the buffer is too small.
inline size_t composeExprListResp(uint8_t* out, size_t outLen) {
  const size_t count = exprcat::count();
  if (out == nullptr || count > kExprListRespMaxCount) {
    return 0;
  }
  const size_t need = 4 + kExprEntryLen * count;
  if (outLen < need) {
    return 0;
  }
  out[0] = kExprListRespHeader;
  out[1] = kExprListRespVer;
  out[2] = static_cast<uint8_t>(count);
  size_t o = 3;
  for (size_t i = 0; i < count; ++i) {
    const exprcat::Entry& e = exprcat::kTable[i];
    out[o++] = e.id;
    out[o++] = e.playOnce ? 0x01 : 0x00;
    size_t n = 0;
    while (n < kExprNameLen && e.name[n] != '\0') {
      out[o++] = static_cast<uint8_t>(e.name[n]);
      ++n;
    }
    while (n < kExprNameLen) {
      out[o++] = ' ';
      ++n;
    }
  }
  uint8_t xorSum = 0;
  for (size_t i = 0; i + 1 < need; ++i) {
    xorSum ^= out[i];
  }
  out[need - 1] = xorSum;
  return need;
}

}  // namespace expression
