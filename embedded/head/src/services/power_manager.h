#pragma once
// desky-head power manager — header-only, REAL matrix (not a stub).
//
// Camera+display are power-hungry: camera {ON,OFF} + display {ON,DIM,OFF}.
// ONE command table: PowerManager::apply() is the SINGLE dispatch point
// both doors call — the USB serial CLI (lowercase verbs, laptop terminal)
// and the UART link stub (uppercase verbs, S3 side) each parse a line into
// a PowerCommand and land here. Boot applies HEAD_BOOT_LEAN (0 = validation:
// everything on; 1 = lean: camera off, display dim).
//
// Matrix rows (pure transitions in head_context.h, unit-tested):
// - cam on/off flips the sensor+task gate, display untouched.
// - oled on/dim/off; OFF gates the render task, DIM = low refresh + low
//   contrast. A face id is stored regardless of display state and rendered
//   whenever the display is next on.
// - face <id> never blanks on bad input (clampFace falls back to BOOT).
//
// Rules: no String/heap in the command path, never blocks boot (drivers own
// their absent-hardware policy; the OLED's "not found" is expected on-MB).

#include "head_context.h"

#ifdef ARDUINO
// ── Firmware: power-matrix owner ───────────────────────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"
#include "hal/camera_driver.h"
#include "middleware/face_renderer.h"
#include "services/event_bus.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef HEAD_BOOT_LEAN
#define HEAD_BOOT_LEAN 0
#endif

class PowerManager {
 public:
  PowerManager() : cam_(nullptr), disp_(nullptr), camState_(CAM_ON), oledState_(OLED_ON), faceId_(FACE_BOOT) {}

  // Latches drivers, applies the boot default, paints the boot face.
  void begin(CameraDriver* cam, FaceRenderer* disp) {
    cam_ = cam;
    disp_ = disp;
    bootPowerState(HEAD_BOOT_LEAN, camState_, oledState_);
    faceId_ = FACE_BOOT;
    applyToDrivers();
    LOG_I("PWR", "boot lean=%d cam=%s oled=%s face=%s", HEAD_BOOT_LEAN, camStateName(camState_),
          oledStateName(oledState_), faceName(faceId_));
  }

  // THE command table: every parsed line from either door lands here.
  void apply(const PowerCommand& cmd) {
    switch (cmd.target) {
      case CmdTarget::CAM: {
        const CameraState want = resolveCamCommand(camState_, cmd.arg != 0);
        if (want != camState_) {
          camState_ = want;
          if (cam_ != nullptr) {
            cam_->setPowerState(camState_ == CAM_ON);
          }
          EventBus::publish(HEAD_EVENT_CAMERA_STATE, static_cast<uint32_t>(camState_));
        }
        LOG_I("PWR", "cam=%s", camStateName(camState_));
        break;
      }
      case CmdTarget::FACE: {
        faceId_ = clampFace(cmd.arg);
        if (disp_ != nullptr) {
          disp_->showFace(faceId_);
        }
        if (oledState_ == OLED_OFF) {
          LOG_I("PWR", "face=%s stored (display off, paints on next on)", faceName(faceId_));
        } else {
          LOG_I("PWR", "face=%s", faceName(faceId_));
        }
        EventBus::publish(HEAD_EVENT_FACE_CHANGED, static_cast<uint32_t>(faceId_));
        break;
      }
      case CmdTarget::OLED: {
        if (cmd.arg < static_cast<int32_t>(OLED_OFF) || cmd.arg > static_cast<int32_t>(OLED_ON)) {
          LOG_W("PWR", "bad oled arg=%ld (want 0..2)", static_cast<long>(cmd.arg));
          break;
        }
        oledState_ = resolveOledCommand(oledState_, static_cast<DisplayState>(cmd.arg));
        if (disp_ != nullptr) {
          disp_->setPowerState(oledState_ != OLED_OFF);
          disp_->setDim(oledState_ == OLED_DIM);
          if (oledState_ != OLED_OFF) {
            disp_->showFace(faceId_);  // Repaint the stored face on every ON/DIM.
          }
        }
        LOG_I("PWR", "oled=%s", oledStateName(oledState_));
        break;
      }
      case CmdTarget::STATUS:
      case CmdTarget::HELP:
      case CmdTarget::INVALID:
        // STATUS/HELP are answered by the CLI door itself; INVALID never
        // reaches apply (doors reject at parse). Guarded, not silent.
        LOG_W("PWR", "apply ignored (no state change)");
        break;
    }
  }

  void status() const {
    const uint32_t frames = (cam_ != nullptr) ? cam_->frames() : 0;
    const uint32_t dropped = (cam_ != nullptr) ? cam_->dropped() : 0;
    const size_t lastBytes = (cam_ != nullptr) ? cam_->lastSizeBytes() : 0;
    LOG_I("PWR", "cam=%s oled=%s face=%s frames=%lu drops=%lu last=%uB fps=%lum", camStateName(camState_),
          oledStateName(oledState_), faceName(faceId_), (unsigned long)frames, (unsigned long)dropped,
          (unsigned)lastBytes, (unsigned long)((cam_ != nullptr) ? cam_->fpsMilli() : 0));
  }

  CameraState camState() const { return camState_; }
  DisplayState oledState() const { return oledState_; }
  uint8_t faceId() const { return faceId_; }

 private:
  void applyToDrivers() {
    if (cam_ != nullptr) {
      cam_->setPowerState(camState_ == CAM_ON);
    }
    if (disp_ != nullptr) {
      disp_->setPowerState(oledState_ != OLED_OFF);
      disp_->setDim(oledState_ == OLED_DIM);
      if (oledState_ != OLED_OFF) {
        disp_->showFace(faceId_);
      }
    }
  }

  CameraDriver* cam_;
  FaceRenderer* disp_;
  CameraState camState_;
  DisplayState oledState_;
  uint8_t faceId_;
};

#endif  // ARDUINO
