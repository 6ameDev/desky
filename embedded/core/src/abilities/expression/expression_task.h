#pragma once
// Expression task binding — abilities/expression (firmware session).
//
// Rare-event request entry plus the tick pump for auto-release around
// expression::Manager. An accepted request (or a play-once yield) stages the
// active id behind an internal latch for a later core->head link pump to
// consume; no wire type is claimed here and no socket is touched. Arduino-free
// (stdint only) so host tests pin the session directly.

#include <stdint.h>

#include "expression_manager.h"

namespace expression {

class Task {
 public:
  Task() : manager_(), staged_(exprcat::kBoot), hasStaged_(false) {}

  // Public request entry for behavior modules and the app-trigger path: true
  // when the request took effect (and staged for dispatch).
  bool request(uint8_t id, uint32_t nowMs) {
    const exprcat::Entry* e = exprcat::lookup(id);
    if (e == nullptr) {
      return false;
    }
    // Reserved media placeholders: dispatch carries the id only.
    (void)e->sound;
    (void)e->motion;
    if (!manager_.request(id, nowMs)) {
      return false;
    }
    staged_ = id;
    hasStaged_ = true;
    return true;
  }

  // Tick pump for play-once auto-release: true once per yield to neutral.
  bool tick(uint32_t nowMs) {
    if (!manager_.tick(nowMs)) {
      return false;
    }
    staged_ = manager_.active();
    hasStaged_ = true;
    return true;
  }

  uint8_t active() const { return manager_.active(); }
  bool hasStaged() const { return hasStaged_; }
  uint8_t staged() const { return staged_; }
  // Clears the dispatch latch after a successful link send; a failed send
  // keeps the latch so the pump retries the same id.
  void markDispatched() { hasStaged_ = false; }

  // Link pump: hands the staged id to the transport's dumb unicast exactly
  // once per stage; a refused send keeps the latch for retry. The caller
  // supplies the frozen wire type so this header imports no link namespace
  // (POSIX declares ::link); staged ids are catalog-valid by construction.
  template <typename Link>
  bool publishStaged(Link& link, uint8_t wireType) {
    if (!hasStaged_) {
      return false;
    }
    uint16_t seq = 0;
    if (!link.sendUnicast(wireType, &staged_, 1, seq)) {
      return false;
    }
    hasStaged_ = false;
    return true;
  }

 private:
  Manager manager_;
  uint8_t staged_;
  bool hasStaged_;
};

}  // namespace expression
