#pragma once
// Behavior coordinator — header-only.
//
// Owns mode transitions (HFSM + P1>P2>P3>P4 arbitrator) and intent fan-out:
// the coordinator decides WHAT (sets activeBehavior, publishes one
// EVENT_BEHAVIOR_STARTED{id}), actuator subsystems own private
// behavior→primitive maps, and completion closes the loop via
// EVENT_BEHAVIOR_DONE{id}. Outputs go through L1
// (MotionController::setVelocity/stop/brake) only — never the HAL.
//
// Top half (namespace coordinator) is Arduino-free pure logic (stdint +
// system_context.h only) so host tests include it directly. The bottom half
// (class Coordinator) is firmware-only: an EventBus subscriber + Core 1 task.
//
// Event protocol (documented once, here):
// - EVENT_UDP_COMMAND_RECEIVED payload packs a ControlPacket into the
//   SystemEvent uint32 big-endian:
//     payload = mode<<24 | throttle<<16 | steering<<8 | flags.
// - Throttle/steering bytes are uint8 center-128:
//     signed = (byte - 128) / 127, clamped to -1..1. Zero stick = 128.
//   Throttle passes into L1 raw (-1..1, no scaling).
// - EVENT_GROUND_CHANGED payload packs both ground rails
//   (packGround/unpackGround): bit0 = gndFwd level, bit1 = gndRev level
//   (1 = ground present, 0 = drop). Published on either rail's transition
//   only. Entry (brake+EMERGENCY) fires on a fresh 1→0 on either rail.
// - EVENT_BEHAVIOR_STARTED payload = BehaviorId. Continuous behaviors
//   carry no duration; timed maneuvers carry theirs in the arm call, so no
//   second word is needed.
// - EVENT_BEHAVIOR_DONE payload = BehaviorId, published by actuators only.
//   The coordinator never publishes DONE: cancellation (preempt/stale) is
//   itself coordinator-caused, so it needs no completion signal.
//
// Safety semantics (locked, per-direction latch):
// - Ground loss on either rail latches EMERGENCY: P1 entry brakes via
//   MotionController::brake() + mode=EMERGENCY + BEHAVIOR_NONE.
// - Steady-state veto per commanded direction while latched: forward needs
//   gndFwd, reverse needs gndRev, pure turn (v==0) always passes (silent
//   clamp of the vetoed component to 0, omega untouched).
// - EMERGENCY holds until a fresh nonzero UDP command arrives whose direction
//   is currently permitted (fwd needs gndFwd, rev needs gndRev, pure turn
//   needs either rail; both-false blocks every exit). Centered, stale-held,
//   or blocked-direction commands never exit; clear-without-command stays put.
//   Wheels never auto-restart.
// - Failsafe: no UDP within CFG_COORDINATOR_STALE_MS preempts everything to
//   stop (including running maneuvers).
// - Non-drive behaviors (timed maneuvers) own the L1 mailbox until DONE: a
//   centered stick does not cancel them (holdMotion), but a nonzero stick
//   (new intent) or the failsafe does.

#include <stdint.h>

#include "behavior/mode_manager.h"
#include "link/expression_catalog.h"
#include "system_context.h"

namespace coordinator {

// P1>P2>P3>P4 input snapshot. groundEvent/freshCommand/doneReceived are
// single-tick edges from the event drain; gndFwd/gndRev are the levels the
// task maintains from EVENT_GROUND_CHANGED payloads (1 = ground present);
// emergencyLatched is the persisted stop condition (set on any ground-loss
// edge, lifted only by a fresh command in a permitted direction).
struct ArbitrateInput {
  SystemState::RobotMode mode = SystemState::MODE_MANUAL;
  BehaviorId activeBehavior = BEHAVIOR_NONE;
  bool emergencyLatched = false;
  bool groundEvent = false;
  bool gndFwd = true;
  bool gndRev = true;
  float cmdV = 0.0f;
  float cmdOmega = 0.0f;
  bool hasCommand = false;
  bool freshCommand = false;
  bool udpStale = false;
  bool doneReceived = false;
  uint32_t doneId = BEHAVIOR_NONE;
};

struct ArbitrateOutput {
  float v = 0.0f;
  float omega = 0.0f;
  bool doBrake = false;
  // True = leave the L1 mailbox untouched this tick (a timed maneuver owns
  // it until DONE). False = apply v/omega via brake/stop/setVelocity.
  bool holdMotion = false;
  SystemState::RobotMode nextMode = SystemState::MODE_MANUAL;
  BehaviorId nextBehavior = BEHAVIOR_NONE;
};

// Pure priority arbitrator: P1 safety > P2 user UDP > P3 auto (stub: stop) >
// P4 idle (stub: stop). No I/O, no statics — the Coordinator task owns state
// and feeds one input per tick. The command event payload pack/unpack and the
// center-128 stick math live in transport/udp_codec.h (wire vocabulary
// home); the drain below unpacks through udp::unpackCommand.
inline ArbitrateOutput arbitrate(const ArbitrateInput& in) {
  ArbitrateOutput out;
  out.nextMode = in.mode;
  out.nextBehavior = in.activeBehavior;

  // P1 entry: fresh ground loss on either rail latches EMERGENCY + brakes,
  // even over a running maneuver (preemption, no DONE needed —
  // coordinator-caused).
  if (in.groundEvent) {
    out.v = 0.0f;
    out.omega = 0.0f;
    out.doBrake = true;
    out.holdMotion = false;
    out.nextMode = SystemState::MODE_EMERGENCY;
    out.nextBehavior = BEHAVIOR_NONE;
    return out;
  }

  // P1 latched: hold EMERGENCY until a fresh nonzero command arrives whose
  // direction is currently permitted (fwd needs gndFwd, rev needs gndRev,
  // pure turn needs either rail — both-false blocks every exit). Levels
  // persist across ticks (clear and re-command land on different ticks).
  if (in.emergencyLatched || in.mode == SystemState::MODE_EMERGENCY) {
    out.nextMode = SystemState::MODE_EMERGENCY;
    out.nextBehavior = BEHAVIOR_NONE;
    const bool wantDrive = in.freshCommand && in.hasCommand && !in.udpStale && (in.cmdV != 0.0f || in.cmdOmega != 0.0f);
    bool permitted = false;
    if (in.cmdV > 0.0f) {
      permitted = in.gndFwd;
    } else if (in.cmdV < 0.0f) {
      permitted = in.gndRev;
    } else if (in.cmdOmega != 0.0f) {
      permitted = in.gndFwd || in.gndRev;
    }
    if (wantDrive && permitted) {
      // Resume: the commanded direction is permitted, so the veto lifts.
      out.doBrake = false;
      out.holdMotion = false;
      out.nextMode = SystemState::MODE_MANUAL;
      out.nextBehavior = BEHAVIOR_DRIVE_FORWARD;
      out.v = in.cmdV;
      out.omega = in.cmdOmega;
      return out;
    }
    // Still latched (includes clear-but-no-new-command, stale-held stick,
    // freshly-centered stick, and blocked-direction commands: all stay
    // stopped in EMERGENCY, never auto-restart). Escape hatch: per-direction
    // veto — forward needs gndFwd, reverse needs gndRev, pure turn always
    // passes (silent clamp, omega untouched).
    out.doBrake = false;
    out.holdMotion = false;
    if (in.udpStale || !in.hasCommand) {
      out.v = 0.0f;
      out.omega = 0.0f;
      return out;
    }
    if (in.cmdV > 0.0f) {
      out.v = in.gndFwd ? in.cmdV : 0.0f;
    } else if (in.cmdV < 0.0f) {
      out.v = in.gndRev ? in.cmdV : 0.0f;
    } else {
      out.v = 0.0f;
    }
    out.omega = in.cmdOmega;
    return out;
  }

  // DONE advances the HFSM: a matching completion clears the active behavior
  // before P2/P4 run, so sequencing is behavior-to-behavior (v2 §D). A
  // nonzero stick (new intent) still wins below; anything else resolves to
  // idle or to the still-running maneuver.
  BehaviorId active = in.activeBehavior;
  if (in.doneReceived && in.doneId != static_cast<uint32_t>(BEHAVIOR_NONE) &&
      in.doneId == static_cast<uint32_t>(active)) {
    active = BEHAVIOR_NONE;
  }

  // Failsafe / P4 idle: stale or no command preempts everything to stop.
  if (in.udpStale || !in.hasCommand) {
    out.nextBehavior = BEHAVIOR_NONE;
    out.v = 0.0f;
    out.omega = 0.0f;
    out.holdMotion = false;
    return out;
  }

  // P2 user UDP (P3 auto is a stub that stops, so any live command is user
  // drive): nonzero stick preempts maneuvers; centered stick ends DRIVE but
  // leaves a timed maneuver running (it owns the mailbox until DONE).
  if (in.cmdV != 0.0f || in.cmdOmega != 0.0f) {
    out.nextBehavior = BEHAVIOR_DRIVE_FORWARD;
    out.v = in.cmdV;
    out.omega = in.cmdOmega;
    out.holdMotion = false;
    return out;
  }
  if (active == BEHAVIOR_DRIVE_FORWARD || active == BEHAVIOR_NONE) {
    out.nextBehavior = BEHAVIOR_NONE;
    out.v = 0.0f;
    out.omega = 0.0f;
    out.holdMotion = false;
    return out;
  }
  out.nextBehavior = active;
  out.holdMotion = true;
  return out;
}

// Safety expression map: assert edges -> scared/angry, either clear edge ->
// neutral (kBoot, which bypasses priority by manager design). Asserts win
// when assert and clear land on the same tick. No I/O, no statics — the
// firmware loop feeds one call per tick from its event drain.
inline bool safetyExpressionFor(bool groundEvent, bool obstacleEvent, uint8_t& idOut, bool groundClear = false,
                                bool obstacleClear = false) {
  if (groundEvent) {
    idOut = exprcat::kScared;
    return true;
  }
  if (obstacleEvent) {
    idOut = exprcat::kAngry;
    return true;
  }
  if (groundClear || obstacleClear) {
    idOut = exprcat::kBoot;
    return true;
  }
  return false;
}

// State-based safety annunciation: face = highest-priority active hazard.
// Ground loss on either rail outranks a latched obstacle; no hazard yields
// none. Pure levels in, no I/O — the firmware loop owns the levels.
inline bool desiredSafetyFace(bool groundLost, bool obstacleLatched, uint8_t& idOut) {
  if (groundLost) {
    idOut = exprcat::kScared;
    return true;
  }
  if (obstacleLatched) {
    idOut = exprcat::kAngry;
    return true;
  }
  return false;
}

// Ownership claim over the safety face: which id safety holds, if any.
// releaseFirst plans [kBoot, id]: the desired id sits below the showing face,
// so the tick releases through neutral first (the staged latch overwrites
// before any pump consumes it, so the wire sees one transition).
struct SafetyDecision {
  bool request;
  uint8_t id;
  bool hasHold;
  uint8_t holdId;
  bool releaseFirst;
};

// Next safety-face step for one drain tick that carried any safety edge.
// Desired present always re-requests (restates the survivor on a sibling
// clear, refreshes on re-assert), stepping down via neutral when the desired
// id sits below the showing face. Desired absent releases neutral only while
// the face still shows our hold; a stolen face just drops the claim.
inline SafetyDecision resolveSafetyEdge(bool desiredPresent, uint8_t desiredId, bool hasLast, uint8_t lastId,
                                        uint8_t activeId) {
  if (desiredPresent) {
    const bool stepDown = exprcat::priorityFor(desiredId) < exprcat::priorityFor(activeId);
    return SafetyDecision{true, desiredId, true, desiredId, stepDown};
  }
  if (hasLast && activeId == lastId) {
    return SafetyDecision{true, exprcat::kBoot, false, 0, false};
  }
  return SafetyDecision{false, 0, false, 0, false};
}

}  // namespace coordinator

#ifdef ARDUINO
// ── Firmware: Coordinator task (EventBus subscriber, Core 1) ──────────────

#include <Arduino.h>

#include "abilities/expression/expression_task.h"
#include "abilities/pairing/pairing_task.h"
#include "behavior/motion_controller.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "services/event_bus.h"
#include "transport/udp_codec.h"
// The RX-observation report pin (TelemetryManager::noteCommand) requires
// transport/udp_server.h first: every includer includes it before this header
// (udp_server.h, abilities/telemetry/telemetry_task.h, and main.cpp order them).
#include "abilities/telemetry/telemetry_manager.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
#ifndef CFG_COORDINATOR_STACK_WORDS
#define CFG_COORDINATOR_STACK_WORDS 4096
#endif
#ifndef CFG_COORDINATOR_PRIORITY_OFFSET
#define CFG_COORDINATOR_PRIORITY_OFFSET 4
#endif
#ifndef CFG_COORDINATOR_CORE
#define CFG_COORDINATOR_CORE 1
#endif
#ifndef CFG_COORDINATOR_STALE_MS
#define CFG_COORDINATOR_STALE_MS 500
#endif
#ifndef CFG_COORDINATOR_LOOP_MS
#define CFG_COORDINATOR_LOOP_MS 20
#endif

class Coordinator {
 public:
  static constexpr uint32_t kStackWords = CFG_COORDINATOR_STACK_WORDS;
  // Below Motion's +5 so the 100Hz servo never misses its deadline.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + CFG_COORDINATOR_PRIORITY_OFFSET;
  static constexpr BaseType_t kCore = CFG_COORDINATOR_CORE;
  static constexpr uint32_t kLoopMs = CFG_COORDINATOR_LOOP_MS;
  static constexpr uint32_t kStaleMs = CFG_COORDINATOR_STALE_MS;

  Coordinator()
      : motion_(nullptr),
        task_(nullptr),
        mutex_(nullptr),
        mode_(SystemState::MODE_MANUAL),
        active_(BEHAVIOR_NONE),
        emergencyLatched_(false),
        gndFwd_(true),
        gndRev_(true),
        obstacleAsserted_(false),
        hasSafety_(false),
        lastSafety_(exprcat::kBoot),
        cmdV_(0.0f),
        cmdOmega_(0.0f),
        hasCmd_(false),
        lastCmdMs_(0),
        pairing_(nullptr),
        telem_(nullptr) {}

  // Subscribes to the bus and pins the coordinator task to Core 1. Call once
  // in setup(), after MotionController::begin (the task drives L1 from its
  // first tick) with logging already up.
  bool begin(MotionController* motion) {
    DESKY_ASSERT(motion != nullptr);
    if (motion == nullptr) {
      return false;
    }
    motion_ = motion;
    sub_ = EventBus::subscribe();
    DESKY_ASSERT(sub_.valid());
    if (!sub_.valid()) {
      return false;
    }
    mutex_ = xSemaphoreCreateMutex();
    DESKY_ASSERT(mutex_ != nullptr);
    if (mutex_ == nullptr) {
      return false;
    }
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&Coordinator::taskEntry, "coord", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  // Telemetry seam: mutex-guarded copy of coordinator-owned intent {mode,
  // activeBehavior, cliffDetected level} for the UDP telemetry task. Returns
  // false before begin(). isDriving is NOT here: it lives in
  // services/TelemetryManager (telemetry-owned RX observation), fed by this
  // loop's direct noteCommand on every verified command event.
  bool snapshot(SystemState& out) {
    if (mutex_ == nullptr) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    out = snap_;
    xSemaphoreGive(mutex_);
    return true;
  }

  // Drive dispatch: behavior owns intent AND the RX observation. The
  // wire-facing half (DRIVE framing + session verify + alive funnel) lives
  // transport-inline in UdpServer::handleDrive, which publishes ONE
  // EVENT_UDP_COMMAND_RECEIVED per verified frame — transport never calls
  // here. This loop drains that event below (fresh command edge for
  // arbitration) and reports it straight to the setup-owned TelemetryManager
  // (noteCommand): timing anchored to the drain tick. No heap, no String, no
  // sockets; key/code/nonce material never logged.

  // Pins the pairing/auth session (set once from main()/setup() before the
  // radio task reads; retained for the pairing-ability seam).
  void attachPairing(pairing::PairingTask* pairing) {
    DESKY_ASSERT(pairing != nullptr);
    pairing_ = pairing;
  }

  // Pins the setup-owned telemetry manager for the direct RX-observation
  // report (noteCommand on every verified command event in the drain below).
  // Set once from main()/setup() alongside attachPairing; null-safe until
  // then (the note is skipped).
  void attachTelemetry(telemetry::TelemetryManager* telem) {
    DESKY_ASSERT(telem != nullptr);
    telem_ = telem;
  }

  // Pins the setup-owned expression owner for the safety + MANUAL-gated
  // app-trigger forwards below. Set once from main()/setup(); null-safe
  // until then (a staged trigger is dropped).
  void attachExpression(expression::Task* expr) {
    DESKY_ASSERT(expr != nullptr);
    expr_ = expr;
  }

  // Mode query the display layer reads: the active system mode (NORMAL by
  // default, PAIRING while the pair window is open, OTA/MANUAL via their
  // notifiers below). Motion is unaffected — the failsafe overlay inhibits
  // without changing the mode.
  sysmode::Mode sysMode() const { return sysMode_.mode; }

  // Event feeds for signals the coordinator cannot poll: OTA staging and the
  // app-driving actions call these (unfired while nothing drives them).
  void onOtaStaged() { sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kOtaStaged); }
  void onOtaDone() { sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kOtaDone); }
  void onManualTake() { sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kManualTakeGranted); }
  void onManualRelease() { sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kManualReleaseGranted); }

 private:
  static void taskEntry(void* arg) { static_cast<Coordinator*>(arg)->loop(); }

  void loop() {
    DESKY_ASSERT(motion_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts. (A halted WDT-subscribed task becomes a
    // panic-reboot loop; DESKY_ASSERT failures already delete-then-halt in
    // FaultManager::fail.)
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();

      // Drain the private queue (non-blocking): at most one decision per
      // tick. Edges reset every tick; the ground levels persist.
      bool groundEvent = false;
      bool groundClear = false;
      bool obstacleEvent = false;
      bool obstacleClear = false;
      bool freshCommand = false;
      bool doneReceived = false;
      uint32_t doneId = BEHAVIOR_NONE;
      SystemEvent ev;
      while (sub_.receive(ev, 0)) {
        switch (ev.type) {
          case EVENT_GROUND_CHANGED: {
            bool fwd = true;
            bool rev = true;
            unpackGround(ev.payload, fwd, rev);
            // Entry edge: fresh 1→0 on either rail. Clear edge: any prior
            // loss fully present again (both rails 1).
            const bool wasLoss = !gndFwd_ || !gndRev_;
            if ((gndFwd_ && !fwd) || (gndRev_ && !rev)) {
              groundEvent = true;
              LOG_D("COORD", "ground lost fwd=%u rev=%u", fwd ? 1u : 0u, rev ? 1u : 0u);
            } else {
              if (wasLoss && fwd && rev) {
                groundClear = true;
              }
              LOG_D("COORD", "ground update fwd=%u rev=%u", fwd ? 1u : 0u, rev ? 1u : 0u);
            }
            gndFwd_ = fwd;
            gndRev_ = rev;
            break;
          }
          case EVENT_UDP_COMMAND_RECEIVED: {
            uint8_t mode = 0;
            uint8_t flags = 0;
            udp::unpackCommand(ev.payload, mode, cmdV_, cmdOmega_, flags);
            hasCmd_ = true;
            lastCmdMs_ = nowMs;
            freshCommand = true;
            // Report the verified command straight to the setup-owned manager
            // (drives TelemetryManager::isDriving). Null-safe until
            // attachTelemetry lands.
            if (telem_ != nullptr) {
              telem_->noteCommand(cmdV_, cmdOmega_, nowMs);
            }
            LOG_EVERY_N(50, "COORD", "udp mode=%u v=%.2f w=%.2f flags=%u", mode, cmdV_, cmdOmega_, flags);
            break;
          }
          case EVENT_BEHAVIOR_DONE:
            doneReceived = true;
            doneId = ev.payload;
            LOG_D("COORD", "done id=%lu", (unsigned long)doneId);
            break;
          case EVENT_OBSTACLE_DETECTED:
            obstacleEvent = true;
            obstacleAsserted_ = true;
            LOG_D("COORD", "obstacle dist=%lu", (unsigned long)ev.payload);
            break;
          case EVENT_OBSTACLE_CLEARED:
            obstacleClear = true;
            obstacleAsserted_ = false;
            LOG_D("COORD", "obstacle cleared dist=%lu", (unsigned long)ev.payload);
            break;
          default:
            break;
        }
      }

      // Unsigned subtraction: wrap-safe across millis() rollover.
      const bool stale = !hasCmd_ || (nowMs - lastCmdMs_ > kStaleMs);

      // Mode policy feed from existing signals only: the pairing session's
      // window/paired reads, plus the alive-fresh poll below (same window +
      // predicate as the telemetry gate, no new timing). OTA/manual events
      // arrive via the notifiers; unfired, the mode rests on NORMAL.
      if (pairing_ != nullptr) {
        if (pairing_->pairWindowOpen(nowMs)) {
          sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kPairWindowOpened);
        } else if (pairing_->isPaired()) {
          sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kPaired);
        } else {
          sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kPairWindowClosed);
        }
      }
      // MANUAL session poll: verified MANUAL_TAKE/RELEASE frames stage
      // consume-on-read flags in the pairing ability; drain them here into
      // the mode-policy notifiers (each staged frame fires at most once).
      if (pairing_ != nullptr) {
        if (pairing_->takeManualTake()) {
          onManualTake();
        }
        if (pairing_->takeManualRelease()) {
          onManualRelease();
        }
      }
      // MANUAL exit on the alive clock: the session holds while the paired
      // peer stays alive-fresh, independent of the drive clock. Command-stale
      // only inhibits motion via motionInhibited below, never exits. A dropped
      // peer (DISCONNECT/UNPAIR refresh-then-clear) reads stale here, so the
      // goodbye lands back in NORMAL on the next tick.
      if (pairing_ != nullptr) {
        if (!pairing_->isAliveFresh(nowMs)) {
          sysMode_ = sysmode::apply(sysMode_, sysmode::Event::kAliveStale);
        }
      }

      // Drive gate seam: the mode policy decides whether stick intent may
      // reach the motion layer. MANUAL-only: verified drive still updates
      // the command clock + telemetry observation in every mode, but intent
      // reaches motion solely while the MANUAL session is held. Null
      // pairing_ (before attach) allows, as before.
      const bool driveAllowed = sysmode::manualDriveAllowed(sysMode_.mode, pairing_ == nullptr || pairing_->isPaired());
      const bool inhibited = sysmode::motionInhibited(sysMode_.mode, stale);

      coordinator::ArbitrateInput in;
      in.mode = mode_;
      in.activeBehavior = active_;
      in.emergencyLatched = emergencyLatched_;
      in.groundEvent = groundEvent;
      in.gndFwd = gndFwd_;
      in.gndRev = gndRev_;
      in.cmdV = cmdV_;
      in.cmdOmega = cmdOmega_;
      in.hasCommand = hasCmd_ && !inhibited && driveAllowed;
      in.freshCommand = freshCommand;
      in.udpStale = stale;
      in.doneReceived = doneReceived;
      in.doneId = doneId;
      const coordinator::ArbitrateOutput out = coordinator::arbitrate(in);

      if (out.doBrake) {
        motion_->brake();
      } else if (!out.holdMotion) {
        if (out.v == 0.0f && out.omega == 0.0f) {
          motion_->stop();
        } else {
          motion_->setVelocity(out.v, out.omega);
        }
      }

      // Safety fires in every mode: deliberately bypasses the MANUAL app-trigger gate below.
      if (expr_ != nullptr && (groundEvent || obstacleEvent || groundClear || obstacleClear)) {
        uint8_t desiredId = 0;
        const bool desired = coordinator::desiredSafetyFace(!gndFwd_ || !gndRev_, obstacleAsserted_, desiredId);
        const coordinator::SafetyDecision safety =
            coordinator::resolveSafetyEdge(desired, desiredId, hasSafety_, lastSafety_, expr_->active());
        if (safety.request) {
          // Claim only what safety actually holds: a rejected final request
          // leaves the previous claim intact, so a later clear still matches
          // the face.
          // A planned step-down releases through neutral first; each accepted
          // request overwrites the staged latch before any pump consumes it,
          // so the wire sees one transition.
          if (safety.releaseFirst) {
            expr_->request(exprcat::kBoot, nowMs);
          }
          if (expr_->request(safety.id, nowMs)) {
            hasSafety_ = safety.hasHold;
            lastSafety_ = safety.holdId;
          }
        } else {
          hasSafety_ = safety.hasHold;
          lastSafety_ = safety.holdId;
        }
      }
      // Expression-trigger poll: a verified EXPR_TRIGGER stages a
      // consume-on-read id in the pairing ability; forward it to the
      // expression owner only while sysmode MANUAL is held, else drop it
      // with no reply. Unknown ids fail closed inside request (core
      // preemption runs; dispatch to head flows through the staged pump).
      if (pairing_ != nullptr && expr_ != nullptr) {
        uint8_t exprId = 0;
        if (pairing_->takeExprTrigger(exprId) && sysmode::exprTriggerAllowed(sysMode_.mode)) {
          expr_->request(exprId, nowMs);
        }
      }

      if (out.nextMode != mode_) {
        LOG_I("COORD", "mode %d->%d gndEv=%d gndFwd=%d gndRev=%d stale=%d", static_cast<int>(mode_),
              static_cast<int>(out.nextMode), groundEvent, gndFwd_ ? 1 : 0, gndRev_ ? 1 : 0, stale);
      }
      if (out.nextBehavior != active_) {
        LOG_I("COORD", "behavior %lu->%lu", (unsigned long)active_, (unsigned long)out.nextBehavior);
        if (out.nextBehavior != BEHAVIOR_NONE) {
          EventBus::publish(EVENT_BEHAVIOR_STARTED, static_cast<uint32_t>(out.nextBehavior));
        }
      }
      mode_ = out.nextMode;
      active_ = out.nextBehavior;
      emergencyLatched_ = (mode_ == SystemState::MODE_EMERGENCY);

      // Shadow the intent fields the telemetry task snapshots (under mutex;
      // the arbitration fields above stay task-local, untouched).
      xSemaphoreTake(mutex_, portMAX_DELAY);
      snap_.mode = mode_;
      snap_.activeBehavior = active_;
      snap_.cliffDetected = !gndFwd_;  // raw fwd flag: wire untouched
      snap_.manual = (sysMode_.mode == sysmode::Mode::kManual);
      snap_.gndFwd = gndFwd_;
      snap_.gndRev = gndRev_;
      xSemaphoreGive(mutex_);

      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (5000 / kLoopMs) == 0) {
        Diagnostics::logWatermarks("COORD");
      }
    }
  }

  MotionController* motion_;
  TaskHandle_t task_;
  EventBus::Subscription sub_;
  SemaphoreHandle_t mutex_;
  SystemState snap_;  // Shadow for snapshot(); written by task, read under mutex.
  SystemState::RobotMode mode_;
  sysmode::State sysMode_;  // System mode policy (NORMAL default; PAIRING while the pair window is open).
  BehaviorId active_;
  bool emergencyLatched_;
  bool gndFwd_;
  bool gndRev_;
  bool obstacleAsserted_;
  bool hasSafety_;
  uint8_t lastSafety_;
  float cmdV_;
  float cmdOmega_;
  bool hasCmd_;
  uint32_t lastCmdMs_;
  pairing::PairingTask* pairing_;
  // Expression owner (setup-owned; pinned by attachExpression()). The
  // coordinator owns no face state — the ability does.
  expression::Task* expr_ = nullptr;
  // Telemetry RX-observation report target (setup-owned; pinned by
  // attachTelemetry()). The coordinator owns no driving flag — the manager
  // does.
  telemetry::TelemetryManager* telem_ = nullptr;
};

#endif  // ARDUINO
