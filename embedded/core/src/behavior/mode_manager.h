#pragma once
// System mode policy — behavior/ orchestration.
//
// Four mutually exclusive modes: NORMAL (autonomous), MANUAL (app driving),
// PAIRING, OTA. apply() is the single explicit transition function over
// events; the coordinator feeds it from existing signals (pair window
// open/closed + paired reads, OTA notifiers, the stale bit it already
// computes). Arduino-free pure logic (stdint only) so host tests include it
// directly.
//
// The comms failsafe is an overlay guard, not a mode: motionInhibited()
// inhibits motion in every mode without changing it.

#include <stdint.h>

namespace sysmode {

// Mutually exclusive system modes. NORMAL is the default and the exit target
// of every other mode.
enum class Mode : uint8_t { kNormal, kManual, kPairing, kOta };

// Transition events. The manual take/release values exist for the app-driving
// actions to fire later; nothing fires them yet, so behavior stays NORMAL.
// OTA staged/started both enter OTA (staging hands off to the updater with no
// observable seam between them).
enum class Event : uint8_t {
  kPairWindowOpened,
  kPaired,
  kPairWindowClosed,
  kOtaStaged,
  kOtaStarted,
  kOtaDone,
  kOtaAborted,
  kManualTakeGranted,
  kManualReleaseGranted,
  kCommandStale,
};

// Display side effect of the mode. The head keeps showing the pairing PIN
// while PAIRING, the progress bar while OTA, and the face otherwise; the
// PairShow/PairDone link frames carrying those states are unchanged.
enum class Display : uint8_t { kFace, kPairing, kOta };

struct State {
  Mode mode = Mode::kNormal;
};

// Explicit transition function. Priority: OTA preempts everything (a window
// opened mid-update stays an update); PAIRING preempts NORMAL/MANUAL. Every
// exit lands on NORMAL. Repeated or out-of-mode events are idempotent no-ops,
// so the coordinator may feed edge levels every tick.
inline State apply(State s, Event e) {
  switch (e) {
    case Event::kOtaStaged:
    case Event::kOtaStarted:
      s.mode = Mode::kOta;
      return s;
    case Event::kOtaDone:
    case Event::kOtaAborted:
      if (s.mode == Mode::kOta) {
        s.mode = Mode::kNormal;
      }
      return s;
    case Event::kPairWindowOpened:
      if (s.mode != Mode::kOta) {
        s.mode = Mode::kPairing;
      }
      return s;
    case Event::kPaired:
    case Event::kPairWindowClosed:
      if (s.mode == Mode::kPairing) {
        s.mode = Mode::kNormal;
      }
      return s;
    case Event::kManualTakeGranted:
      if (s.mode == Mode::kNormal) {
        s.mode = Mode::kManual;
      }
      return s;
    case Event::kManualReleaseGranted:
      if (s.mode == Mode::kManual) {
        s.mode = Mode::kNormal;
      }
      return s;
    case Event::kCommandStale:
      if (s.mode == Mode::kManual) {
        s.mode = Mode::kNormal;
      }
      return s;
  }
  return s;
}

// Failsafe overlay: inhibits motion in every mode without changing the mode.
// commandStale is the coordinator's existing failsafe input (derived from its
// own stale threshold); this function adds no new timing.
inline bool motionInhibited(Mode mode, bool commandStale) {
  (void)mode;
  return commandStale;
}

// Drive gating hook, consulted by the coordinator before stick intent reaches
// the motion layer. Preserves current behavior exactly: stick intent is
// honored whenever the session verifies (the transport only publishes
// verified command events, so unpaired ticks carry no intent either way).
// Narrow this to the manual mode once the take/release actions land.
inline bool manualDriveAllowed(Mode mode, bool paired) {
  (void)mode;
  return paired;
}

// Mode query the display layer reads: which overlay follows the active mode.
inline Display displayFor(Mode mode) {
  switch (mode) {
    case Mode::kPairing:
      return Display::kPairing;
    case Mode::kOta:
      return Display::kOta;
    case Mode::kNormal:
    case Mode::kManual:
    default:
      return Display::kFace;
  }
}

}  // namespace sysmode
