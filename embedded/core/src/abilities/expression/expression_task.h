#pragma once
// Expression task binding — abilities/expression (firmware session).
//
// Rare-event request entry plus the tick pump for auto-release around
// expression::Manager. An accepted request (or a play-once yield) stages the
// active id behind an internal latch for a later core->head link pump to
// consume; no wire type is claimed here and no socket is touched. Arduino-free
// (stdint only) so host tests pin the session directly.

#include <stdint.h>

#include "abilities/audio/audio_codec.h"
#include "expression_manager.h"
#include "link/motion_primitives.h"

namespace expression {

class Task {
 public:
  Task() : manager_(), staged_(exprcat::kBoot), hasStaged_(false), bundle_(exprcat::kBoot), hasBundle_(false) {}

  // Public request entry for behavior modules and the app-trigger path: true
  // when the request took effect (and staged for dispatch).
  bool request(uint8_t id, uint32_t nowMs) {
    if (exprcat::lookup(id) == nullptr) {
      return false;
    }
    if (!manager_.request(id, nowMs)) {
      return false;
    }
    staged_ = id;
    hasStaged_ = true;
    bundle_ = id;
    hasBundle_ = true;
    return true;
  }

  // Tick pump for play-once auto-release: true once per yield to neutral.
  bool tick(uint32_t nowMs) {
    if (!manager_.tick(nowMs)) {
      return false;
    }
    staged_ = manager_.active();
    hasStaged_ = true;
    bundle_ = manager_.active();
    hasBundle_ = true;
    return true;
  }

  uint8_t active() const { return manager_.active(); }
  bool hasStaged() const { return hasStaged_; }
  uint8_t staged() const { return staged_; }
  // Clears the dispatch latch after a successful link send; a failed send
  // keeps the latch so the pump retries the same id.
  void markDispatched() { hasStaged_ = false; }
  bool hasBundle() const { return hasBundle_; }
  uint8_t bundle() const { return bundle_; }
  // Marks the staged bundle conducted; the link latch is independent, so a
  // refused link send retries the face id without replaying sound/motion.
  void markConducted() { hasBundle_ = false; }

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

  // Bundle conductor (cancel-then-start over duck-typed audio + motion sinks).
  template <typename Audio, typename Motion>
  bool conductBundle(Audio& audio, Motion& motion) {
    if (!hasBundle_) {
      return false;
    }
    hasBundle_ = false;
    const exprcat::Entry* e = exprcat::lookup(bundle_);
    audio.cancelAll();
    motion.stop();
    if (e == nullptr) {
      return true;
    }
    // Neck/arm poses ride in the row but are not played in V2 (no servo HAL yet).
    const exprcue::Cue cue = exprcue::cueFor(e->sound);
    for (uint8_t i = 0; i < cue.len; ++i) {
      audio.beep(cue.notes[i].freqHz, cue.notes[i].durMs, cue.notes[i].gapMs);
    }
    const motionprim::Primitive prim = motionprim::forId(e->motion);
    if (!prim.still) {
      motion.startWiggle(prim.durationMs, prim.rateHz, prim.amplitude);
    }
    return true;
  }

 private:
  Manager manager_;
  uint8_t staged_;
  bool hasStaged_;
  uint8_t bundle_;
  bool hasBundle_;
};

}  // namespace expression
