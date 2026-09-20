#pragma once
// Thread-safe system state + event bus types (v2 architecture).
// Mutex + FreeRTOS queue wiring lands in Phase 1.

#include <stdint.h>

struct SystemState {
  float pitch = 0.0f;
  float roll = 0.0f;
  uint16_t distanceMM = 0;

  bool isPickedUp = false;
  bool cliffDetected = false;
  bool isDriving = false;

  enum RobotMode { MODE_MANUAL, MODE_AUTONOMOUS, MODE_LOW_POWER, MODE_EMERGENCY } mode = MODE_MANUAL;
};

enum EventType {
  EVENT_CLIFF_DETECTED,
  EVENT_PICKED_UP,
  EVENT_UDP_COMMAND_RECEIVED,
  EVENT_BATTERY_LOW,
  EVENT_USER_TOUCH,
  EVENT_FACE_RECOGNIZED,
  EVENT_BEHAVIOR_STARTED,
  EVENT_BEHAVIOR_DONE
};

struct SystemEvent {
  EventType type;
  uint32_t payload = 0;
  uint32_t timestampMs = 0;
};
