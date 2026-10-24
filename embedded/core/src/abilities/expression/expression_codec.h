#pragma once
// Expression codec — abilities/expression (core-side face ownership).
//
// Thin alias over the shared expression catalog (link/expression_catalog.h):
// core-side id constants plus the neutral-family predicate. Arduino-free
// (stdint only) so host tests include it directly.

#include <stdint.h>

#include "behavior/mode_manager.h"
#include "link/expression_catalog.h"

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

}  // namespace expression
