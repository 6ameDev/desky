#pragma once
// Shared vocabulary (Arduino-free, host-tested): face ids, power-matrix
// states, the command verb struct, head event types.

#include <stdint.h>
#include <string.h>

#include "link/expression_catalog.h"

// Face ids
// FACE ids are the frozen wire vocabulary; Display (abilities/display) is the ability name.
enum FaceId : uint8_t {
  FACE_BOOT = 0,  // neutral squares — power-on default
  FACE_HAPPY = 1,
  FACE_SAD = 2,
  FACE_BLINK = 3,
  FACE_ALERT = 4,
  // Entered ONLY via showQr(); never via a FACE id.
  FACE_QR = 5,
  FACE_CURIOUS = 6,
  FACE_SURPRISED = 7,
  FACE_COUNT = 8,
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
    case FACE_QR:
      return "qr";
    case FACE_CURIOUS:
      return "curious";
    case FACE_SURPRISED:
      return "surprised";
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
  if (strcmp(name, "curious") == 0) {
    return FACE_CURIOUS;
  }
  if (strcmp(name, "surprised") == 0) {
    return FACE_SURPRISED;
  }
  return 0xFF;
}

// Multiplexed protocol, separate vocabularies: FACE frames carry legacy ids and EXPR frames carry catalog ids,
// both failing closed to BOOT through the shared catalog lookup. QR id 5 has no catalog row and stays reserved.
inline uint8_t clampFace(int id) {
  if (id < 0 || id >= static_cast<int>(FACE_COUNT)) {
    return FACE_BOOT;
  }
  return exprcat::lookup(static_cast<uint8_t>(id)) == nullptr ? static_cast<uint8_t>(FACE_BOOT)
                                                              : static_cast<uint8_t>(id);
}

// Power-matrix states
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

// Boot defaults from HEAD_BOOT_LEAN.
inline void bootPowerState(int lean, CameraState& cam, DisplayState& oled) {
  if (lean != 0) {
    cam = CAM_OFF;
    oled = OLED_DIM;
  } else {
    cam = CAM_ON;
    oled = OLED_ON;
  }
}

// Matrix transitions (each row unit-tested).
inline CameraState resolveCamCommand(CameraState cur, bool on) {
  (void)cur;
  return on ? CAM_ON : CAM_OFF;
}

inline DisplayState resolveOledCommand(DisplayState cur, DisplayState want) {
  (void)cur;
  return want;
}

// Both doors dispatch through this struct.
enum class CmdTarget : uint8_t { CAM, FACE, OLED, STATUS, HELP, INVALID, LINK, SNAP, OTA };

struct PowerCommand {
  CmdTarget target = CmdTarget::INVALID;
  int32_t arg = 0;  // CAM: 0/1; FACE: id; OLED: DisplayState
};

// Head events
enum HeadEventType : uint32_t {
  HEAD_EVENT_FACE_CHANGED = 0,
  HEAD_EVENT_CAMERA_STATE = 1,
  HEAD_EVENT_LINK_CMD = 2,
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
