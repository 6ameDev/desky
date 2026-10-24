#pragma once
// Expression manager — abilities/expression.
//
// Pure one-active preemption policy over the shared catalog: higher priority
// preempts, equal refreshes, lower never displaces; the neutral family always
// releases; play-once ids yield to neutral after their hold. Zero sockets,
// zero rendering. Arduino-free (stdint only) so host tests include it
// directly.

#include <stdint.h>

#include "expression_codec.h"

namespace expression {

class Manager {
 public:
  Manager() : active_(exprcat::kBoot), sinceMs_(0) {}

  // Rare-event entry (not per-tick): true when the request took effect.
  // Unknown ids fail closed with zero state change.
  bool request(uint8_t id, uint32_t nowMs) {
    const exprcat::Entry* e = exprcat::lookup(id);
    if (e == nullptr) {
      return false;
    }
    if (!exprcat::isNeutral(id) && e->priority < exprcat::priorityFor(active_)) {
      return false;
    }
    active_ = id;
    sinceMs_ = nowMs;
    return true;
  }

  // Auto-release pump: true once when a play-once id yielded to neutral.
  // Unsigned subtraction: wrap-safe across millis() rollover.
  bool tick(uint32_t nowMs) {
    if (!exprcat::isPlayOnce(active_) || (nowMs - sinceMs_) < exprcat::holdMsFor(active_)) {
      return false;
    }
    active_ = exprcat::kBoot;
    sinceMs_ = nowMs;
    return true;
  }

  uint8_t active() const { return active_; }
  uint32_t activeSinceMs() const { return sinceMs_; }

 private:
  uint8_t active_;
  uint32_t sinceMs_;
};

}  // namespace expression
