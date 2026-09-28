#pragma once
// Thread-safe system state + event bus types (v2 architecture).
// Mutex + FreeRTOS queue wiring lands in Phase 1.

#include <stdint.h>

// Minimal maneuver vocabulary for the async-maneuver + DONE-event pattern.
// Payload-sized: BehaviorId fits SystemEvent.payload (uint32_t) by construction.
// Declared before SystemState (which owns the activeBehavior intent field).
enum BehaviorId : uint32_t {
  BEHAVIOR_NONE = 0,
  BEHAVIOR_DRIVE_FORWARD,
  BEHAVIOR_HAPPY_WIGGLE,
};

struct SystemState {
  float pitch = 0.0f;
  float roll = 0.0f;
  uint16_t distanceMM = 0;

  bool isPickedUp = false;
  bool cliffDetected = false;
  bool isDriving = false;
  bool gndFwd = true;  // fwd = ToF cliff derivation (compensated fwd rule)
  bool gndRev = true;  // rev = fail-open, no rear sensor yet (invalid-hold pins true)

  enum RobotMode { MODE_MANUAL, MODE_AUTONOMOUS, MODE_LOW_POWER, MODE_EMERGENCY } mode = MODE_MANUAL;

  // Coordinator-owned intent (v2 §D intent fan-out): WHAT is running, never
  // HOW. Actuator subsystems own private behavior→primitive maps; completion
  // closes the loop via EVENT_BEHAVIOR_DONE.
  BehaviorId activeBehavior = BEHAVIOR_NONE;
};

enum EventType {
  EVENT_CLIFF_DETECTED,
  EVENT_GROUND_CHANGED,
  EVENT_PICKED_UP,
  EVENT_UDP_COMMAND_RECEIVED,
  EVENT_BATTERY_LOW,
  EVENT_USER_TOUCH,
  EVENT_FACE_RECOGNIZED,
  EVENT_BEHAVIOR_STARTED,
  EVENT_BEHAVIOR_DONE
};

// Unified ground event (documented once, here): EVENT_GROUND_CHANGED payload
// packs both rails' levels — bit0 = gndFwd level, bit1 = gndRev level
// (1 = ground present, 0 = drop). Published on either rail's transition only;
// dead sources hold (never clear). Arduino-free helpers below.
inline uint32_t packGround(bool gndFwd, bool gndRev) { return (gndFwd ? 1u : 0u) | ((gndRev ? 1u : 0u) << 1); }

inline void unpackGround(uint32_t payload, bool& gndFwd, bool& gndRev) {
  gndFwd = (payload & 1u) != 0;
  gndRev = (payload & 2u) != 0;
}

struct SystemEvent {
  EventType type;
  uint32_t payload = 0;
  uint32_t timestampMs = 0;
};
