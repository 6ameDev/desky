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

#include "abilities/face/face_task.h"
#include "common/logger.h"
#include "config.h"
#include "hal/camera_driver.h"
#include "services/event_bus.h"

#ifndef HEAD_BOOT_LEAN
#define HEAD_BOOT_LEAN 0
#endif

class PowerManager {
 public:
  PowerManager() : cam_(nullptr), disp_(nullptr), camState_(CAM_ON), oledState_(OLED_ON), faceId_(FACE_BOOT) {}

  void begin(CameraDriver* cam, FaceRenderer* disp) {
    cam_ = cam;
    disp_ = disp;
    bootPowerState(HEAD_BOOT_LEAN, camState_, oledState_);
    faceId_ = FACE_BOOT;
    applyToDrivers();
    LOG_I("PWR", "boot lean=%d cam=%s oled=%s face=%s", HEAD_BOOT_LEAN, camStateName(camState_),
          oledStateName(oledState_), faceName(faceId_));
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
    const bool pair = disp_ != nullptr && disp_->pairActive();
    LOG_I("PWR", "cam=%s oled=%s face=%s qr=%s pair=%s frames=%lu drops=%lu last=%uB fps=%lum", camStateName(camState_),
          oledStateName(oledState_), faceName(faceId_), qrActive() ? "on" : "off", pair ? "on" : "off",
          (unsigned long)frames, (unsigned long)dropped, (unsigned)lastBytes,
          (unsigned long)((cam_ != nullptr) ? cam_->fpsMilli() : 0));
  }

  // Demo door: the USB CLI qr verb with no link involved.
  void showQrDemo() {
    if (disp_ != nullptr) {
      disp_->showQrDemo();
    }
  }

  void clearQr() {
    if (disp_ != nullptr) {
      disp_->clearQr("cli");
    }
  }

  bool qrActive() const { return disp_ != nullptr && disp_->qrActive(); }

  CameraState camState() const { return camState_; }
  DisplayState oledState() const { return oledState_; }
  uint8_t faceId() const { return faceId_; }

  void setFaceId(uint8_t id) { faceId_ = id; }

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
  FaceRenderer* disp_;
  CameraState camState_;
  DisplayState oledState_;
  uint8_t faceId_;
};

#endif  // ARDUINO
