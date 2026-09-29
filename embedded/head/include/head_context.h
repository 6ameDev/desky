#pragma once
// desky-head shared vocabulary — Arduino-free pure logic (stdint/string
// only), mirroring core's include/system_context.h. Single home for the
// face ids, the power-matrix states, the ONE command-table verb struct both
// doors (USB CLI + UART link stub) dispatch through, and the head event
// types bound to the shared bus mechanics (EventBusT) by
// src/services/event_bus.h.
//
// Host Unity tests include this header directly (like core's fusion/
// coordinator pure tops): every mapping/transition below is unit-covered.

#include <stdint.h>
#include <string.h>

// ── Face ids (procedural art lives in middleware/face_renderer.h) ──
enum FaceId : uint8_t {
  FACE_BOOT = 0,  // neutral squares — power-on default
  FACE_HAPPY = 1,
  FACE_SAD = 2,
  FACE_BLINK = 3,  // line eyes — also the DIM-mode resting face
  FACE_ALERT = 4,  // wide eyes + O mouth
  FACE_COUNT = 5,
};

inline const char* faceName(uint8_t id) {
  switch (id) {
    case FACE_BOOT:
      return "boot";
    case FACE_HAPPY:
      return "happy";
    case FACE_SAD:
      return "sad";
    case FACE_BLINK:
      return "blink";
    case FACE_ALERT:
      return "alert";
    default:
      return "unknown";
  }
}

// Name (lowercase, CLI dialect) -> id; 0xFF when unknown.
inline uint8_t faceIdFromName(const char* name) {
  if (name == nullptr) {
    return 0xFF;
  }
  if (strcmp(name, "boot") == 0) {
    return FACE_BOOT;
  }
  if (strcmp(name, "happy") == 0) {
    return FACE_HAPPY;
  }
  if (strcmp(name, "sad") == 0) {
    return FACE_SAD;
  }
  if (strcmp(name, "blink") == 0) {
    return FACE_BLINK;
  }
  if (strcmp(name, "alert") == 0) {
    return FACE_ALERT;
  }
  return 0xFF;
}

// Any out-of-range id falls back to BOOT (never blank the face on bad input).
inline uint8_t clampFace(int id) {
  if (id < 0 || id >= static_cast<int>(FACE_COUNT)) {
    return FACE_BOOT;
  }
  return static_cast<uint8_t>(id);
}

// ── Power-matrix states (camera+display are power-hungry) ──
enum CameraState : uint8_t { CAM_OFF = 0, CAM_ON = 1 };

enum DisplayState : uint8_t { OLED_OFF = 0, OLED_DIM = 1, OLED_ON = 2 };

inline const char* camStateName(CameraState s) { return s == CAM_ON ? "on" : "off"; }

inline const char* oledStateName(DisplayState s) {
  switch (s) {
    case OLED_ON:
      return "on";
    case OLED_DIM:
      return "dim";
    default:
      return "off";
  }
}

// Boot defaults from HEAD_BOOT_LEAN: 0 = validation (everything on),
// nonzero = lean (camera off, display dim). Pure so host tests pin it.
inline void bootPowerState(int lean, CameraState& cam, DisplayState& oled) {
  if (lean != 0) {
    cam = CAM_OFF;
    oled = OLED_DIM;
  } else {
    cam = CAM_ON;
    oled = OLED_ON;
  }
}

// Matrix transitions (each row unit-tested):
// - cam: on/off flips the sensor+task gate, display untouched.
// - oled: on/dim/off; OFF gates the render task, DIM = low refresh +
//   low contrast, ON = full 25fps. A face id is stored regardless of the
//   display state (render applies it whenever the display is next on).
inline CameraState resolveCamCommand(CameraState cur, bool on) {
  (void)cur;
  return on ? CAM_ON : CAM_OFF;
}

inline DisplayState resolveOledCommand(DisplayState cur, DisplayState want) {
  (void)cur;
  return want;
}

// ── ONE command table: both doors dispatch through this struct ──
// USB serial CLI (lowercase verbs, laptop terminal) and the UART link stub
// (uppercase verbs, S3 side) each parse a line into a PowerCommand and call
// the single PowerManager::apply() entry point. Framing/CRC is task 3.
enum class CmdTarget : uint8_t { CAM, FACE, OLED, STATUS, HELP, INVALID };

struct PowerCommand {
  CmdTarget target = CmdTarget::INVALID;
  int32_t arg = 0;  // CAM: 0/1 off/on; FACE: face id; OLED: DisplayState value
};

// ── Head events (bound to shared EventBusT by services/event_bus.h) ──
enum HeadEventType : uint32_t {
  HEAD_EVENT_FACE_CHANGED = 0,  // payload = FaceId now shown (or pending)
  HEAD_EVENT_CAMERA_STATE = 1,  // payload = CameraState (0/1)
  HEAD_EVENT_LINK_CMD = 2,      // payload = raw CmdTarget value received over UART
  HEAD_EVENT_COUNT = 3,
};

struct HeadEvent {
  HeadEventType type;
  uint32_t payload = 0;
  uint32_t timestampMs = 0;
};

inline const char* headEventName(HeadEventType type) {
  switch (type) {
    case HEAD_EVENT_FACE_CHANGED:
      return "FACE_CHANGED";
    case HEAD_EVENT_CAMERA_STATE:
      return "CAMERA_STATE";
    case HEAD_EVENT_LINK_CMD:
      return "LINK_CMD";
    default:
      return "UNKNOWN";
  }
}
