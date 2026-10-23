#pragma once
// Power manager (header-only): camera {ON,OFF} + display {ON,DIM,OFF} rails.
// CAM/OLED route here via HeadCoordinator::dispatch; no String/heap; never
// blocks boot (absent OLED on-MB is expected).

#include "head_context.h"

// Arduino-free command-value table: one definition shared by firmware + host tests.
namespace headapply {

// False when arg is not a DisplayState value.
inline bool resolveOled(int32_t arg, DisplayState& out) {
  if (arg < static_cast<int32_t>(OLED_OFF) || arg > static_cast<int32_t>(OLED_ON)) {
    return false;
  }
  out = static_cast<DisplayState>(arg);
  return true;
}

inline CameraState resolveCam(int32_t arg) { return (arg != 0) ? CAM_ON : CAM_OFF; }

inline uint8_t resolveFace(int32_t arg) { return clampFace(arg); }

}  // namespace headapply

#ifdef ARDUINO
// ── Firmware: power-matrix owner ───────────────────────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"
#include "hal/camera_driver.h"
#include "services/display_port.h"
#include "services/event_bus.h"

#ifndef HEAD_BOOT_LEAN
#define HEAD_BOOT_LEAN 0
#endif

class PowerManager {
 public:
  PowerManager() : cam_(nullptr), disp_(nullptr), camState_(CAM_ON), oledState_(OLED_ON), eyesId_(FACE_BOOT) {}

  void begin(CameraDriver* cam, IDisplayPort* disp) {
    cam_ = cam;
    disp_ = disp;
    bootPowerState(HEAD_BOOT_LEAN, camState_, oledState_);
    eyesId_ = FACE_BOOT;
    applyToDrivers();
    LOG_I("PWR", "boot lean=%d cam=%s oled=%s face=%s", HEAD_BOOT_LEAN, camStateName(camState_),
          oledStateName(oledState_), faceName(eyesId_));
  }

  // Rails entry: CAM/OLED only. Anything else is guarded, never silent.
  void apply(const PowerCommand& cmd) {
    switch (cmd.target) {
      case CmdTarget::CAM: {
        const CameraState want = headapply::resolveCam(cmd.arg);
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
      case CmdTarget::OLED: {
        DisplayState want = OLED_OFF;
        if (!headapply::resolveOled(cmd.arg, want)) {
          LOG_W("PWR", "bad oled arg=%ld (want 0..2)", static_cast<long>(cmd.arg));
          break;
        }
        oledState_ = resolveOledCommand(oledState_, want);
        if (disp_ != nullptr) {
          disp_->setPowerState(oledState_ != OLED_OFF);
          disp_->setDim(oledState_ == OLED_DIM);
          if (oledState_ != OLED_OFF) {
            disp_->repaint();  // Repaint the live mode (QR/pairing survive ON/DIM; FACE clears them).
          }
        }
        LOG_I("PWR", "oled=%s", oledStateName(oledState_));
        break;
      }
      default:
        // Guarded, not silent.
        LOG_W("PWR", "apply ignored (no state change)");
        break;
    }
  }

  void status() const {
    const uint32_t frames = (cam_ != nullptr) ? cam_->frames() : 0;
    const uint32_t dropped = (cam_ != nullptr) ? cam_->dropped() : 0;
    const size_t lastBytes = (cam_ != nullptr) ? cam_->lastSizeBytes() : 0;
    LOG_I("PWR", "cam=%s oled=%s face=%s frames=%lu drops=%lu last=%uB fps=%lum", camStateName(camState_),
          oledStateName(oledState_), faceName(eyesId_), (unsigned long)frames, (unsigned long)dropped,
          (unsigned)lastBytes, (unsigned long)((cam_ != nullptr) ? cam_->fpsMilli() : 0));
  }

  CameraState camState() const { return camState_; }
  DisplayState oledState() const { return oledState_; }
  uint8_t eyesId() const { return eyesId_; }

  void setEyesId(uint8_t id) { eyesId_ = id; }

 private:
  void applyToDrivers() {
    if (cam_ != nullptr) {
      cam_->setPowerState(camState_ == CAM_ON);
    }
    if (disp_ != nullptr) {
      disp_->setPowerState(oledState_ != OLED_OFF);
      disp_->setDim(oledState_ == OLED_DIM);
      if (oledState_ != OLED_OFF) {
        disp_->repaint();
      }
    }
  }

  CameraDriver* cam_;
  IDisplayPort* disp_;
  CameraState camState_;
  DisplayState oledState_;
  uint8_t eyesId_;
};

#endif  // ARDUINO
