#pragma once
// desky-head power manager — header-only, REAL rails matrix (not a stub).
//
// Camera+display are power-hungry: camera {ON,OFF} + display {ON,DIM,OFF}.
// Narrowed rails owner (Task 6): the ONE command table lives in
// behavior/coordinator.h (HeadCoordinator::dispatch, the SINGLE dispatch
// point both doors call) and routes CAM/OLED here; FACE/QR route to the
// FaceRenderer there. Boot applies HEAD_BOOT_LEAN (0 = validation:
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

// ── Arduino-free command-value table (host-testable) ───────────────────────
// The pure value mapping PowerManager::apply() enforces below. apply() calls
// these so firmware and host tests share one definition (behavior-identical
// by construction): OLED accepts exactly the DisplayState range 0..2,
// CAM is a nonzero gate, FACE clamps downstream (never blanks on bad input).
namespace headapply {

// OLED arg must be a DisplayState value (0..2); false = apply LOG_Ws and
// keeps state.
inline bool resolveOled(int32_t arg, DisplayState& out) {
  if (arg < static_cast<int32_t>(OLED_OFF) || arg > static_cast<int32_t>(OLED_ON)) {
    return false;
  }
  out = static_cast<DisplayState>(arg);
  return true;
}

// CAM arg is a boolean gate: nonzero -> on, zero -> off.
inline CameraState resolveCam(int32_t arg) { return (arg != 0) ? CAM_ON : CAM_OFF; }

// FACE arg clamps downstream: out-of-range -> BOOT.
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

  // Rails entry: CAM/OLED only. FACE/QR route through
  // HeadCoordinator::dispatch() (behavior layer), which calls back here for
  // these two rails. Anything else (STATUS/HELP/LINK/SNAP/OTA/INVALID, or a
  // FACE/QR that bypassed the coordinator) never changes rail state.
  // Guarded, not silent.
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
        // STATUS/HELP/LINK/SNAP/OTA are answered by the CLI door itself (LINK
        // prints the WiFi status line, SNAP queues a JPEG still, OTA starts
        // the pull updater); INVALID never reaches apply (doors reject at
        // parse). FACE/QR route through the coordinator, never here.
        // Guarded, not silent.
        LOG_W("PWR", "apply ignored (no state change)");
        break;
    }
  }

  void status() const {
    const uint32_t frames = (cam_ != nullptr) ? cam_->frames() : 0;
    const uint32_t dropped = (cam_ != nullptr) ? cam_->dropped() : 0;
    const size_t lastBytes = (cam_ != nullptr) ? cam_->lastSizeBytes() : 0;
    LOG_I("PWR", "cam=%s oled=%s face=%s qr=%s pair=%s frames=%lu drops=%lu last=%uB fps=%lum", camStateName(camState_),
          oledStateName(oledState_), faceName(faceId_), qrActive() ? "on" : "off", pairActive() ? "on" : "off",
          (unsigned long)frames, (unsigned long)dropped, (unsigned)lastBytes,
          (unsigned long)((cam_ != nullptr) ? cam_->fpsMilli() : 0));
  }

  // Bench demo door: the USB CLI `qr` verb paints the deterministic demo
  // pattern (FaceRenderer::showQrDemo) with no link involved. (Historical:
  // the LinkClient handed the 63B wire bitmap through a showQr() wrapper
  // here; the flag-day removal dropped that trigger and the dead wrapper
  // with it — the demo path below is untouched.)
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

  // Pairing handoff (Pre-2): the LinkClient hands the authoritative PIN +
  // validity window here; the renderer re-derives the bitmap locally and
  // paints under its own present/enabled/dim gates. No business logic beyond
  // forwarding + rival-overlay hiding.
  void showPairing(const uint8_t pin6[6], uint16_t validSecs) {
    if (disp_ != nullptr) {
      disp_->showPairing(pin6, validSecs);
    }
  }

  void clearPairing(const char* why = "cli") {
    if (disp_ != nullptr) {
      disp_->clearPairing(why);
    }
  }

  bool pairActive() const { return disp_ != nullptr && disp_->pairActive(); }

  CameraState camState() const { return camState_; }
  DisplayState oledState() const { return oledState_; }
  uint8_t faceId() const { return faceId_; }

  // Coordinator FACE-branch handoff (behavior layer owns the FACE dispatch;
  // the stored id stays here so boot/status read one source).
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
