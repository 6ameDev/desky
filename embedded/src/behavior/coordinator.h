#pragma once
// desky v2 behavior coordinator — header-only, Phase 4 skeleton.
//
// Owns mode transitions (v2 §D HFSM + P1>P2>P3>P4 arbitrator) and intent
// fan-out: the coordinator decides WHAT (sets activeBehavior, publishes one
// EVENT_BEHAVIOR_STARTED{id}), actuator subsystems own private
// behavior→primitive maps, and completion closes the loop via
// EVENT_BEHAVIOR_DONE{id} (v2 §D). Outputs go through L1
// (MotionController::setVelocity/stop/brake) only — never the HAL.
//
// Structure: the top half (namespace coordinator) is Arduino-free pure logic
// (stdint + system_context.h only) so host Unity tests include this header
// directly. The bottom half (class Coordinator) is firmware-only (#ifdef
// ARDUINO): an EventBus subscriber + Core 1 task.
//
// Event protocol (documented once, here):
// - EVENT_UDP_COMMAND_RECEIVED payload packs a ControlPacket (udp_codec.h)
//   into the SystemEvent uint32 big-endian:
//     payload = mode<<24 | throttle<<16 | steering<<8 | flags.
// - Throttle/steering bytes are uint8 center-128:
//     signed = (byte - 128) / 127, clamped to -1..1. Zero stick = 128.
//   Throttle passes into L1 raw (-1..1, no scaling).
// - EVENT_GROUND_CHANGED payload packs both ground rails (system_context.h
//   packGround/unpackGround): bit0 = gndFwd level, bit1 = gndRev level
//   (1 = ground present, 0 = drop). Published on either rail's transition
//   only. Entry (brake+EMERGENCY) fires on a fresh 1→0 on either rail.
// - EVENT_BEHAVIOR_STARTED payload = BehaviorId. Continuous behaviors
//   (DRIVE_FORWARD) carry no duration; timed maneuvers carry theirs in the
//   arm call (cf. MotionController::startWiggle), so no second word is needed.
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
// - Non-drive behaviors (timed maneuvers armed by future P3 code) own the L1
//   mailbox until DONE: a centered stick does not cancel them (holdMotion),
//   but a nonzero stick (new intent) or the failsafe does.

#include <stdint.h>

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

// UDP stick byte (center-128) -> L1 unit (-1..1). Exact: 128 maps to 0.0f,
// 255 to +1.0, 0 clamps to -1.0.
inline float byteToUnit(uint8_t raw) {
  float f = (static_cast<float>(static_cast<int>(raw) - 128)) / 127.0f;
  if (f > 1.0f) {
    return 1.0f;
  }
  if (f < -1.0f) {
    return -1.0f;
  }
  return f;
}

// Unit (-1..1) -> stick byte, inverse of byteToUnit (for tests/synthetic).
inline uint8_t unitToByte(float v) {
  if (v > 1.0f) {
    v = 1.0f;
  }
  if (v < -1.0f) {
    v = -1.0f;
  }
  int b = static_cast<int>(v * 127.0f + 128.0f + (v >= 0.0f ? 0.5f : -0.5f));
  if (b > 255) {
    return 255;
  }
  if (b < 0) {
    return 0;
  }
  return static_cast<uint8_t>(b);
}

// Big-endian pack: payload = mode<<24|throttle<<16|steering<<8|flags.
inline uint32_t packCommand(uint8_t mode, uint8_t throttle, uint8_t steering, uint8_t flags) {
  return (static_cast<uint32_t>(mode) << 24) | (static_cast<uint32_t>(throttle) << 16) |
         (static_cast<uint32_t>(steering) << 8) | static_cast<uint32_t>(flags);
}

inline void unpackCommand(uint32_t payload, uint8_t& mode, float& v, float& omega, uint8_t& flags) {
  mode = static_cast<uint8_t>((payload >> 24) & 0xFF);
  v = byteToUnit(static_cast<uint8_t>((payload >> 16) & 0xFF));
  omega = byteToUnit(static_cast<uint8_t>((payload >> 8) & 0xFF));
  flags = static_cast<uint8_t>(payload & 0xFF);
}

// Pure priority arbitrator: P1 safety > P2 user UDP > P3 auto (stub: stop) >
// P4 idle (stub: stop). No I/O, no statics — the Coordinator task owns state
// and feeds one input per tick.
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

}  // namespace coordinator

#ifdef ARDUINO
// ── Firmware: Coordinator task (EventBus subscriber, Core 1) ──────────────

#include <Arduino.h>

#include "behavior/motion_controller.h"
#include "config.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/logger.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as sensor_fusion.h).
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
        cmdV_(0.0f),
        cmdOmega_(0.0f),
        hasCmd_(false),
        lastCmdMs_(0) {}

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

  // Telemetry seam (Workstream B, additive only — arbitration untouched):
  // mutex-guarded copy of coordinator-owned intent {mode, activeBehavior,
  // cliffDetected level} for the UDP telemetry task. Returns false before
  // begin(). isDriving is NOT here: the server tracks it locally from RX
  // freshness (see udp_server.h).
  bool snapshot(SystemState& out) {
    if (mutex_ == nullptr) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    out = snap_;
    xSemaphoreGive(mutex_);
    return true;
  }

 private:
  static void taskEntry(void* arg) { static_cast<Coordinator*>(arg)->loop(); }

  void loop() {
    DESKY_ASSERT(motion_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kLoopMs);
    uint32_t ticks = 0;
    // Never returns / never halts by design: a halted task that stays
    // WDT-subscribed becomes a panic-reboot loop, so there is no halt path
    // here at all (DESKY_ASSERT failures already delete-then-halt in
    // FaultManager::fail).
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();

      // Drain the private queue (non-blocking): at most one decision per
      // tick. Edges reset every tick; the ground levels persist (resume needs
      // clear-then-recommand across ticks, never same-tick coincidence).
      bool groundEvent = false;
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
            // Entry edge: fresh 1→0 on either rail. Clear edges (0→1) only
            // update the levels below.
            if ((gndFwd_ && !fwd) || (gndRev_ && !rev)) {
              groundEvent = true;
              LOG_D("COORD", "ground lost fwd=%u rev=%u", fwd ? 1u : 0u, rev ? 1u : 0u);
            } else {
              LOG_D("COORD", "ground update fwd=%u rev=%u", fwd ? 1u : 0u, rev ? 1u : 0u);
            }
            gndFwd_ = fwd;
            gndRev_ = rev;
            break;
          }
          case EVENT_UDP_COMMAND_RECEIVED: {
            uint8_t mode = 0;
            uint8_t flags = 0;
            coordinator::unpackCommand(ev.payload, mode, cmdV_, cmdOmega_, flags);
            hasCmd_ = true;
            lastCmdMs_ = nowMs;
            freshCommand = true;
            LOG_EVERY_N(50, "COORD", "udp mode=%u v=%.2f w=%.2f flags=%u", mode, cmdV_, cmdOmega_, flags);
            break;
          }
          case EVENT_BEHAVIOR_DONE:
            doneReceived = true;
            doneId = ev.payload;
            LOG_D("COORD", "done id=%lu", (unsigned long)doneId);
            break;
          default:
            break;
        }
      }

      // Unsigned subtraction: wrap-safe across the ~49-day millis() rollover.
      const bool stale = !hasCmd_ || (nowMs - lastCmdMs_ > kStaleMs);

      coordinator::ArbitrateInput in;
      in.mode = mode_;
      in.activeBehavior = active_;
      in.emergencyLatched = emergencyLatched_;
      in.groundEvent = groundEvent;
      in.gndFwd = gndFwd_;
      in.gndRev = gndRev_;
      in.cmdV = cmdV_;
      in.cmdOmega = cmdOmega_;
      in.hasCommand = hasCmd_ && !stale;
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
  BehaviorId active_;
  bool emergencyLatched_;
  bool gndFwd_;
  bool gndRev_;
  float cmdV_;
  float cmdOmega_;
  bool hasCmd_;
  uint32_t lastCmdMs_;
};

#endif  // ARDUINO
